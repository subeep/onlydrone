# DJI DroneID Passive Receiver — Build Plan

**Target hardware (verified 2026-10-26):** USRP X310, serial `326CF02`, 2× UBX-160 v2, HG FPGA
(v38, FW 6.0, RFNoC + DRAM Replay). Host UHD 4.1.0.5. Reachable at `addr=192.168.10.2`.

**Design constraint: build 1GbE-first.** Single-antenna decode is the baseline; **two-antenna direction
finding is a core feature, not optional.** Wideband is a runtime switch enabled on 10GbE. Direction
finding uses **both UBX-160 boards coherently** (Radio#0 + Radio#1, shared LO/time); on 1GbE it runs in
`sc8` (fits the link), on 10GbE in `sc16`.

**Licensing: GPL.** The project is GPL, so the agent may directly adapt the GPL reference
implementations (proto17, gr-droneid, RUB SysSec, antsdr). Each ported file's header must cite its
upstream source and carry the GPL notice.

**This is a passive receiver only.** It demodulates an existing DJI broadcast (DroneID). It never
transmits, never touches a drone's control/video link, and never attempts to break O4 payload
encryption. The frame *generator* in `tools/` produces synthetic IQ for unit tests and is never fed
to a transmitter.

---

## 0. Ground truth to establish before trusting any spec

Treat every protocol constant below as a **hypothesis to confirm against a real capture**, not gospel.
The O4 "0xAA/0x87 frame" and "4-byte session hashcode" claims in particular are **unverified** — do not
hard-code them until Phase 1 capture confirms them.

Public references the agent should read and cite in code comments:
- RUB-SysSec DroneID (NDSS 2023) — format, scrambling, turbo coding, CRC.
- `proto17/dji_droneid` and `gr-droneid` — working demod reference (MATLAB/GNU Radio).
- `antsdr_dji_droneid` — embedded C reference.
These are **GPL**, and **this project is GPL** (§9 resolved) — adapt their demod/FEC/detector source
directly rather than reimplementing from scratch; cite upstream and keep the GPL header in each ported file.

---

## 1. Hardware envelope (measured, not assumed)

| Resource | Value | Consequence for design |
|---|---|---|
| 1GbE sustained | ~25 MSPS `sc16`, ~45 MSPS `sc8` | One 15.36 MSPS channel fits `sc16`. No full-band. |
| 10GbE sustained | ~200 MSPS `sc16` | Full 2.4 band (92.16) or 5.8 slice (184.32) possible. |
| DroneID channel rate | **15.36 MSPS** | = 184.32e6 / 12 → clean DDC decim on X310. |
| UBX analog BW | 160 MHz | Enough for any single-slice capture. |
| Master clock rate | **184.32 MHz** (set explicitly) | Integer relationship to 15.36; avoids fractional resampling. |
| RX channels | 2 (Radio#0, Radio#1) | Decode baseline = Radio#0. **DF uses both, coherently** (core feature). |
| DRAM Replay | present | Lossless burst capture / deterministic replay for tests. |

**Throughput math the code must respect (bytes/s = Msps × bytes/sample):**
- 15.36 MSPS × 4 (`sc16`) = 61.4 MB/s → OK on 1GbE.
- 2× 15.36 MSPS × 4 = 123 MB/s → **over 1GbE**; use `sc8` (61 MB/s) for DF on 1GbE.
- 92.16 MSPS × 4 = 369 MB/s → 10GbE only.

**Host prerequisites (document in README, check at startup):**
- `MTU 9000` on the SDR NIC (currently 1500 — overflows at high rate).
- `net.core.rmem_max`/`wmem_max` ≥ 50e6; UHD `recv_frame_size`, `num_recv_frames` tuned.
- CPU governor = performance; NIC IRQ affinity pinned; `send_buff_size`/`recv_buff_size` in device args.
- `ref_locked` sensor polled when GPSDO/external ref selected.

---

## 2. DroneID signal model (confirm in Phase 1)

OFDM burst, LTE-like:
- Sample rate 15.36 MSPS, FFT size **1024**, subcarrier spacing 15 kHz, ~600 occupied subcarriers.
- **8 or 9 OFDM symbols** per burst (version-dependent — detect, don't assume).
- Two **Zadoff-Chu** sync symbols, roots **600 and 147** → use for detection + fine timing + CFO.
- Modulation **QPSK**; descrambled with a fixed known sequence (NOT encryption); LTE turbo code; **CRC-24**.
- Cyclic prefix: LTE normal CP pattern (first symbol longer) — handle both CP lengths.

Decoded payload (OcuSync 2 / O3 / O3+) fields to parse: version, sequence, state flags, serial
number (string), drone lat/lon/alt, speed (E/N/U), yaw, GPS timestamp, pilot lat/lon, home lat/lon,
drone type. Units: coordinates are scaled integers (×1e7 style — confirm), angles/speeds scaled —
verify against Mini 2 ground truth.

**Generation coverage expectation (from prior analysis):**
- O2 / O3 / O3+ → full decode expected (Mini 2 is the validation drone).
- O1 → unknown; may differ. Leave a decoder hook, don't block the build on it.
- Wi-Fi DJI (Spark/Mavic Air/Mini SE) → DroneID in Wi-Fi beacons; separate cheap decoder (§6 `wifi/`).
- O4 (Air 3S) → detect + fingerprint + rough range; **payload decode not expected**.

---

## 3. Architecture

```
            ┌─────────────────────────────── config (TOML/JSON) ───────────────────────────────┐
            │  mode: narrowband|wideband|df   wire_fmt: sc16|sc8   band plan   dwell schedule    │
            └───────────────────────────────────────────────────────────────────────────────────┘
 UHD RX  ─►  ring buffer  ─►  channelizer  ─►  burst detector  ─►  OFDM demod  ─►  FEC  ─►  parser ─►  sinks
(X310,       (lock-free,      (NB: passthru   (ZC xcorr +         (CP strip,     (descr,   (struct)  (JSON/
 Radio#0)     SPSC, sc16/8)    WB: PFB split)   power gate)        FFT, chan eq)  turbo,              SQLite/
                                                                                 CRC24)               map/GPX)
      │                                                                                                │
      └──────────────► O4 path: burst classifier + RSSI/SNR/CFO + (df mode) 2-ch AoA ──────────────────┘
```

Threading: dedicated RX thread (never blocks), bounded SPSC ring to DSP thread pool, sinks on their
own thread. Backpressure policy = drop-oldest with a logged counter (never stall the RX thread).

---

## 4. Repository layout

```
punjdrone/
├── CMakeLists.txt
├── DRONEID_BUILD_PLAN.md        # this file
├── README.md                    # host setup, MTU/sysctl, run examples
├── config/
│   ├── x310_1gbe_nb.toml        # default: narrowband, single ant, sc16
│   ├── x310_10gbe_wb.toml       # wideband 2.4 band
│   └── x310_df.toml             # dual-channel DF (sc8 on 1GbE)
├── include/droneid/             # public headers (one per module)
├── src/
│   ├── sdr/         usrp_source.{h,cpp}      # UHD setup, timed tuning, channel cfg
│   ├── core/        ring_buffer.h, types.h, logging.h, config.{h,cpp}
│   ├── dsp/         channelizer.*, detector.*, ofdm.*, cfo.*, equalizer.*
│   ├── fec/         scrambler.*, turbo.*, rate_match.*, crc24.*
│   ├── proto/       payload.*, fields.*, units.*
│   ├── o4/          classifier.*, rf_metrics.*, aoa.*
│   ├── wifi/        beacon_parser.*          # Wi-Fi DroneID (Phase 6)
│   ├── out/         json_sink.*, sqlite_sink.*, track_store.*, gpx.*
│   ├── ui/          app_window.*, panels.*, compass.*, model.*, control.*  # §11 native Dear ImGui UI
│   └── app/         main.cpp, cli.cpp
├── third_party/     # vendored Dear ImGui + ImPlot + GLFW glue (all MIT/zlib, GPL-compatible)
├── progress.md      # checklist state, Markdown task list (UI reads/writes; agent updates as work lands)
├── tools/
│   ├── frame_gen.*      # synthetic DroneID IQ for tests (NOT transmitted)
│   ├── iq_record.*      # thin wrapper: capture to file w/ metadata sidecar
│   └── iq_replay.*      # feed file through the pipeline (no radio)
├── tests/              # GoogleTest: per-module + end-to-end on recorded IQ
└── data/               # sample IQ (gitignored if large; checksums tracked)
```

---

## 5. Key interfaces (define these first, implement behind them)

```cpp
// core/types.h
using cf32 = std::complex<float>;
struct Burst { int64_t t_ns; double cfo_hz; float snr_db; float rssi_dbfs;
               std::vector<cf32> samples; uint32_t channel_hz; };

// sdr/usrp_source.h  — abstracts live radio vs. file replay behind one interface
class IqSource {
public:
  virtual size_t recv(cf32* buf, size_t n, int64_t& t_ns) = 0;   // returns samples read
  virtual double sample_rate() const = 0;
  virtual ~IqSource() = default;
};
class UsrpSource : public IqSource { /* UHD multi_usrp, addr=192.168.10.2 */ };
class FileSource : public IqSource { /* replay .iq + sidecar for tests  */ };

// dsp/detector.h
class BurstDetector {                 // ZC cross-correlation + power gate
public:
  std::optional<Burst> feed(std::span<const cf32>);   // emits on sync found
};

// fec/turbo.h
struct DecodeResult { std::vector<uint8_t> bytes; bool crc_ok; };
DecodeResult decode_payload(std::span<const float> soft_bits);  // descr+turbo+crc24

// o4/aoa.h  — direction finding, core feature (both UBX coherent)
struct Bearing { float az_deg; float quality; bool ambiguous_front_back; };
class DirectionFinder {               // phase-difference of arrival across 2 coherent channels
public:
  void set_calibration(const PhaseCal&);          // from splitter / known-source cal step
  Bearing estimate(const Burst& ch0, const Burst& ch1);   // same burst, two antennas
};

// proto/payload.h
struct DroneIdFrame { std::string serial, type; double drone_lat, drone_lon, drone_alt;
                      double pilot_lat, pilot_lon, home_lat, home_lon;
                      float speed_e, speed_n, speed_u, yaw; uint64_t gps_time_ms;
                      uint8_t version; bool pilot_valid, home_valid; };
std::optional<DroneIdFrame> parse(std::span<const uint8_t> payload);
```

Design rule: **the decode pipeline consumes `IqSource`**, so every test runs file-in without a radio,
and live mode is the same code with `UsrpSource`.

```cpp
// ui/model.h  — modules push typed events into a thread-safe UiModel the render thread reads each frame (§11)
struct UsrpStatus { enum State { Disconnected, Connecting, Connected, Error } state;
                    std::string serial, addr, err; double rate_sps, center_hz, gain_db;
                    bool ref_locked; uint64_t overflow_count; };
struct DroneSeen  { std::string serial, type, generation; float rssi_dbfs, snr_db;
                    int64_t first_ns, last_ns; std::optional<float> bearing_deg; };
// DroneIdFrame (decoded packet) and Bearing are reused as-is.
class EventSink {               // every module emits through this; UI + json/sqlite sinks subscribe
public:
  virtual void on_status(const UsrpStatus&) = 0;
  virtual void on_drone(const DroneSeen&)   = 0;
  virtual void on_packet(const DroneIdFrame&) = 0;
  virtual void on_bearing(const std::string& serial, const Bearing&) = 0;
};
// ui/control.h — UI buttons post commands to the SDR control thread; never block the render thread
class Control { public: virtual void connect() = 0; virtual void disconnect() = 0;
                        virtual void reconnect() = 0; };   // enqueues; acts on UsrpSource lifecycle
```

---

## 6. Phased delivery — each phase ends with a runnable binary + passing tests

### Phase 0 — Scaffold (no DSP)
- CMake, deps (UHD, FFTW3, nlohmann/json, SQLite3, GoogleTest, Dear ImGui + ImPlot + GLFW [UI];
  liquid-dsp optional).
- `IqSource` interface, `FileSource`, ring buffer, config loader, logging.
- `iq_record`/`iq_replay` wrappers.
- **Accept:** `iq_replay` streams a file through a no-op pipeline; `ctest` green.

### Phase 1 — Capture & characterize (uses your Mini 2 + Air 3S)  ⟵ START HERE after scaffold
- `UsrpSource`: `master_clock_rate=184.32e6`, DDC to 15.36 MSPS, `addr=192.168.10.2`, Radio#0 only.
- Record scripts for 2.4 GHz and 5.8 GHz DroneID center frequencies while each drone is **in flight**
  (DroneID may only transmit with motors armed / controller linked).
- Analysis notebook/tool: burst finder (energy), measure bandwidth, symbol count, CP pattern, burst
  period, channel occupancy. **Confirm §2 constants.** Produce a written capture report in `data/`.
- **Also record a two-channel coherent capture** (both UBX, shared time/LO, splitter on one source) to
  establish the DF phase-calibration baseline used in Phase 7.
- **Accept:** Mini 2 bursts isolated and parameters match §2 (or §2 updated with evidence); Air 3S
  bursts characterized (what's readable vs. not); coherent two-channel capture shows a stable,
  measurable inter-channel phase.

### Phase 2 — Offline decoder (no live radio)
- `frame_gen` synthetic bursts → detector → OFDM demod (CP strip, 1024 FFT, ZC timing/CFO, equalize,
  QPSK soft-demap) → `fec/` (descramble, rate de-match, turbo max-log-MAP, CRC-24) → `proto/parse`.
- Validate against: (a) synthetic frames (known payload in == out), (b) public reference IQ,
  (c) your Phase 1 Mini 2 recording. Compare decoded lat/lon to DJI app ground truth.
- **Accept:** CRC-valid decode of Mini 2 recording; fields match app within tolerance.

### Phase 3 — Live narrowband (1GbE target)
- Live `UsrpSource` on one DroneID channel at 15.36 MSPS `sc16`, single antenna.
- Overflow handling, resync, soak test.
- **UI shell (§11):** USRP bar with working Connect/Disconnect/Reconnect + live status (rate, center,
  ref_locked, overflow count); empty drones/packets/direction panels; checklist panel reading `progress.md`.
- **Accept:** live decode of Mini 2 at 15.36 MSPS with zero overflows over 10 min on 1GbE rate budget;
  UI connects/disconnects the radio and shows live status + decoded packets.

### Phase 4 — Channel dwell/hop (1GbE coverage strategy)
- Scheduler dwelling across the known DroneID channel set; dwell time derived from measured burst
  period (Phase 1) so Pr(catch) is quantified. Timed tuning via UHD command time.
- **Accept:** detects Mini 2 regardless of which DroneID channel it uses; report shows catch rate.

### Phase 5 — O4 path (Air 3S)
- Burst classifier (bandwidth/period/CFO signature) → detect + count drones.
- `rf_metrics`: RSSI (rough range, "closer/farther" only), SNR, CFO per source, session fingerprint
  (only fields Phase 1 actually verified).
- **DF-enabled output:** when running in `df` mode, attach a `Bearing` to each detected source
  (decodable or not — O4 bursts get a direction even without payload decode).
- **Accept:** reliably flags Air 3S present, separates it from Wi-Fi, logs metrics and bearing. No false
  claim of absolute position.

### Phase 6 — Wideband (10GbE) + Wi-Fi DroneID + outputs
- `channelizer` PFB path for full 2.4 band at 92.16 MSPS on 10GbE (config switch, same pipeline).
- `wifi/beacon_parser` for legacy Wi-Fi DJI drones.
- `out/`: JSON lines, SQLite, per-serial track store, GPX export, optional local web map; alerts
  ("new serial", "within X m by RSSI heuristic").
- **Accept:** wideband catches bursts across the band without dwell gaps; Wi-Fi drone decoded; map +
  logs populated.

### Phase 7 — Direction finding (CORE feature, both antennas)
- Dual coherent RX: both UBX on one shared time/LO, tuned at the same command time, phase reset at
  start. `DirectionFinder` computes phase-difference-of-arrival AoA per burst.
- **Calibration is mandatory** (the two UBX LOs are independent): splitter or known-source step →
  `PhaseCal`, loaded from `config/x310_df.toml`. Store cal with a timestamp; re-run if LO/temp drifts.
- Works for **both decodable drones (O2/O3) and O4** — bearing comes from the raw burst, so the Air 3S
  gets a direction even when its payload can't be decoded.
- Wire format: `sc8` on 1GbE (two channels fit), `sc16` on 10GbE.
- Known limits to document: one antenna pair → front/back ambiguity; several-to-10°+ error near
  buildings; absolute position still needs multiple receivers (out of scope here).
- **Accept:** stable, repeatable bearing to Mini 2 and Air 3S after calibration, with a measured error
  envelope written to the capture report.

---

## 7. UHD specifics for THIS device (bake into `UsrpSource`)

- Device args: `addr=192.168.10.2, master_clock_rate=184.32e6, recv_frame_size=8000,
  num_recv_frames=1024` (raise frames on 10GbE; `recv_frame_size` ≤ MTU-dependent — needs jumbo for 8000).
- **VERIFIED 2026-10-26:** `master_clock_rate` must be in the args to `make()`; calling
  `set_master_clock_rate()` afterward is silently ignored on the X310 (stays at 200 MHz → 15.3846 MSPS
  via odd ÷13 decimation). With it in args, `get_rx_rate()` == 15.36e6 exactly, 0 overflows at MTU 1500.
- Request rate 15.36e6 → DDC decim 12 (integer, no CORDIC fractional). Verify `get_rx_rate()` == 15.36e6.
- `set_rx_bandwidth` to ~10 MHz for narrowband to cut adjacent energy.
- Wire format: `otw_format="sc16"` default; expose `sc8` for DF-on-1GbE and wideband headroom.
- For DF/coherent: shared clock+time, `set_command_time`, tune both radios at the same command time,
  reset phase; the two UBXs are independent LOs → **calibration is mandatory**, document it.
- Poll `ref_locked` if external/gpsdo clock chosen; fail fast if unlocked.

---

## 8. Test strategy

- **Unit:** scrambler round-trip, CRC-24 vectors, turbo decode of known codewords, ZC correlation peak
  position, unit-scaling of fields.
- **Golden-file end-to-end:** checked-in short IQ recordings → expected decoded JSON (exact match).
- **Property:** `frame_gen` with random payloads + AWGN sweep → decode succeeds above an SNR threshold;
  track the threshold as a regression metric.
- **Soak:** long live run, assert zero unhandled overflows and bounded memory.
- CI runs everything except live-radio phases (those are documented manual bring-up checklists).

---

## 9. Licensing — RESOLVED: GPL

The project is **GPL**. The reference implementations (`proto17/dji_droneid`, `gr-droneid`, RUB SysSec,
antsdr) are GPL, so the agent **may port/adapt their turbo/scrambler/detector logic directly** — the
fastest path and lowest risk on the fiddly FEC. Requirements:
- Ship a top-level `LICENSE` (GPLv3 unless an upstream forces GPLv2) and `COPYING`.
- Every ported file header cites its upstream source + original copyright + GPL notice; record provenance
  in `THIRD_PARTY.md`.
- Keep any GPL-incompatible deps out (that's why FEC is in-tree, not srsRAN/AGPL).

## 10. Legal / privacy guardrails (enforce in code & docs)

- Passive receive only; no TX anywhere in the shipped binary. `frame_gen` output is file-only.
- Pilot/home coordinates are personal data: store locally, support a "metrics-only" mode that omits
  pilot location, and document that this is for defensive awareness / reporting to authorities, not
  confrontation.
- README states the user is responsible for local legality of receiving/decoding these transmissions.

---

## 11. Operator UI (native C++ desktop app)

> **Status (2026-10-26): implemented as `droneid_ui`** (`-DDRONEID_WITH_UI=ON`), see `docs/GUI.md`.
> Shell, all five panels, checklist in-place edit, worker/render threading, and offline `--replay`
> decoding are working (10/10 CTest, real-frame render verified). Still open: wiring Connect/Disconnect
> to `UsrpSource` with a live streaming-decode loop, RF-metric/DF panels, and ImPlot. Spec below stands.

A **native desktop GUI in the same process** as the pipeline — no browser, no web server, no network. The
UI runs on its own render thread at ~30–60 fps, reading a thread-safe snapshot of state; the DSP runs on
worker threads. Closing the window does not stop capture (headless keeps running) and vice-versa.

**Tech:** **Dear ImGui** (immediate-mode, MIT) with a **GLFW + OpenGL3** backend, plus **ImPlot** (MIT)
for the compass/polar view — all GPL-compatible, vendored in `third_party/`. Immediate-mode suits a
live-updating instrument panel: each frame re-renders from current state, so there's no widget-state sync
to get wrong. ImGui docking branch gives the resizable panel layout below; the layout persists to
`imgui.ini`.

### In-process wiring (no sockets)

- Pipeline modules emit through `EventSink` into a **`UiModel`**: a mutex-guarded (or double-buffered)
  struct holding current `UsrpStatus`, the drone table, a ring buffer of recent packets, and bearings.
- The render thread copies/locks the model once per frame and draws — it **never** runs DSP or blocks on
  the radio.
- Buttons call `Control`, which **enqueues** a command for the SDR control thread (connect/disconnect/
  reconnect can take time; the UI stays responsive and reflects the new `UsrpStatus` next frame).

### Layout (as specified)

```
┌──────────────────────────────────────────────────────────────────────┬───────────────┐
│  USRP BAR  [ Connect ] [ Disconnect ] [ Reconnect ]   ● status        │  CHECKLIST    │
│  serial 326CF02 · 192.168.10.2 · 15.36 MSPS · 2.4595 GHz · gain 20 dB  │  (live work   │
│  · ref_locked ✓ · overflows 0                                          │   progress)   │
├───────────────────────────────────────────────────┬──────────────────┤               │
│  DRONES DETECTED                                   │   DIRECTION       │  Phase 0 ▸    │
│  ┌──────────────────────────────────────────────┐ │   (polar /        │  ☑ scaffold   │
│  │ serial │ type │ gen │ RSSI │ SNR │ bearing   │ │    compass)       │  ☑ IqSource   │
│  │ ...    │ Mini2│ O2  │ -61  │ 18  │  37° ◂    │ │    one needle     │  Phase 1 ▸    │
│  └──────────────────────────────────────────────┘ │    per source,    │  ☐ capture    │
├───────────────────────────────────────────────────┤    colour-keyed   │  ☐ Mini2 chars│
│  DECODED PACKETS (live, newest on top)             │    to the drone   │  ...          │
│  time · serial · lat/lon/alt · pilot · home · CRC  │    row            │  [+ add item] │
│  12:04:03 · 326.. · 30.71,76.72 220m · pilot ✓ ... │                   │               │
└───────────────────────────────────────────────────┴──────────────────┴───────────────┘
```

- **USRP bar (top, full width):** Connect / Disconnect / Reconnect buttons; a status dot
  (grey disconnected, amber connecting, green connected, red error). Shows serial, addr, **sample rate**,
  **center frequency** (and band/channel in dwell mode), gain, `ref_locked`, and a live overflow counter.
  Buttons call `Control` (enqueued to the SDR thread); the bar re-renders from the next `UsrpStatus` snapshot.
- **Drones detected (middle-left):** one row per unique serial/source — type, generation, RSSI, SNR,
  first/last seen, and its current bearing. O4 sources appear here even when undecoded. Clicking a row
  highlights its needle in the Direction panel and filters the packet log.
- **Decoded packets (bottom-left):** live scroll of CRC-valid `DroneIdFrame`s — timestamp, serial,
  drone lat/lon/alt, pilot, home, CRC flag. "Metrics-only" toggle hides pilot/home (privacy, §10).
- **Direction (right, spans both left rows):** polar/compass view, one needle per active source,
  colour-matched to its drone row; length/opacity = bearing quality; marks front/back ambiguity. Empty
  with a "DF off — single antenna" note when not in `df` mode.
- **Checklist (far-right side panel):** the live work tracker you asked for — see below.

### Checklist panel (persistent, constantly updated)

- Backed by **`progress.md`** at repo root: a plain Markdown task list, grouped under `##` phase headings
  (the §6 phases), one GitHub-style checkbox per acceptance item. States:
  `- [ ]` todo, `- [/]` in progress, `- [x]` done; an optional `— note` tail after an item is free text.
- The app loads it on start and **watches its mtime**, reloading when it changes on disk; it parses the
  task list and renders collapsible phase groups with ImGui checkboxes. Toggling a box (or editing a note)
  **rewrites `progress.md` in place**, preserving ordering, headings, and any prose between sections.
- **The coding agent edits the same Markdown file as work lands**, so the panel always reflects real
  state — it's the shared, human-readable, git-diffable source of truth, not a separate to-do list.
- Seed it from §6 on first run (a small generator turns the phase/acceptance list into `progress.md`).
- Parser rule: only lines matching `^\s*- \[[ x/]\] ` are checklist items; everything else is passed
  through untouched on rewrite, so the file stays a normal readable Markdown doc.

### Threading rules (enforce)

- The render thread only reads the `UiModel` snapshot and submits ImGui draw calls — **no DSP, no UHD
  calls, no blocking** on it.
- Module→UI updates go through the bounded model (drop-oldest for the packet ring) so a slow frame never
  stalls the pipeline. The UI is one `EventSink` among the json/sqlite sinks — **run headless with
  `--no-ui`** and the pipeline is identical.
- `Control` commands are enqueued and executed on the SDR control thread; the UI shows `Connecting…`
  state meanwhile.
- GL context + window live on the thread that created them (GLFW requirement); keep all ImGui calls there.

### UI build order (folds into §6)

- **Phase 3:** ship the UI shell — USRP bar (connect/disconnect/reconnect + live status) and the empty
  panels. This is the first time there's a live radio to drive.
- **Phase 2→3:** decoded-packets panel lights up from the decoder.
- **Phase 5:** drones-detected panel (incl. O4) populates.
- **Phase 7:** Direction panel goes live.
- Checklist panel works from Phase 0 (it only needs `progress.md`).

## 12. First actions for the coding agent

1. Phase 0 scaffold (CMake + `IqSource`/`FileSource` + ring buffer + replay tool).
2. Phase 1 `UsrpSource` (Radio#0, 184.32e6 clock, 15.36 MSPS) + capture scripts; **record Mini 2 and
   Air 3S first** — the captures drive every constant in §2.
3. Only then build the Phase 2 decoder against those recordings.

Do not hard-code O4 frame internals or any §2 constant until a Phase 1 capture confirms it.
