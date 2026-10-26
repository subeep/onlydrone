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
fixture is included in the tests. Live decoding and the UI are still pending.

## Build

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Core tools need CMake and a C++17 compiler. The decoder requires `pkg-config` and
FFTW3f; the bundled turbo decoder requires x86 SSE. Enable
`-DDRONEID_WITH_UHD=ON` for radio capture (UHD and Boost required). UI, SQLite and
GoogleTest switches are placeholders; tests currently use the bundled harness.

## Try it (no radio)

```bash
./build/iq_gen /tmp/tone.iq --rate 15360000 --tone 100000 --secs 0.02 --format sc16
./build/iq_replay /tmp/tone.iq        # reads the .meta sidecar for format/rate
./build/droneid --config config/x310_1gbe_nb.toml
./build/droneid_decode tests/data/mini2_centered.sc16 --rate 15360000 --format sc16
```

The decoder expects a centered signal at an OFDM-compatible sample rate, normally
15.36 MSPS. It resolves residual integer-carrier offsets within ±240 kHz of the CP
estimate by jointly matching both ZC symbols. Automatic wideband centering and
50-MSPS resampling remain pending. `--freq-offset` performs an explicit frequency
shift before detection. Telemetry field boundaries and phone coordinates are
checked against the reference; altitude, speed and yaw units still need flight validation.

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
