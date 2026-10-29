# Automatic X310 frequency sweep

Use **balanced** as the starting profile on the current X310/UBX-160 with 1 GbE.
There is no measured universal “ideal” sweep: detection depends on channel
occupancy, packet period, SNR, interference, bandwidth and processing gaps.
The implementation automatically retunes, receives, validates and processes each
dwell; it is not merely a printed frequency list. Bench capture continuity has
passed; per-drone detection/coverage acceptance remains incomplete. Native Linux
tests and Windows cross-compilation pass.

29 October bench update: the connected X310 completed all 33 balanced visits with
valid capture continuity and zero overflows. Actual start-to-final-capture time
was about 61 seconds including processing, so the 28–30-second listening budget
is not an observed revisit time. The initial scan preceded transmitter activation.
Known-source detection/coverage results are in `data/analysis/final_20261029/REPORT.md`.

## Balanced frequencies and dwells

| Stage | Centers, MHz | Base listening dwell | Purpose |
|---|---|---:|---|
| DJI DroneID 2.4 GHz | 2399.5, 2414.5, 2429.5, 2444.5, 2459.5 | 800 ms each | Existing O1/O2/O3/O3+ decoder channel hypotheses |
| DJI DroneID 5.8 GHz | 5756.5, 5776.5, 5796.5 | 800 ms each | Same decoded-packet path |
| O4 2.4 GHz | 2405, 2415, 2425, 2435, 2445, 2455, 2465, 2475, 2478.5 | 640 ms each | Signal-structure candidates |
| O4 5.8 GHz | 5730, 5740, 5750, 5760, 5770, 5780, 5790, 5800, 5810, 5820, 5830, 5840, 5845 | 640 ms each | Signal-structure candidates |
| Wi-Fi Remote ID | 2412, 2437, 2462 (channels 1, 6, 11) | 1,000 ms each | ASTM F3411 Beacon/NAN, DSSS/OFDM |
| Optional O4 5.2 GHz | 5175, 5185, 5195, 5205, 5215, 5225, 5235, 5245 | 640 ms each | Additional regional operating band |

