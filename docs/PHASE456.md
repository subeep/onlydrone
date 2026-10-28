# Phase 4–6 receiver tools

This implementation follows the numbering in `progress.md`: Phase 4 is hopping,
Phase 5 is RF/O4 analysis, and Phase 6 is direction finding. The original build
plan calls direction finding Phase 7 and calls wideband/Wi-Fi/outputs Phase 6.
That separate wideband/Wi-Fi/SQLite work is **not implemented by this change**.

These headless tools complement the implemented Phase 3 GUI/replay and live
single-channel pipeline. Initial X310 transport/capture checks are now recorded in
`X310_BENCH_20261028.md`; full live acceptance remains open. All radio operations are receive-only.
The DF completion sequence is in `docs/DIRECTION_FINDING_PLAN.md`.

## Phase 4: scheduled dwell reception

```bash
./build/droneid_hop --band 24 --dwell 0.8 --cycles 2
./build/droneid_hop --band 58 --period 0.64 --burst 0.00065 --probability 0.95
```

The second command is an **illustrative model**, not a measured local burst
period. Supply a period measured from continuous, validated DroneID captures;
the public samples concatenate candidate bursts and cannot establish periodicity.

For a periodic stationary transmitter on the selected channel and uniformly
random phase, the probability of receiving one complete burst during one dwell is
`clamp((dwell - burst_duration) / period, 0, 1)`. Tuning/settling time is outside
the dwell. The scheduler reports this conditional probability, not a measured
receiver catch rate. Repeated visits are not necessarily independent: deterministic
hop cycles can lock to an unfavorable phase. Packet loss, transmitter channel
hopping, interference and decode failures are not included in the model.

UHD-enabled builds can execute the visits:

```bash
./build/droneid_hop --live --config config/x310_1gbe_nb.toml --band 24 --dwell 0.8 --cycles 2
```

The tool queues a future UHD tuning time, starts reception after settling, checks
RX timestamps/overflows, receives a bounded dwell, stops RX, and decodes it. This
is **not gapless streaming**: decoding and restarting add coverage gaps, reported
as `extra_gap_before_tune_seconds`. Live dwell is capped at two seconds and the
settling guard must be at least 20 ms. Actual tune settling still needs hardware
measurement. The JSONL summary reports valid visits and visits containing a
CRC-valid packet; that hit fraction is not the fraction of all transmitted packets
caught. No silent decode is attempted across a detected RX gap.

The timed-control implementation is compiled against the installed UHD 4.1.0.5
headers (`set_command_time`, `clear_command_time`, timed stream start). Late
commands are rejected before dispatch where possible. Live timing, settling,
coverage and the 1GbE soak test remain unverified.

## Phase 5: RF evidence, not invented O4 identities

Update 28 October: `o4_detect` and the GUI now evaluate repeated signal structure
as experimental `o4_candidate` evidence. This is separate from the energy-only
tool below and does not establish identity or drone count. Full local results,
algorithm and remaining validation: [O4 detector](O4_DETECTOR.md).

```bash
./build/rf_analyze capture.iq --noise-dbfs -60 > rf-events.jsonl
```

The noise value is an example. Measure it at the same frequency, gain and receive
bandwidth. Sidecar format/rate settings override CLI defaults, just as in the
other file tools. Input is processed in 65,536-sample blocks. The energy detector
uses 32-sample power gates, bridges quiet gaps shorter than approximately 20 µs,
and emits events of at least 50 µs, splitting long activity at approximately
10 ms. Split events and file-boundary events are marked `truncated`.

Each JSONL event contains sample index, duration, mean RSSI in dBFS, SNR relative
to the supplied noise estimate, Hann-windowed 99%-power bandwidth, spectral
centroid, and fraction of samples whose I or Q reaches 0.999 full-scale. A spectral
centroid is **not carrier-frequency offset**. dBFS is not calibrated received
power or distance. RSSI changes across different gain settings are not comparable.
No absolute range, unique-drone count, session ID or O4 frame layout is inferred.

The four-file smoke check in `data/analysis/phase456_rf_20261026/` includes the
no-drone baseline and both active Air 3S bands. All files were read completely.
There are RF events in the baseline too: energy/bandwidth alone does not establish
O4 identity or distinguish Wi-Fi. Every event therefore remains `unknown_rf` and
`identified_drones` is null. A verified classifier, per-source CFO/fingerprint,
source association and GUI integration remain pending labeled, controlled data.

