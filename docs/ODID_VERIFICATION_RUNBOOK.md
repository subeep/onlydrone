# Open Drone ID (Wi-Fi Beacon / NAN) — device verification runbook

Use this to verify the F3411 Wi-Fi decoder against the operator's physical Remote ID
transmitter. Everything here is receive-only: the X310 never transmits. Design, gates
and limits are in `docs/OPEN_DRONE_ID.md`.

Rules: record every command's output in the run folder. Report only what the saved logs
show. A step that was skipped, or failed and was not repeated, stays unverified in
`progress.md`.

## 0. Before you start

- Build with UHD and UI, and confirm the suites pass:
  ```bash
  cmake --build build && ctest --test-dir build --output-on-failure
  ```
  `test_odid`, `test_odid_wifi` and `test_odid_cli` must pass. They use synthetic IQ only.
- Fit a 2.4 GHz antenna (dual-band if you will test 5 GHz) on **RX2 of channel 0**.
- From the transmitter's configuration app, note: its transport(s) (Beacon, NAN or both), its
  Wi-Fi channel(s), its MAC address, its UAS ID (serial / CAA registration) and operator ID,
  and whether it has a GPS fix. These are the reference values for the pass criteria.
- If possible, run an independent receiver at the same time (for example the OpenDroneID
  Android receiver app on a phone with NAN/beacon support). Screenshot what it shows.
- Place the transmitter 2–5 m from the antenna for the first captures.

## 1. Create the run folder

```bash
RUN=data/analysis/odid_$(date -u +%Y%m%d)
mkdir -p "$RUN"
{ date -u; git rev-parse HEAD; git status --short; uhd_find_devices; } > "$RUN/provenance.log" 2>&1
```

In `$RUN/notes.md`, record the transmitter model, firmware, the reference values above,
the antenna, the distance and the gain you use.

## 2. Transport check at the ODID profile (200 MHz master clock, 20 MSPS)

The ODID profile (`config/x310_odid_wifi.toml`) uses a different master clock from the
benchmarked 184.32 MHz profiles, so measure it once:

```bash
timeout 90s /usr/lib/uhd/examples/benchmark_rate \
  --args 'addr=192.168.10.2,master_clock_rate=200000000' \
  --rx_rate 20000000 --rx_channels 0 --rx_otw sc16 --duration 60 \
  --overrun-threshold 0 --drop-threshold 0 --seq-threshold 0 \
  > "$RUN/transport_20msps_60s.log" 2>&1; echo "exit $?" >> "$RUN/transport_20msps_60s.log"
```

**Pass:** zero drops, overruns, sequence errors and timeouts, and zero transmitted samples
(640 Mb/s sc16 payload on the 1 GbE link). **On failure:** do not lower the rate below 20 MSPS,
because 802.11 needs 20 MHz. Check the MTU and NIC counters, or try `--rx_otw sc8` only after
it is verified with this UHD build.

## 3. Baseline: transmitter off

```bash
./build/iq_record "$RUN/baseline_ch6.iq" --config config/x310_odid_wifi.toml --secs 2 --store sc16 \
  > "$RUN/baseline_ch6_record.log" 2>&1
./build/odid_decode "$RUN/baseline_ch6.iq" --format sc16 > "$RUN/baseline_ch6_decode.log" 2>&1
```

**Expect:** `odid_frames=0`. `fcs_ok` / `non_odid` above zero are normal: ordinary Wi-Fi
access points are decoding correctly, which is useful evidence that the PHY works on real
signals. Keep this log.

## 4. Capture the transmitter on its channel

Default is channel 6 (2437 MHz), used by NAN and commonly by beacons. Use `--freq` for another
channel: channel 1 is 2412e6, 11 is 2462e6, 36 is 5180e6 and 149 is 5745e6 (NAN 5 GHz).