DJI publishes Air 3S link bands of 2400–2483.5, 5170–5250 and 5725–5850 MHz,
with regional variation ([official specifications](https://www.dji.com/air-3s/specs)).
The O4 center grid is an engineering choice, using at most 10 MHz spacing within
a 20 MSPS capture to retain edge margin. Geometric band coverage does not prove
that every partially captured O4 waveform can be classified. The older DroneID
centers are the repository's existing channel hypotheses, not a complete list
for every model/region. Do not infer all drones use them.

Keep **20 MSPS**, **200 MHz master clock**, single RX2, host fc32, wire sc16.
This is approximately **80 MB/s / 640 Mb/s of IQ payload**, before Ethernet/UHD
overhead, and allows all three decoder modes without reinitializing the X310
between 184.32 and 200 MHz clocks. Prior fixed-channel transport at 20 MSPS was
verified on this setup; it does not validate this new hopping schedule.

Each visit adds **50 ms settling after the timed tune** and **100 ms command lead**.
These are conservative initial guards, not measured LO-settling guarantees.
The scheduler adds deterministic **0–40 ms dwell jitter** and rotates the first
entry by seven positions each cycle. Each complete cycle still visits every
configured entry. Modes are interleaved, rather than exhausting one mode first.

## Profile tradeoffs and actual revisit time

| Profile | DroneID dwell | O4 dwell | Wi-Fi dwell | Visits/cycle | Cycle before processing, default jitter |
|---|---:|---:|---:|---:|---:|
| fast | 640 ms | 320 ms | 500 ms, channels 1/6/11 | 33 | 18.61–19.93 s |
| **balanced** | **800 ms** | **640 ms** | **1 s, channels 1/6/11** | **33** | **28.43–29.75 s** |
| thorough | 1.28 s | 1.28 s | 1 s, channels 1–13 | 43 | 57.85–59.57 s |
| thorough + 5.2 GHz | 1.28 s | 1.28 s | 1 s, channels 1–13 | 51 | 69.29–71.33 s |

These are scheduling budgets, **not measured revisit times**. RX stops after each
dwell so its decoder can process a complete, bounded capture without silently
dropping queued windows. Add processing, allocation, draining and host scheduling
time. `processing_seconds` and `blind_seconds_before_dwell` are written for every
visit. During these gaps, and while on another frequency, this receiver cannot
observe the missed band. Long confirmation dwells on every hit would further
delay other channels; automatic unbounded “lock on hit” is intentionally absent.

O4's strong path needs at least 80 ms and its weak path needs **160 ms of continuous
samples**. A 640 ms dwell yields about forty 16 ms evidence windows and more
opportunity for intermittent weak signals. The minimum 160 ms is not a reliable
operational dwell recommendation. The Avata 2 holdout still includes a miss.

For a *measured*, fixed-channel periodic DroneID burst with period `T` and complete
burst duration `b`, the random-phase single-visit model is:

`P(complete burst within dwell) = clamp((D − b) / T, 0, 1)`.

For illustration only, `T=640 ms`, `b=0.65 ms` gives `D=608.65 ms` for a modeled
95% single-visit probability. The balanced 800 ms gives margin under those
assumptions. Do not treat 640 ms as every drone's measured period, this probability
as total recall, or visits as independent when transmitters hop. Measure actual
periods and residence time before shortening dwells.

## Run it

In the GUI choose **Automatic sweep**, **balanced**, enter the X310 device address,
gain and antenna, and choose a writable evidence log path. Click Connect.
Disconnect stops the sweep. Fixed-center and Wi-Fi-only controls do not override
the sweep plan. Enable 5.2 GHz if it is relevant to the drones being tested; enable
all Wi-Fi channels if channel 1/6/11 coverage is insufficient.

Inspect the exact plan without touching hardware:

```bash
./build/droneid_sweep --profile balanced
./build/droneid_sweep --profile thorough --include-52
```

Run one bounded cycle, then continuous operation after acceptance:

```bash
./build/droneid_sweep --live --device addr=192.168.10.2 --profile balanced --cycles 1 --gain 20 --output sweep.jsonl
./build/droneid_sweep --live --device addr=192.168.10.2 --profile balanced --cycles 0 --gain 20 --output sweep.jsonl
./build/droneid_ui --device addr=192.168.10.2 --sweep --sweep-profile balanced --sweep-jsonl sweep.jsonl
```

On Windows use `tools\droneid_sweep.exe` or `droneid_ui.exe` from the extracted
package; install compatible x64 UHD first. Ctrl+C stops the CLI. Evidence files
append across runs and each run starts with a timestamped `sweep_start` event.

Each dwell reports center, mode, actual scheduled device start, samples, validity,
decoded-packet count, O4 candidate-window count, processing time, preceding blind
interval and cumulative overflow count. A late first timestamp, a sample gap,
an overflow or an interrupted/partial dwell rejects that whole dwell. All detector
state is new after a retune. O4 candidates never become drone identities or bearings.

## Hardware acceptance and improving coverage

1. Run one cycle with antennas and no known transmitter. Check all visits are
   valid and measure actual revisit times from device timestamps. Confirm tuning
   settles by comparing initial and later dwell sections against a known source.
2. Run a known drone on each tested band, then vary its channel/link mode. Compare
   fixed-channel captures against sweep captures. Record missed visits and SNR.
3. Repeat with Wi-Fi-only, other-drone and multiple simultaneous sources; report
   false candidate visits/hour, event-level recall and revisit latency. Use the
   frozen detector; do not label all RF activity as drones.
4. Test ten minutes, stop/reconnect, cable removal and error reporting. Require
   zero unexplained capture gaps/overflows, and retain all blind-interval logs.
5. Optimize from those measurements: shorten only demonstrated-safe dwell/settle
   times, reorder known active channels while reserving exploration time, or use
   another independent receiver for fixed-channel follow-up.
6. For materially broader simultaneous coverage, move to verified 10 GbE transport
   and a wideband channelizer. An X310's 160 MHz analog bandwidth alone does not
   deliver 160 MHz of samples over 1 GbE. Do not reuse an untested sc8 conversion
   profile or two-channel transport beyond the measured budget.

Current limits: O4+ unvalidated; BLE Remote ID absent; proprietary non-DJI/915 MHz
links are not decoded; some DroneID payloads remain encrypted/unparsed; sweeping
does not supply a phase calibration for direction finding. Complete DF still
requires the physical reference and angle tests in `X310_DF_VERIFICATION_RUNBOOK.md`.