## Phase 6: aligned paired capture and calibrated bearing

```bash
./build/iq_record_pair /tmp/paired --config config/x310_df.toml --seconds 10
```

This receives Radio#0 and Radio#1 through one multi-channel UHD stream, uses
11.52 MSPS and sc16 on the wire with `config/x310_df.toml`, and writes **cf32** files plus per-channel metadata.
On-wire traffic is approximately 92.16 MB/s plus overhead; disk traffic is
approximately 184.32 MB/s. The installed stack rejected sc8 conversion. The tool
rejects mismatched timestamps, overflows and unexpected rate. The integrated recorder
now uses a bounded writer queue and manifest-last publication: interrupted captures
remain incomplete. Hardware versions, board serials, rates, tuning, bandwidth and
LO/reference locks are queried before and after capture. Continuous lock monitoring
and recorder fault-injection tests remain pending; see `docs/X310_BENCH_20261028.md`.

Timed start alone does not establish RF phase alignment. X310/UBX also supports
timed LO synchronization, with residual phase offsets requiring calibration.
Actual clock compatibility and phase-repeatability must be measured on this rig;
do not describe compiled software plumbing as measured RF coherence. See the
official synchronization references in `docs/DIRECTION_FINDING_PLAN.md`.

Calibration uses a splitter or a known broadside source with the same receiver
gain, RF frequency, sample rate, antenna/cable setup and uninterrupted LO session:

```bash
./build/df_bearing /tmp/paired.ch0.cf32 /tmp/paired.ch1.cf32 --aligned \
  --splitter-calibrate --spacing-m 0.05 --measured-unix-ns CAPTURE_TIMESTAMP_NS \
  > /tmp/phase-cal.toml
```

Replace `CAPTURE_TIMESTAMP_NS` with the actual capture's Unix timestamp. The
paired metadata includes `capture_host_unix_ns`, a host-receive-time approximation
for freshness checks, distinct from device sample timestamps. Calibration includes
frequency, rate, phase bias, antenna spacing, timestamp and expiration (default
one hour). Use shorter expiration when drift demands it. Splitter calibration
requires normalized inter-channel coherence of at least 0.98.

For a later target window **within that same uninterrupted receiver/LO session**:

```bash
./build/df_bearing /tmp/paired.ch0.cf32 /tmp/paired.ch1.cf32 --aligned \
  --cal /tmp/phase-cal.toml --start-sample 1000000 --samples 16000
```

The start/sample counts are illustrative: select the same isolated burst in both
files. `--aligned` is an explicit operator assertion of time-aligned, gap-free
coherent inputs. Equal file lengths alone do not prove that. Do not use sequential
single-channel recordings. Treat reconnects and retunes as invalidating calibration
until timed-phase repeatability and configuration/session provenance are verified,
even when file age/frequency checks pass. This is a conservative application policy,
not a claim that UBX cannot synchronize its LOs. Session identity and cable changes
cannot be inferred from raw files; the operator must enforce this.

Bearing is `asin(wrapped_phase * wavelength / (2*pi*spacing))`, relative to antenna
pair broadside. Positive phase means channel 1 leads channel 0 under this
convention; verify physical left/right polarity with a known source. The result
always reports front/back ambiguity and never absolute coordinates. Spacing must
not exceed half a wavelength. Invalid/expired calibration, unequal timestamps or
lengths, non-finite samples, insufficient power, coherence below 0.8, and
physically inconsistent phase are rejected. Multiple overlapping transmitters
and multipath can still yield misleading bearings; coherence is not identity.

The API works on raw IQ regardless of whether a payload decodes. Automatic burst
pairing, source association, attaching bearings to RF-event/telemetry records,
the direction panel and measured angular-error envelope are still pending.

## Validation

```bash
cmake --build build -j 2
ctest --test-dir build --output-on-failure
```

Tests cover schedule wrap and catch-model boundaries, invalid inputs, calibrated
synthetic angles, phase wrapping, stale/mismatched calibration, low coherence,
RF metrics, chunk-independent gating, bounded continuous activity, CLI rejection
and calibration/estimate roundtrip. They do not substitute for live-radio tests.
