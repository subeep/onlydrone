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

## Live acceptance — deferred by operator

**No X310 or transmitting drone is available for this verification run. No radio was contacted.**
Historical two-second smoke-test notes do not prove the revised pipeline's live performance.
Commands for a later supervised hardware session:

```bash
./build/droneid_ui --device addr=192.168.10.2 --freq 2459500000 --gain 30
./build/droneid_ui --device addr=192.168.10.2 --freq 2459500000 --secs 2 --check
```

Live `--check` measures requested samples after connection, not a timer started before connection;
five consecutive receive timeouts fail. It defaults to two seconds. Validate a known transmitter,
CRC payloads, ten-minute throughput, overflows, gaps, queue drops, reconnect and shutdown before
declaring live acceptance. Physical-button interaction and hardware recovery still need testing.

## Direction finding

The former GUI path reopened the radio and applied file calibration from another LO session.
That is not a valid phase reference. `--df --df-cal FILE` now **fails closed without opening a radio**.
Use paired-IQ tools described in `docs/PHASE456.md`; integrated same-session calibration,
timestamp alignment and antenna-rig verification are still required before enabling GUI bearings.

## Verified offline (2026-10-26)

- Release build with UI/UHD enabled; 12/12 CTest suites, including worker and CLI tests.
- Real Mini 2 replay: 10 CRC packets, one track with count 10. Mavic Air 2: one CRC packet.
- Native 1280×800 rendering inspected; status, counts and packet rows populated from replay.
- Synthetic boundary, cancellation, repeated replay, invalid CLI and fail-closed DF regressions.
- Corpus evidence and exact limitations: `docs/VALIDATION_20261026.md` and `data/analysis/`.
