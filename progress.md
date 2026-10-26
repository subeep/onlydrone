# punjdrone — build progress

Live checklist, shared between the operator and the coding agent. The native UI (Phase 3+)
renders this and rewrites it in place; the agent ticks items as work lands. States:
`- [ ]` todo · `- [/]` in progress · `- [x]` done. Lines that aren't checkboxes are left untouched.

See [`DRONEID_BUILD_PLAN.md`](DRONEID_BUILD_PLAN.md) for the detail behind each item.

## Phase 0 — Scaffold
- [x] CMake build (C++17, Ninja), feature toggles for UHD/UI/SQLite/GTest
- [x] Core types + protocol constants (`types.h`)
- [x] Lock-free SPSC ring buffer + tests
- [x] Leveled logger (`logging.h`)
- [x] Config loader (TOML-subset) + tests
- [x] `IqSource` interface + `FileSource` (cf32/sc16/sc8) + tests
- [x] No-op `Pipeline` (sample count + mean power)
- [x] `iq_replay` tool (file -> ring -> pipeline)
- [x] `iq_gen` tool (synthetic IQ for tests; never transmitted)
- [x] App entry (`droneid`) loads config, `--no-ui`/`--version`
- [x] README with host setup (MTU/sysctl) + build/run
- [x] **Accept:** `iq_replay` streams a file through a no-op pipeline; `ctest` green (3/3)

