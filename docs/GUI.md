# Operator UI — verification and use

Native Dear ImGui + GLFW/OpenGL3. Build with `DRONEID_WITH_UI=ON`; UHD is optional for offline replay.
The checklist panel remains removed as requested. The checklist parser remains independently tested.

## Architecture

- `src/ui/ui_model.{h,cpp}`: mutex-protected snapshots, bounded packet log, per-serial track counts.
- `src/ui/receiver_worker.{h,cpp}`: replay and live workers shared by GUI and headless tests.
- `src/ui/app.cpp`: rendering, argument validation and asynchronous stop/reconnect requests.
- Replay reads bounded blocks using the sidecar's actual rate. Overlapping decode windows retain
  boundary bursts; only the owning window publishes each result. Cancellation is checked between windows.
- Live RX and DSP use separate threads with a three-window queue. Oldest windows are dropped under
  overload, with visible counters; timestamp gaps/overflows reset partial windows. This prevents DSP
  from directly blocking RX, but does not establish lossless real-time throughput on hardware.
- RF metrics are aggregate **unknown RF**, not O4 identification, unique drone counts or valid bearings.
- A separate experimental O4-like structure line reports candidate windows from the
  shared detector, without creating identities or positions. See `O4_DETECTOR.md`.
- CRC-valid unsupported payloads remain in the packet log with raw hex, without invented serials,
  GPS or tracks. Hover a serial/unparsed cell to inspect the payload.

## Build and replay

```bash
cmake -S . -B build -G Ninja -DDRONEID_WITH_UI=ON -DDRONEID_WITH_UHD=ON
cmake --build build
ctest --test-dir build --output-on-failure
./build/droneid_ui --replay data/iq_samples/mini2/mini2_sm.cf32 --format cf32 --rate 50000000 --auto-center
./build/droneid_ui --replay tests/data/mini2_wideband.cf32 --format cf32 --rate 50000000 --auto-center --check
./build/droneid_ui --replay data/iq_samples/mini2/mini2_sm.cf32 --format cf32 --rate 50000000 --auto-center --frames 120 --screenshot /tmp/receiver.ppm
```

`--check` does not open a window. It reports CRC packet rows, tracks, serial counts, payload hex,
received samples, gaps, dropped windows and errors. Replay completion is **Disconnected**, explicitly
not live reception. `--frames N --screenshot FILE.ppm` saves the final framebuffer as P6 PPM; it needs
a display and does not guarantee replay finishes before the frame limit.

Replay options: `--replay FILE --format sc16|cf32 --rate HZ`, `--secs S` (0 = complete file),
`--auto-center` OR `--freq-offset HZ`. Metadata sidecars override file rate/format. For wideband input,
auto-centering may select the wrong active channel; corpus logs record successful explicit shifts.

Connect starts the configured source; Disconnect requests cancellation; Reconnect waits for worker
completion before restarting. Rendering does not join a running worker during these button actions.
Closing the window stops and joins the worker; capture does not continue in the background.

## Live acceptance — transport checked, transmitter tests pending

The connected X310 has now passed a three-second headless run through the current
live worker: 46,080,000 samples, zero overflows, gaps or dropped windows. It reported
ambient RF activity and no decoded packets; there was no known drone transmitter.
The live worker currently uses fixed 15.36 MSPS (the `--rate` option configures replay).
The dual-channel transport profile is separately 11.52 MSPS/sc16. See
`X310_BENCH_20261028.md` for evidence and limitations. Example live commands:

```bash
./build/droneid_ui --device addr=192.168.10.2 --freq 2459500000 --gain 30
./build/droneid_ui --device addr=192.168.10.2 --freq 2459500000 --secs 2 --check
```

Live `--check` measures requested samples after connection, not a timer started before connection;
five consecutive receive timeouts fail. It defaults to two seconds. Validate a known transmitter,
CRC payloads, ten-minute throughput, overflows, gaps, queue drops, reconnect and shutdown before
declaring live acceptance. Physical-button interaction and hardware recovery still need testing.

The native GUI also completed a bounded X310 run with 46,080,000 received samples
and zero gap/drop/overflow counters. Rendering correctly showed Disconnected after
the run and no active bearings. Direction status text now wraps and the cancel
button has a separate row to avoid clipping in the default 380-pixel panel.

