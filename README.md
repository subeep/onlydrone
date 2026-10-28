# punjdrone

Passive **DJI DroneID** receiver for the USRP **X310** (2× UBX-160). Listens to the
DroneID broadcast only — it never transmits and never touches a drone's control or
video link. See [`DRONEID_BUILD_PLAN.md`](DRONEID_BUILD_PLAN.md) for the full design and
[`progress.md`](progress.md) for live build status.

**License:** GPL-3.0-or-later (see §9 of the plan).

## Status

**Phase 2 — offline decoder.** UHD capture, ZC detection, OFDM demodulation,
turbo decoding and CRC-24 validation are implemented. The saved, centered Mini 2
reference capture now yields **10/10 CRC-valid detected bursts**. A short real-IQ
fixture is included in the tests. Headless phase 4–6 tools now provide timed dwell
reception, streaming RF metrics and paired-channel calibration/bearing workflows.
The native UI and offline backend are verified; live hardware acceptance and O4 identification
remain pending. See [`docs/PHASE456.md`](docs/PHASE456.md) for commands and limits, and
[`docs/VALIDATION_20261026.md`](docs/VALIDATION_20261026.md) for local corpus results.

## Build

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Core tools need CMake and a C++17 compiler. The decoder requires `pkg-config` and
FFTW3f; the bundled turbo decoder requires x86 SSE. Enable
`-DDRONEID_WITH_UHD=ON` for radio capture (UHD and Boost required). Enable
`-DDRONEID_WITH_UI=ON` for the native operator UI (Dear ImGui + GLFW/OpenGL3; needs
GLFW and OpenGL). SQLite and GoogleTest switches are placeholders; tests use the bundled harness.

## Operator UI (`droneid_ui`)

Native Dear ImGui app (`-DDRONEID_WITH_UI=ON`): source/USRP bar (Connect/Disconnect/Reconnect
drive the source), drones table, decoded-packets log, and a direction compass. It reads the
decoder backend only; the render thread draws from a thread-safe snapshot while a worker thread
decodes. See [`docs/GUI.md`](docs/GUI.md).

```bash
cmake -S . -B build -G Ninja -DDRONEID_WITH_UI=ON -DDRONEID_WITH_UHD=ON && cmake --build build
# replay source (offline, no radio):
./build/droneid_ui --replay tests/data/mini2_centered.sc16 --format sc16 --rate 15360000
# live radio source (Connect drives UsrpSource; needs the UHD build):
./build/droneid_ui --device addr=192.168.10.2 --freq 2459500000 --gain 30
```

Connect/Disconnect/Reconnect start/stop/restart the chosen source. The live streaming-decode loop is
wired to `UsrpSource` with separate RX/DSP threads and visible gap/drop counters; the revised path
still needs hardware acceptance. The operator deferred live tests because no radio/drone is available.
The drones panel shows aggregate "unknown RF" activity, not O4 identity or a drone count.
CRC-valid unsupported payloads are logged without invented telemetry. GUI `--df --df-cal FILE`
currently fails closed: same-session LO calibration must be integrated before showing bearings.
The [direction-finding completion plan](docs/DIRECTION_FINDING_PLAN.md) defines paired replay,
calibration lifecycle, shared DSP, GUI integration, bench and field acceptance gates.
CLI/UI packet diagnostics now distinguish envelope format and inner CRC16 from outer CRC24;
unsupported formats remain unparsed and neither checksum authenticates identity.

## Try it (no radio)

```bash
./build/iq_gen /tmp/tone.iq --rate 15360000 --tone 100000 --secs 0.02 --format sc16
./build/iq_replay /tmp/tone.iq        # reads the .meta sidecar for format/rate
./build/droneid --config config/x310_1gbe_nb.toml
./build/droneid_decode tests/data/mini2_centered.sc16 --rate 15360000 --format sc16
```

The decoder runs its OFDM chain at 15.36 MSPS. Input rates from 10 to 250 MSPS are converted with a streaming
anti-alias FIR resampler; 50 MSPS real data and 61.44/92.16 MSPS synthetic data are tested.
It searches residual integer-carrier offsets within ±960 kHz of the CP estimate by
jointly matching both ZC symbols. `--freq-offset` shifts a known channel before
resampling. `--auto-center` instead selects the strongest 10 MHz band in the first
1,048,576 input samples and holds that choice for the file. This is useful for an
isolated reference signal; it does not distinguish DroneID from stronger interference
or follow hopping signals. Telemetry field boundaries and phone coordinates are
checked against the reference; altitude, speed and yaw units still need flight validation.

```bash
./build/droneid_decode data/reference/mini2_sm.cf32 --format cf32 --rate 50000000 --auto-center --secs 0
```

See `data/reference/README.md` for the pinned public sample and checksum.
`--secs 0` reads the whole file; positive durations use the input rate, including
sidecar overrides. Reported sample indices are in original input-sample units;
reported CFO is the residual after the chosen frequency shift. `--scan` is a
diagnostic over at most the first 1,048,576 input samples.

## Open Drone ID (ASTM F3411 Remote ID, Wi-Fi Beacon / NAN)

Opt-in add-on: `odid_decode` decodes recorded Wi-Fi IQ (≥ 20 MSPS), `droneid_ui --odid` shows ODID
drones tagged `ODID/ASTM`, and `odid_gen` writes synthetic fixtures. Bench-verified on 28 October with an
ESP32-S3 Remote ID transmitter (beacon + NAN, `data/analysis/odid_20261028/`). See `docs/OPEN_DRONE_ID.md` and `docs/ODID_VERIFICATION_RUNBOOK.md`.

## Host setup for live capture (Phase 1+)

The X310 is verified reachable at `addr=192.168.10.2`. Before high-rate streaming:

- **Jumbo frames:** set the SDR NIC to `MTU 9000` (currently 1500 — will overflow at
  wideband rates).
- **Socket buffers:** `sudo sysctl -w net.core.rmem_max=50000000 net.core.wmem_max=50000000`.
- **CPU governor:** set to `performance`; pin NIC IRQ affinity.
- Confirm the link: `uhd_find_devices`, then `uhd_usrp_probe --args addr=192.168.10.2`.

## Legal / privacy

Receiving and decoding these transmissions may be regulated where you live — you are
responsible for local legality. Pilot/home coordinates are personal data; a
"metrics-only" mode (Phase 5+) omits them. Use for defensive awareness, not confrontation.