## Phase 1 — Capture & characterize (Mini 2 + Air 3S)
- [x] `UsrpSource` (Radio#0, master_clock_rate 184.32e6 via device args, 15.36 MSPS, addr=192.168.10.2)
- [x] `iq_record` live capture with metadata sidecar
- [x] Capture scripts for 2.4 GHz and 5.8 GHz DroneID center frequencies (`scripts/capture.sh`)
- [x] Burst-analysis tool `burst_scan`: bandwidth, symbol-count est, burst period, occupancy (FFTW)
- [x] Hardware smoke test: 15.36 MSPS exact, 0 overflows; burst_scan found 440 ambient bursts @ 2.4595 GHz
- [x] Capture report scaffold + baseline (`data/CAPTURE_REPORT.md`)
- [x] Record Mini 2 in flight — 2.4 GHz (5 slices) captured, 0 overflows; strong structured bursts seen
- [x] Dwell capture 30 s on 2.4595 GHz (centered) — still 0 bursts; scan peak corr 0.059 (noise)
- [ ] **Capture with GPS lock**: repeat outdoors with a verified fix to validate telemetry.
      GPS gating is NOT established: the public Mini 2 reference carries phone coordinates
      despite having no drone position fix. The cause of our missing local beacon remains unknown.
- [x] Band sweep: `scripts/sweep_droneid.sh` captures every DroneID channel centered + decodes each
      (proven 15.36 path) — catches the beacon on whatever channel the drone uses
- [x] Outdoor GPS-locked 5-channel sweep: still NO beacon (max ZC corr ~0.11 = noise; ref hits 0.9+).
      Our local Mini 2 isn't emitting a detectable legacy DroneID beacon on 2.4 — try 5.8 band, verify
      GPS lock quality, or this unit/firmware/region may differ. Decoder itself is proven on the ref.
- [x] Verified wideband capture streams clean at MTU 1500 (61.44 & 92.16 MSPS, 0 overflows)
- [/] `channelize` tool (wideband DDC): works for presence detection; clean decode through decimation
      needs more work (ISI at symbol boundaries) — sweep script is the reliable decode path for now
- [x] Record Air 3S (O4) in flight — 2.4 (5 slices) + 5.8 (4 slices) captured, 0 overflows
- [x] `spectrogram` tool (STFT -> PNG via FFTW+zlib); 5.8 video link confirmed, 2.4 congested
- [/] DroneID presence: drones clearly present, but beacon not isolable by energy/visual — needs ZC correlator
- [ ] Two-channel coherent capture (both UBX, splitter) for DF phase-cal baseline
- [/] Fill capture report — done for captures; §2 constants NOT yet confirmed (needs ZC detector)
- [ ] **Accept:** Mini 2 bursts isolated matching §2 — BLOCKED on Phase 2 ZC matched filter

## Phase 2 — Offline decoder (no live radio)
- [x] Ported full chain from GPL reference proto17/dji_droneid (+ vendored turbofec, CRCpp)
- [x] `frame_gen` synthetic DroneID bursts (payload in == out)
- [x] Burst detector (ZC xcorr root 600, normalized, FFT overlap-add)
- [x] OFDM demod (CP strip, 1024 FFT, STO via CP, coarse CFO, ZC channel eq, QPSK)
- [x] FEC: LTE Gold descramble, rate de-match, turbo, CRC-24-LTEA
- [x] Payload parser + unit scaling (`proto/frame.cpp`) — field offsets to confirm on a real frame
- [x] `droneid_decode` tool (+ `--scan` freq-offset diagnostic)
- [x] **Validated end-to-end on synthetic**: detect corr 0.99, turbo+CRC OK, serial recovered exactly
- [x] Fixed turbofec SSE requirement (no non-SSE fallback → zeros); 4/4 tests pass
- [x] **Detector validated on REAL DJI data** — RUB-SysSec `mini2_sm` reference (DJI Mini 2, 50 MSPS):
      resampled to 15.36 and detected 10 bursts at corr 0.78–0.96. create_zc, OFDM sync, detection all
      confirmed correct on genuine DroneID.
- [x] **Found: real captures have a large carrier offset** — `mini2_sm` signal sits at +9.67 MHz; must
      locate the ~10 MHz band and CENTER it before resampling/decode (center-at-DC slices the signal).
      Our own field captures had no detected beacon; the cause is still unconfirmed.
- [x] **Real-burst CRC fixed (2026-10-26)**: CP CFO is ambiguous modulo 15 kHz. Joint matching
      of roots 600 and 147 resolves timing and integer-carrier offset (±16 carriers). The saved
      centered `mini2_ref2.iq` had a -57.47 kHz residual CFO, previously mistaken for +2.53 kHz.
      All 10 detected bursts now pass CRC-24, with and without LPF. Fractional timing was not
      the cause of these failures; no timing-search or phase-walking patch was needed.
- [x] Golden real-IQ test (64 kB fixture), positive/negative CFO regressions, and file-chunk tests.
      Fixed whole-file default, skipped chunk-boundary samples, and final sample-limit handling.
      Release CTest: 4/4 passing (7 DJI cases). Clean Debug AddressSanitizer build: 4/4 passing
      with leak detection disabled because LeakSanitizer cannot run under this environment's ptrace.
- [x] Correct v2 telemetry field boundaries, phone coordinate order, and 64-bit GPS timestamp;
      verified serial/sequence/phone location against the real fixture. Motion units remain provisional.
- [ ] Support explicit 8-symbol legacy frames, inner 16-bit CRC, and validate nonzero motion units.
- [ ] Add signal-centering + 50-MSPS-class resample into the decoder (currently a scratch tool).
- [/] **Accept:** saved centered Mini 2 reference yields 10/10 CRC-valid detected bursts, phone GPS
      only. Upstream's README reports 7/9 for `mini2_sm`; its separate 14/18 example is not this
      fixture's acceptance count. Integrated raw-rate preprocessing and a CRC-valid local capture remain.

## Phase 3 — Live narrowband (1GbE) + UI shell
- [ ] Live decode on one channel at 15.36 MSPS sc16, single antenna
- [ ] Overflow handling, resync, soak test
- [ ] UI shell: USRP bar (Connect/Disconnect/Reconnect + live status), empty panels
- [ ] Checklist panel reading/writing `progress.md`
- [ ] Decoded-packets panel live
- [ ] **Accept:** live Mini 2 decode, zero overflows over 10 min on 1GbE budget; UI drives the radio

## Phase 4 — Channel dwell/hop (1GbE coverage)
- [ ] Dwell scheduler across known DroneID channels (timed tuning)
- [ ] Dwell time derived from measured burst period; quantified catch rate
- [ ] **Accept:** detects Mini 2 on any DroneID channel; report shows catch rate

## Phase 5 — O4 path (Air 3S)
- [ ] Burst classifier (bandwidth/period/CFO signature); detect + count drones
- [ ] RF metrics: RSSI (rough range), SNR, CFO, verified session fingerprint
- [ ] Drones-detected panel populated (incl. undecoded O4)
- [ ] **Accept:** reliably flags Air 3S, separates from Wi-Fi, logs metrics; no false position


## Phase 6 — Direction finding (core, both antennas)
- [ ] Dual coherent RX (shared time/LO, same-command-time tune, phase reset)
- [ ] `DirectionFinder` phase-difference AoA per burst
- [ ] Calibration step (splitter / known source) -> `PhaseCal` from `config/x310_df.toml`
- [ ] Bearings for both O2/O3 (decoded) and O4 (raw burst)
- [ ] Direction panel live; sc8 on 1GbE / sc16 on 10GbE
- [ ] **Accept:** repeatable bearing to Mini 2 and Air 3S after cal, with measured error envelope