```bash
./build/iq_record "$RUN/odid_ch6.iq" --config config/x310_odid_wifi.toml --secs 10 --store sc16 \
  > "$RUN/odid_ch6_record.log" 2>&1
./build/odid_decode "$RUN/odid_ch6.iq" --format sc16 --jsonl "$RUN/odid_ch6.jsonl" \
  > "$RUN/odid_ch6_decode.log" 2>&1
tail -3 "$RUN/odid_ch6_decode.log"
```

If the channel is unknown, sweep 3 s per channel and decode each file:

```bash
for f in 2412000000 2437000000 2462000000 5180000000 5745000000; do
  ./build/iq_record "$RUN/sweep_$f.iq" --config config/x310_odid_wifi.toml --freq $f --secs 3 --store sc16 \
    > "$RUN/sweep_${f}_record.log" 2>&1
  ./build/odid_decode "$RUN/sweep_$f.iq" --format sc16 > "$RUN/sweep_${f}_decode.log" 2>&1
  grep summary "$RUN/sweep_${f}_decode.log"
done
```

## 5. Pass criteria for the offline decode

All of these must hold, checked against the reference values from step 0:

1. `odid_frames` ≥ 1, and the `transport=` field matches the configured transport(s).
2. `id=` equals the transmitter's UAS ID, and `mac=` matches its MAC (if the app shows it).
3. With a GPS fix, `lat`/`lon` agree with the phone or independent receiver to within the
   transmitter's stated accuracy. `operator=` matches the configured operator ID.
4. `iq_record` reports zero overflows.

Record the frame rate (frames ÷ seconds) and `fcs_fail`. These are observations, not acceptance.

**Diagnosing a silent capture** (from the `summary:` counters):

| Symptom | Likely cause / action |
|---|---|
| `ofdm_sync=0` and `dsss_sync=0` | Wrong channel, transmitter off, or far too weak: sweep, move closer |
| `ofdm_signal_bad` or `dsss_header_bad` high | Clipping or low SNR: try `--gain 10` and `--gain 30` |
| `fcs_fail` high, `fcs_ok` low | Clipping/interference, or a PHY detail differs from the device: keep the file and report it |
| `fcs_ok` > 0, `non_odid` only | Other Wi-Fi decodes; the transmitter is on another channel or transport |
| `odid_rejected` > 0 | F3411 payload malformed per the validity rules: keep the JSONL/IQ and report it |
| `dsss_cck_skipped` > 0 | 5.5/11 Mb/s CCK frames are present (not supported) |

If real ODID frames fail but other access points decode, keep the IQ file: it is the evidence
needed to find a PHY difference.

## 6. GUI replay of the recording

```bash
./build/droneid_ui --odid --replay "$RUN/odid_ch6.iq" --format sc16 --check > "$RUN/ui_replay_check.log" 2>&1
./build/droneid_ui --odid --replay "$RUN/odid_ch6.iq" --format sc16 --frames 300 --screenshot "$RUN/ui_replay.ppm"
```

**Pass:** the packet count equals `odid_frames` from step 4. Track lines show
`source=ODID/ASTM` with the same UAS ID, and the screenshot shows the drone in the
"Drones detected" table (`gen` = `ODID/ASTM wifi_beacon` or `wifi_nan`).

## 7. Live GUI

```bash
./build/droneid_ui --odid --device addr=192.168.10.2 --freq 2437e6 --gain 20 --check --secs 30 \
  > "$RUN/ui_live_check.log" 2>&1
./build/droneid_ui --odid --device addr=192.168.10.2 --freq 2437e6 --gain 20
```

**Pass:** the transmitter appears as an `ODID/ASTM` track with the correct UAS ID and position.
Overflows, timestamp gaps and dropped DSP windows are zero or reported. Take a screenshot
of the native window and save it as `$RUN/ui_live.png`. Record whether the track keeps updating
for at least 5 minutes, and save the status line counters.

## 8. Write up

Put `$RUN/README.md` in the format of `docs/X310_BENCH_20261028.md`: inventory, commands, a
pass/fail table pointing at each log, and open limitations. Then update the ODID section of
`progress.md` and the gate table in `docs/OPEN_DRONE_ID.md`. Mark items verified only where
a saved log shows it.