## Open Drone ID mode (`--odid`, opt-in)

`--odid` switches the source to the ASTM F3411 Wi-Fi decoder (Beacon + NAN; `docs/OPEN_DRONE_ID.md`).
Without it, nothing changes. Replay needs ≥ 20 MSPS (default `--rate 20e6`; `.meta` overrides).
Live uses a 200 MHz master clock, 20 MSPS and `--freq` (default 2437 MHz, channel 6). `--odid`
rejects DF, paired replay, `--auto-center` and rates below 20 MSPS. `--freq-offset` is allowed.

```bash
./build/droneid_ui --odid --replay capture.iq --format sc16 [--check]
./build/droneid_ui --odid --device addr=192.168.10.2 --freq 2437e6 --gain 20
```

Decoded frames (802.11 FCS-valid, F3411-valid) appear in both tables tagged `ODID/ASTM`. In the drones
table, `serial` is the UAS ID (or `mac:…` before a Basic ID arrives), `type` is `ODID/ASTM <UA type>`,
`gen` is `ODID/ASTM wifi_beacon|wifi_nan`, and `corr` is PHY sync quality. In the packet log,
`seq` is the F3411 message counter, `pilot` is the System message's operator location, and `-` marks
a frame without position. Hovering shows the PHY/rate and the Message Pack hex. Track keys are
namespaced (`ODID/ASTM:<id>`), so a drone that also sends DJI DroneID gets a separate track.
`--check` prints `source=ODID/ASTM` on ODID track/packet lines; DJI lines are unchanged. The status
detail shows ODID/FCS/sync counters. DJI decoding and the unknown-RF summary are off in this mode.
Bench-verified 28 October with an ESP32-S3 Remote ID transmitter: replay matches the CLI, native
replay and live windows render the ODID track, and live ran 30 s with zero overflows/gaps/drops
(`data/analysis/odid_20261028/`). The longitude columns are clipped at the default window width.

## Direction finding

Current paired-replay behavior and review limitations: `docs/DF_REVIEW_20261027.md`.
`--pair-manifest FILE` is required for paired GUI bearings. Choose either
`--df-cal FILE` or `--df-cal-samples N --df-ref splitter|ota_broadside`; the prefix
must actually be a reference capture, not arbitrary drone samples. Both modes
use capture-time freshness. `--df-jsonl FILE` retains historical accepted/rejected
events; active arrows clear at replay EOF or cancellation. Live DF stays disabled.

The former GUI path reopened the radio and applied file calibration without verified phase-repeatability
or session provenance. That is not a validated phase reference. `--df --df-cal FILE` now **fails closed without opening a radio**.
Use paired-IQ tools described in `docs/PHASE456.md`; the offline lifecycle is integrated,
but persistent live calibration, hardware alignment and antenna-rig verification remain.
UBX/X310 timed LO synchronization is supported, but calibration reuse must be measured on this rig.
The staged implementation and hardware acceptance gates are in `docs/DIRECTION_FINDING_PLAN.md`.

## Packet diagnostics (2026-10-27)

Packet rows distinguish `v2`, `v3`, `infp`, `cryp`, auxiliary type `0x11` and unknown envelopes.
Hover the serial/format for raw bytes and inner CRC16 status. The row's `CRC24 OK` label refers
only to the outer transport checksum. Supported v2 telemetry requires valid inner CRC16 and
a recognized logical length (79 or 91 bytes); other formats or failed checks never create a track.
The headless worker prints the same diagnostics. CRCs detect corruption, not spoofed identity.
Nine retained real-IQ clips pass exact CLI/UI payload and envelope comparisons; all nine have
valid inner CRC16. This is offline integration evidence, not O4 identification accuracy.

## Verified offline (2026-10-26)

- Release build with UI/UHD enabled; 12/12 CTest suites, including worker and CLI tests.
- Real Mini 2 replay: 10 CRC packets, one track with count 10. Mavic Air 2: one CRC packet.
- Native 1280×800 rendering inspected; status, counts and packet rows populated from replay.
- Synthetic boundary, cancellation, repeated replay, invalid CLI and fail-closed DF regressions.
- Corpus evidence and exact limitations: `docs/VALIDATION_20261026.md` and `data/analysis/`.
