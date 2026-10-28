# X310 direction-finding verification: step-by-step runbook

Prepared: **28 October 2026**. Scope: one X310, two UBX-160 receivers, two RX2
inputs, and the C++ receiver/GUI in this repository. Phase 6 means direction finding.

This is a verification procedure, not a record of tests already passed. Complete
each gate with saved evidence before advancing. Numerical targets below are
**proposed project acceptance criteria**, not Ettus specifications or achieved results.

## 1. What this procedure will establish

The intended output is a calibrated **signed angle relative to the antenna pair's
broadside**, associated with one isolated RF burst and, where available, that same
burst's decoded serial. A two-element baseline retains front/back ambiguity.
It does not establish unique 360° azimuth, range, elevation or GPS position.

We must verify five different things separately:

1. Both receivers deliver continuous, correctly paired samples.
2. Their relative phase is stable enough and calibration survives only explicitly
   tested operations.
3. Electronics, cables, antennas and geometry produce the expected phase-versus-angle response.
4. DSP, calibration state and GUI preserve the measurements and reject invalid data.
5. Measured field error and accepted coverage meet a declared operating envelope.

Existing single-channel drone IQ can test decoders. It cannot establish physical
direction accuracy. Synthetic second channels test software only.

### Current evidence and limits

| Item | Status at preparation |
|---|---|
| X310 and two UBX boards discovered | Passed; X310 `326CF02`, UBX `325B957` / `325B9BA` |
| 600-second dual-channel UHD transport soak | Passed at 11.52 MSPS/channel, sc16; zero reported transport errors |
| Bounded timed paired application recording | Passed; manifests, checksums, timestamp continuity and queried hardware settings |
| Single-channel GUI live reception | Passed a three-second smoke check at its fixed 15.36 MSPS |
| Native GUI display / uncalibrated paired replay | Checked; no accepted bearings without calibration |
| Software tests | 13/13 release CTest suites passed in the preceding bench run |
| Split-source phase stability / physical calibration | Not measured; operator has antennas or no reference connected |
| Persistent paired live worker / reference switching | Not implemented; live DF fails closed |
| Differential group-delay correction | Not implemented; current estimator is scalar phase only |
| Surveyed angular accuracy / O4 identity accuracy | Not established |

Evidence: [initial bench report](X310_BENCH_20261028.md). Architecture and remaining
implementation work: [completion plan](DIRECTION_FINDING_PLAN.md) and
[integration review](DF_REVIEW_20261027.md). This runbook is the operational test
sequence; older unchecked implementation lists are not a current feature inventory.

### When the hardware is needed

| Work | X310 | Additional requirement |
|---|---|---|
| Build, fault fixtures, saved-pair replay, analysis tooling | No | Existing local data |
| Inventory, transport and bounded capture | Yes | Ethernet; antennas or terminated inputs are sufficient |
| Electronics phase stability and retune tests | Yes | Known RF generator, splitter, attenuators, characterized cables |
| Broadband delay / sample skew measurement | Yes | Shared broadband/noise or modulated reference; a CW tone alone is insufficient |
| Full antenna calibration and accuracy | Yes | Surveyed rigid antenna rig, known LOS source/angles |
| Decoded serial-to-bearing association | Yes | Linked transmitting drone or controlled known waveform/source |
| O4 event association | Yes for new paired data | Independently labeled O4 transmission and negative controls; decode not required |

**The next useful physical step is a split reference. Merely leaving two antennas
connected does not provide a known phase calibration.**

## 2. Freeze the test profile and evidence folder

Use this initial profile; test changes as separate profiles:

| Setting | Initial value |
|---|---|
| Radio address | `192.168.10.2`; host `192.168.10.1/24` |
| Hardware / software | X310 HG, two UBX-160 v2, UHD `4.1.0.5-3`, FPGA `38.0` |
| Master clock / paired rate | 184.32 MHz / 11.52 MSPS per channel |
| Channels / wire / stored samples | `[0,1]` / sc16 / cf32 |
| Center / receive ports | 2459.5 MHz / RX2 on both boards |
| Initial gain | 10 dB on both channels; change only as a recorded new configuration |
| Reference / time | Internal / internal for this single motherboard |
| Analog bandwidth | Supported UBX setting 160 MHz at this frequency |
| Initially evaluated signal band | Narrow reference first; measured expansion toward ±4.5 MHz later |

The 160 MHz analog setting is not the usable digital bandwidth. At 11.52 MSPS,
complex-sample Nyquist limits are ±5.76 MHz; useful passband is narrower because
of filtering and edge guards. The current calibration CLI declares up to 10 MHz
total passband; that declaration is not evidence that a center-tone calibration
is accurate over that whole band.

The installed stack rejected sc8-to-fc32 conversion. Do not restore the old sc8
profile without a separate compatibility test. Keep software/FPGA upgrades out of
an acceptance session; if upgraded, record the new versions and rerun the gates.

From the repository root, create a fresh evidence directory:

```bash
cd /home/sudeep/Music/punjdrone
RUN="data/analysis/df_verify_$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$RUN"
git rev-parse HEAD > "$RUN/git-head.txt"
git status --short > "$RUN/git-status.txt"
git diff --binary > "$RUN/working-tree.patch"
cp config/x310_df.toml "$RUN/profile.toml"
date -u --iso-8601=seconds > "$RUN/host-time.txt"
```

Keep `RUN` in the same shell throughout the command examples. Record relevant
untracked source files separately; a Git diff does not include them. Save compiler,
OS, CPU, NIC/driver, storage path, available space, room temperature, operator,
radio warmup time, source model/serial/settings and the actual wiring diagram.

Use a unique prefix for every acquisition. Do not edit historical IQ/manifests
to make validation pass or reuse a session ID across radio reopen operations.
Leave datasets in the project and `/media/sudeep/Storage/datasets` read-only.

**Gate:** another operator can identify exactly which build, rig and profile were tested.

## 3. Prepare and verify the physical bench

### Equipment

- RF source covering each frequency to be tested, with known output level.
- Two-way 50-ohm splitter rated for the band, with insertion-loss and phase-balance data.
- Two fixed attenuators and two labeled receive cables, with measured or documented loss.
- Power meter or spectrum analyzer to verify power at each receiver end before connection.
- Appropriate terminations; no X310 transmit operation is needed by this plan.
- For later steps: characterized RF switch paths, broadband reference capability,
  rigid antenna mount, matched antennas, angle reference and distance measurement.
- Optional VNA for cable/splitter phase and antenna matching characterization.

### Wiring

```text
Known RF generator (output initially OFF)
                |
       common attenuation as needed
                |
          2-way splitter
          /            \
   branch pad A     branch pad B
        |                |
    cable RX-A        cable RX-B
        |                |
   UBX A RX2         UBX B RX2
   channel 0         channel 1
```

1. Photograph and label both paths, including connector adapters and splitter ports.
2. With the source disabled, assemble the path and inspect connectors.
3. Measure each branch's delivered power using the same cable end that will attach
   to RX2. Record frequency, source level, attenuation and measurement uncertainty.
4. Start around **−50 dBm per RX input** where practical. Increase only if needed
   for measured SNR, keeping the initial bench target at or below **−35 dBm per port**.
5. Disable the source, connect RX2 inputs, then enable the source at the verified setting.
6. Keep the source frequency and amplitude stable during each capture. Log every change.

The UBX maximum RF input is **−15 dBm**, not a test target. The selected −35 dBm
ceiling provides margin; receiver gain does not protect the RF input from excessive
power. Account for all signals and source transients. These limits are supported by
the [Ettus UBX specification](https://kb.ettus.com/UBX) and
[UBX handling guide](https://kb.ettus.com/UBX_Getting_Started_Guides).

For each branch, calculate and then measure:

```text
P_RX_dBm = P_source_dBm − common_loss_dB − splitter_branch_loss_dB
           − branch_pad_loss_dB − cable_loss_dB
```

Example only: −10 dBm source, 10 dB common pad, 3.5 dB splitter branch loss,
20 dB branch pad and 1 dB cable loss yields −44.5 dBm. Substitute actual losses.
A splitter's nominal 3 dB division is not its complete insertion loss.

**Gate:** both input powers and branch identities are known. If source/power
verification equipment is missing, continue software or transport work only.

## 4. Establish software and transport baselines

### 4.1 Build and regression checks — no radio required

```bash
cmake --build build -j 2 > "$RUN/build.log" 2>&1
ctest --test-dir build --output-on-failure > "$RUN/ctest.log" 2>&1
./build/iq_record_pair --help > "$RUN/iq-record-pair-help.txt"
./build/df_bearing --help > "$RUN/df-bearing-help.txt"
./build/droneid_ui --help > "$RUN/gui-help.txt"
```

Check exit status and logs after each command. The expected baseline is 13 passing
CTest suites; a changed suite count needs explanation, not automatic rejection.
Do not proceed with unexplained failures in pairing, calibration, decoder or UI tests.

### 4.2 Inventory — X310 required, one process owns it

Close other radio applications before every hardware command. Never run the
benchmark, recorder and live GUI against the radio simultaneously.

```bash
ip -brief address > "$RUN/network.txt"
cat /sys/class/net/enp0s31f6/speed > "$RUN/link-speed.txt"
cat /sys/class/net/enp0s31f6/mtu > "$RUN/link-mtu.txt"
timeout 15s uhd_find_devices --args 'type=x300' > "$RUN/discovery.log" 2>&1
timeout 30s uhd_usrp_probe \
  --args 'addr=192.168.10.2,master_clock_rate=184320000' \
  > "$RUN/probe.log" 2>&1
```

Verify radio/board serials, actual clock, firmware/FPGA/UHD, RX2 availability,
bandwidth range and sensor availability. Do not select an external clock or PPS
that is not physically connected and verified. One X310 shares the motherboard
timebase; sample-time alignment and RF phase calibration still need separate checks.

### 4.3 Transport-only soak

The previous 600-second result is reusable baseline evidence for the unchanged
host/profile. Repeat after link, kernel, UHD, clock/rate or hardware changes:

```bash
timeout 630s /usr/lib/uhd/examples/benchmark_rate \
  --args 'addr=192.168.10.2,master_clock_rate=184320000' \
  --rx_rate 11520000 --rx_channels 0,1 --rx_otw sc16 --duration 600 \
  --overrun-threshold 0 --drop-threshold 0 --seq-threshold 0 \
  > "$RUN/transport-600s.log" 2>&1
```

**Pass:** completed duration, nonzero samples from the selected two-channel stream,
zero reported drops, overruns, RX sequence errors, timeouts and late commands;
transmitted sample count remains zero. Save exit status and full summary.

This discards IQ and does not test phase, the application writer, paired DSP or GUI.
At this profile the combined wire payload is 92.16 MB/s before overhead. Full cf32
recording is 184.32 MB/s: 18.432 MB per 0.1 s, 184.32 MB per second, and 110.592 GB
per ten minutes. Use metrics and bounded snippets rather than a full ten-minute file.

**On failure:** save NIC counters and CPU/load context, isolate the cause, then
repeat. Do not silently change rate/wire format and report the old profile as passing.

## 5. Verify paired recording and provenance

This step works with antennas/no reference, but in that case it proves transport
and metadata only. With a split source it also provides the first phase-analysis input.

For a splitter-only bench, the 0.05 m value below is an explicit nominal conversion
parameter, not an antenna measurement. Use a measured value and new rig ID for OTA work.

```bash
timeout 30s ./build/iq_record_pair "$RUN/pair-smoke" \
  --config config/x310_df.toml --seconds 0.1 --gain 10 \
  --rig-id splitter_bench_v1 --cable-id rxA_rxB_v1 \
  --spacing-m 0.05 --orientation-deg 0 \
  > "$RUN/pair-smoke.log" 2>&1

./build/droneid_ui --check --pair-manifest "$RUN/pair-smoke.pair.toml" \
  > "$RUN/pair-smoke-ui.log" 2>&1
```

Inspect the complete manifest and both sidecars:

- `complete=true`, status `ok`, matching nonempty session/rig/configuration identity.
- Exactly 1,152,000 cf32 samples per channel for 0.1 s at the expected rate;
  each IQ file is 9,216,000 bytes, excluding sidecars.
- Positive common first device timestamp, correct sample counts, no reported
  discontinuities/overflows and verified checksums.
- `hardware_observed=true`, correct board/version IDs, reference and both LO locks.
- Matched actual rates, tuned center, gains, antenna ports, RF/DSP settings and
  supported analog bandwidth. Log requested and actual values separately.
- No fabricated reference, accepted bearing or drone identity during uncalibrated replay.

Current lock checks happen before and after acquisition, not continuously.
Equal timestamps do not prove zero analog delay or RF phase alignment. A pure tone
cannot uniquely reveal integer sample skew; a broadband reference is needed later.

**Gate:** complete pairs replay successfully; invalid/incomplete pairs do not.
Record any UHD tick-rate-zero warnings. Their presence must be investigated by
the phase tests, not dismissed because samples arrived.

## 6. Measure split-source phase stability within one capture

### 6.1 Reference setup

1. Warm up the equipment for a recorded interval, initially 30 minutes. Treat this
   as a repeatable test condition, not proof that thermal drift has stopped.
2. Set RX center to 2459.5 MHz and the CW source initially to **2460.0 MHz**
   (a +500 kHz offset), avoiding DC. Verify measured frequency and input power.
3. Keep both RX gains fixed. Confirm adequate SNR and no clipping in either channel.
4. Record 1 second with the Step 5 command under a new prefix and `--seconds 1`.
5. Reserve the first reference window for calibration and later windows for evaluation.
   Do not calibrate and score the exact same samples.

### 6.2 Analysis required

Implement/verify a bounded paired-analysis script before claiming this step passed.
There is currently no dedicated phase-stability report command. It must read the
manifest, validate the pair, operate in original sample coordinates and report:

```text
C_k   = sum(conj(x0[n]) * x1[n]) over window k
phi_k = arg(C_k)
rho_k = abs(C_k) / sqrt(sum(abs(x0[n])^2) * sum(abs(x1[n])^2))
r_k   = wrap_to_pi(phi_k - reference_phase)
```

Use at least 100 non-overlapping windows; a 65,536-sample window is about 5.69 ms
at 11.52 MSPS. Report per-channel power, measured SNR, clipping, coherence, circular
phase statistics, drift and timestamp validity. Use circular statistics near ±π;
ordinary arithmetic averages across the wrap boundary are invalid. Save plots of
phase residual, coherence and power versus device sample time.

**Proposed short-term gate:** SNR ≥20 dB on both channels, zero clipped reference
samples, reference coherence ≥0.98, residual circular standard deviation ≤1°,
and P95 absolute residual ≤3° on held-out windows. Report all failed windows.
The code's broader quality guards do not substitute for this stricter bench gate.

### 6.3 Existing CLI calibration smoke check

Only perform the following on an independently verified **split-reference** capture.
For `pair-smoke`, calibrate on the first 65,536 samples and score a later disjoint
window. Obtain the capture timestamp from the manifest; do not insert current time.

```bash
PAIR="$RUN/pair-smoke.pair.toml"
CAPTURE_NS=$(awk '$1 == "capture_host_unix_ns" {print $3}' "$PAIR")
./build/df_bearing --manifest "$PAIR" --splitter-calibrate \
  --samples 65536 --spacing-m 0.05 --measured-unix-ns "$CAPTURE_NS" \
  --max-age 60 > "$RUN/reference-cal.toml" 2> "$RUN/reference-cal.log"
```

Check successful exit and a complete calibration file before proceeding:

```bash
./build/df_bearing --manifest "$PAIR" --cal "$RUN/reference-cal.toml" \
  --start-sample 131072 --samples 65536 \
  > "$RUN/heldout-reference.json" 2> "$RUN/heldout-reference.log"
```

The expected held-out result is a small **electrical residual** near zero, with
valid coherence. Calling that zero an accurately surveyed antenna bearing would
be incorrect. A single CW check also does not validate the CLI's whole declared
passband. Limit its use to the tested signal until Step 9 is complete.

If using a later calibration window, the required measurement timestamp is
`capture_host_unix_ns + round(start_sample * 1e9 / sample_rate)`.

**Gate:** saved calibration, disjoint evaluation windows and raw phase statistics
agree. The same capture is suitable for same-session analysis; a newly opened
recorder is a new radio session, even if the frequency is unchanged.

## 7. Build the persistent bench/paired-live path before long-term tests

This is a software prerequisite, not an existing runnable command. The recorder
currently closes the radio when each file ends and limits captures to 60 seconds.
A shell loop around it cannot establish continuous same-session phase stability.
Likewise, `droneid_ui --device ... --df` currently fails closed by design.

Implement a bench runner and persistent paired worker using the same source and
calibration service. They must:

- Own one open radio from reference acquisition through target measurement.
- Use a single two-channel RX stream and common future tune/start times.
- Record session and configuration epochs; never copy a calibration ID into a new session.
- Produce per-window metrics continuously, retaining only bounded IQ around selected
  reference windows, failures and representative targets.
- Poll and timestamp reference/LO sensor status at a documented cadence, initially
  1 Hz plus checks around tuning. State that brief unlocks between polls can be missed.
- Record configuration changes, queue drops, timestamp gaps, lock observations,
  rejected windows, calibration transitions and rejection reasons.
- Support explicit reference/target intervals and settling exclusions. Do not infer
  that any coherent received signal is the calibration source.
- Bind calibration to the actual rig/cables/switch path, board identities, software
  versions, RF/DSP tune, gains, rate, antennas, digital processing and passband.
- Fail closed on unverified changes; use the same validation path for CLI and GUI.

For splitter-to-antenna transfer, provide characterized switch paths while keeping
the radio open. Reconnecting cables by hand is not automatically repeatable.
An alternative is a surveyed OTA broadside calibration with the actual antennas
left in place. That method must have its own recorded provenance and acceptance.

**Gate:** deterministic tests prove lifecycle/queue behavior; a bench run retains
one real session from reference to held-out target. Do not simply remove the live-DF
guard to reach this gate.

## 8. Test drift, timed retuning and lifecycle repeatability

Use the persistent runner, unchanged split reference and fixed input power.
Do not re-zero phase after every trial and then claim calibration reuse works.
Save residuals relative to the initial reference, including failures.

| Test | Minimum proposed experiment | Required decision |
|---|---|---|
| Warmup drift | Track from startup through at least 30 min | Determine stable interval and whether longer warmup is needed |
| Post-warmup drift | 30 min of continuous metrics | Establish maximum age from measured drift, not a default 3600 s |
| Timed stop/start, same open handle | 100 cycles | Quantify CORDIC/start effects and settle interval |
| Timed tune to same frequency | 100 cycles | Quantify phase repeatability without recalibrating each cycle |
| Tune away and back | 100 cycles per intended frequency pair | Validate or prohibit frequency-keyed reuse |
| Close/reopen application | At least 10 independent sessions | Always invalidate by default; measure offsets diagnostically |
| Radio power cycle | At least 3 independent starts with recorded warmup | New session/calibration; no silent old-calibration adoption |
| Gain / path change | Each intended setting, both transition directions | New configuration/calibration unless separately validated |

For each cycle save requested tune time, actual RF/DSP settings, stream start
timestamp, locks, excluded settling duration, phase mean/std, coherence, power,
gaps and calibration state. Start with the recorder's conservative 100 ms
tune-to-stream gap; reduce it only through a measured settling experiment.

Use the ≤1° within-window standard deviation and ≤3° P95 residual targets from
Step 6 for the proposed reusable configurations. Report worst case and the count
above 5° as well. Zero unexplained phase jumps are required for automatic reuse.
If reuse fails, keep the system functional through explicit recalibration; do not
label a failed reuse policy as a failed ability to make any calibrated measurement.

Timed tuning and timed starts are supported mechanisms, but residual phase depends
on frequency and can drift. Follow the [UHD 4.1 synchronization guidance](https://files.ettus.com/manual_archive/v4.1.0.4/html/page_sync.html).
Installed UHD is 4.1.0.5-3; the archived 4.1.0.4 manual is nearby-version guidance,
not a substitute for measurements on this installation.

**Gate:** explicitly list valid settings, settling time, measured calibration age
and which lifecycle actions require recalibration. Unsupported reuse stays disabled.

## 9. Verify frequency response, sample skew and signal bandwidth

### 9.1 Tone sweep — receiver settings fixed

1. Hold the RX tune fixed and step the external source across offsets
   −4.5, −3, −1, −0.5, +0.5, +1, +3 and +4.5 MHz.
2. Wait for generator settling, verify power, and collect at least 100 valid
   analysis windows per offset. Keep the same radio session.
3. Plot differential phase and gain versus actual tone frequency. Unwrap phase
   only across coherent neighboring measurements; refine spacing if slope is ambiguous.
4. Fit the phase slope, keeping the fitted sign convention explicit. For
   `arg(conj(x0)*x1)`, a channel-1 delay gives slope `−2*pi*delta_delay` under
   the usual positive-frequency complex exponential convention.
5. Separate linear delay from nonlinear residual response and splitter/cable effects.
   Repeat after swapping known external branches as a diagnostic new rig state;
   restore and recalibrate before further acceptance tests.

### 9.2 Broadband reference

Feed one known broadband/modulated source through the splitter. Measure relative
integer lag, fractional delay and cross-spectrum over the occupied band. A CW
phase measurement alone cannot distinguish a sample shift from a phase offset.
Verify expected aligned sample indexing and quantify analog differential delay;
do not mistake legitimate analog group delay for a missing digital sample.

Inject a one-sample skew into a **copy** of a saved pair to demonstrate that the
diagnostic detects it. Never repair the original evidence files in place.

**Gate:** a scalar calibration is accepted only over a measured passband whose
residual phase meets the declared budget for all tested offsets and waveforms.
Otherwise narrow the supported band or implement and verify matched frequency-domain
complex-response/fractional-delay correction. The current code rejects nonzero
delay-slope calibration files; adding a phase ramp versus sample time is not a
valid substitute for correcting group delay across frequency.

For wideband O4 candidates, do not claim full-signal DF from a truncated slice of
an otherwise unvalidated waveform. Record any subband selection and validate its
angle bias separately. 11.52 MSPS is not full coverage of arbitrary O4 link bandwidths.

## 10. Inject failures and prove invalidation

Run software faults without hardware where possible. Use disposable copies and
bounded test storage; never fill the system drive or alter source datasets.

| Fault | Required observable result |
|---|---|
| Modified IQ/checksum, missing sidecar, unequal EOF | Pair rejected before any accepted bearing |
| Wrong session, epoch, rig, spacing, channel order or gain | Calibration rejected or marked stale |
| Expired calibration | No active valid angle; explicit age/expiry reason |
| Channel swap | Rejected configuration or independently recalibrated new orientation; never silent sign flip |
| Missing timestamp / sample discontinuity | Drop affected pair, reset both DSP paths, invalidate calibration |
| RX overflow / paired queue backpressure | Visible counters and invalidation; never process mismatched channels |
| LO/reference unlock | Invalidation on observation; record sensor cadence/latency |
| Saturation or poor coherence | Reference/target rejected; no plausible needle retained |
| Two overlapping emitters / multipath control | Measure unsafe accepted estimates; coherence alone is insufficient |
| Writer full / write-close-rename failure | No complete manifest; incomplete/error artifact retained |
| SIGINT during recording | Clean stop or incomplete capture, never published as complete |
| Cancel, disconnect, reopen, unplugged Ethernet | Clear active bearings; no deadlock; reconnect starts uncalibrated |

Implement controlled error injection for writer/queue/sensor failures before
trying destructive physical scenarios. Physical Ethernet removal is a separate
supervised test once recovery logging exists. Record detection-to-invalidation
latency; proposed UI target is ≤1 s after the software observes the fault.

**Gate:** zero valid bearings from known-invalid test windows; every injected fault
has the expected state transition and reason. An observed zero is a finite-sample
result, not proof of a zero long-term failure probability.

## 11. Verify the complete GUI/backend path

### 11.1 Offline paired checks available now

```bash
./build/droneid_ui --check --pair-manifest "$PAIR" \
  --df-jsonl "$RUN/uncalibrated-events.jsonl" > "$RUN/uncalibrated-ui.log" 2>&1

./build/droneid_ui --check --pair-manifest "$PAIR" \
  --df-cal "$RUN/reference-cal.toml" \
  --df-jsonl "$RUN/calibrated-events.jsonl" > "$RUN/calibrated-ui.log" 2>&1

./build/droneid_ui --pair-manifest "$PAIR" \
  --df-cal "$RUN/reference-cal.toml" --frames 180 \
  --screenshot "$RUN/paired-gui.ppm"
```

Run calibrated examples only after successful Step 6 and only on the matching
reference session. A continuous tone may be split/marked truncated by the burst
detector and legitimately produce no accepted burst bearings. Use isolated
reference bursts for event-level GUI testing; use `df_bearing` and phase metrics
for continuous-tone electrical tests. Do not weaken isolation gates to get an arrow.

Compare CLI and GUI on **identical sample intervals**. Packet/serial association,
validity and rejection reasons must agree; proposed numerical angle tolerance is
0.1° for the same samples and calibration. Different window boundaries are not
an apples-to-apples comparison.

Inspect calibration state, reference label, age, ambiguity, accepted/rejected
counts, RX errors, readable controls and exported event records. At EOF the GUI
must show replay completion and clear active needles while preserving event history.

### 11.2 Live acceptance — after Step 7 implementation

Exercise Connect → Uncalibrated → reference capture → Valid → target reception,
then Cancel, Disconnect and Reconnect. Confirm the renderer remains responsive,
the RX worker stops, and reconnect does not restore an old valid arrow.

Run the **integrated paired RX + DSP + GUI** for at least ten minutes at the
accepted profile. Save periodic metrics, CPU/memory/queue high-water marks and
bounded paired snippets. Require zero unexplained gaps, overflows, paired queue
drops, stale valid arrows and silent calibration changes. Target 95th-percentile
event display latency ≤1 s, measured from a documented event-time reference;
log processing/display timestamps because the current summary alone cannot prove latency.

This test is still required despite the already-passed UHD transport soak.
The single-channel GUI's fixed 15.36 MSPS path is not paired live DF acceptance.

## 12. Build and survey the antenna rig

1. Use two matched, identically polarized antennas on a rigid mount. Label channel
   0/1 and define broadside, positive angle and the physical reference axis.
2. Measure spacing between effective phase centers as well as practical mounting
   dimensions; record uncertainty. If phase centers are uncertain, characterize
   them rather than asserting sub-millimeter accuracy from ruler spacing.
3. Use `d <= c/(2*f_max)` for the highest RF frequency in the claimed operating band.
   At 2.4595 GHz the half wavelength is approximately 60.95 mm; at 5.7965 GHz it
   is approximately 25.86 mm. These are center-frequency examples; include band edges.
4. A 50 mm baseline is an initial 2.4 GHz rig, not a 5.8 GHz rig. A smaller baseline
   reduces phase sensitivity at 2.4 GHz; antenna size/coupling can constrain feasibility.
5. Fix cables so rotation does not flex them. Record mount, nearby metal, orientation,
   cable routing, heights and polarization. Do not change them between calibration and test.
6. Place the reference in clear LOS and at comparable height. Evaluate far-field
   conditions using the largest relevant aperture dimension, not just the baseline.
   Use `R >> 2*D^2/lambda` as a planning check, and verify results at two distances
   (for example 5 m and 10 m if suitable for the actual apertures and site).
7. Reduce ground/building reflections, record remaining geometry, and measure the
   source angle independently. Target ground-truth uncertainty ≤1°; propagate it
   into the reported system uncertainty rather than ignoring it.

For the phase model:

```text
lambda = c / f_RF
corrected_phase = wrap_to_pi(measured_phase - calibration_phase)
theta = asin(corrected_phase * lambda / (2*pi*d))
sigma_theta_rad ≈ lambda * sigma_phase_rad / (2*pi*d*cos(theta))
```

This explains worsening sensitivity toward endfire. Confirm channel polarity at
a known positive angle before evaluating the sign. Do not fit a sign convention
after viewing the held-out test errors.

**Gate:** a surveyed, photographed rig with repeatable path identity, measured
geometry and declared band. Electronic splitter calibration alone does not include
antenna phase patterns, coupling or mount effects.

## 13. Perform OTA calibration and held-out angle measurements

### 13.1 Broadside reference

With the actual antennas and unchanged radio session, place a known source at
surveyed 0°. Acquire reference windows, verify power/coherence/isolation, and use
the `ota_broadside` method. Record this separately from the splitter calibration.
The CLI option is `--ota-calibrate`; it still requires a valid paired manifest,
matching timestamp and measured spacing. Do not relabel an arbitrary source as broadside.

Keep calibration windows separate from test windows. Once this reference is taken,
move the source or rotate the rigid rig through known angles without changing the
receive cables/LO configuration. A new recorder process cannot reuse calibration
by assigning it the old session ID. Use the persistent acquisition path from Step 7.

### 13.2 Fixed-channel test matrix

Test **−60°, −45°, −30°, −15°, 0°, +15°, +30°, +45°, +60°** in randomized order.
For every angle and band collect at least 100 independently identifiable isolated
bursts in each of at least three independently calibrated sessions: at least
2,700 eligible bursts per band. Overlapping windows from one burst are one trial.

Use development sessions to select thresholds, then freeze them before held-out
sessions. If thresholds change, collect fresh held-out data. Keep invalid or
low-SNR trials in the overall accounting even when they are outside the claimed envelope.

Repeat at two source distances and include tests at lower SNR, different source
orientations and gains as separate strata. At minimum the claimed envelope must
specify band/passband, gain, SNR, angle range, polarization, rig and environment.

### 13.3 Required controls

- Source off / no drone: ambient RF events must not become invented drone identities.
- Wi-Fi-only source and other-drone source: measure classification errors separately.
- Two emitters with overlapping bursts and a reflective/multipath arrangement:
  quantify unsafe accepted angles and abstentions; no assumed single-source truth.
- Source behind the array: show the mirror ambiguity, not an asserted front solution.
- Reversed channels and changed cable/switch path: calibration invalidation or
  separately surveyed new-rig calibration, never silent reuse.
- New warmup/power-cycle/session: independent calibration and repeat accuracy test.

High inter-channel coherence is necessary in several tests but is not proof of
one source, LOS propagation, a particular drone type or an unambiguous angle.

## 14. Verify drone event association without confusing it with angle accuracy

Start with one controlled transmitter at a time, then introduce controls.

| Source group | What to verify |
|---|---|
| O1/O2/O3 with supported telemetry | CRC-valid packet and serial belong to the exact paired burst interval used for the angle |
| O3+ payloads with unsupported telemetry | Retain verified payload/envelope; do not invent a serial or telemetry fields |
| O4 / O4+ | Payload decoding optional; independent event-level source labels required to claim drone-specific DF |
| Wi-Fi / no-drone / mixed sources | Report false identities, false valid bearings and abstentions |

Known model presence in a recording is not proof that every burst came from that
model. Establish event labels using controlled source schedules, independently
observed transmissions and temporal/frequency evidence. Retain uncertain cases
as uncertain. Report O4 and O4+ separately. Until a classifier is independently
validated, the receiver's label remains `unknown_rf` even if the experimenter
knows which drone was active.

Existing local datasets can validate decoders and classifier hypotheses without
large downloads. They establish angle truth only if they genuinely contain
synchronized multi-channel captures with calibrated rig and surveyed geometry.

## 15. Compute acceptance metrics and decide what can be enabled

For every eligible isolated burst, preserve ground truth, acceptance/rejection,
predicted signed angle, ambiguity, SNR/coherence, calibration age, session, band,
sample range and processing/display timestamps. Define eligibility before testing;
never define it as “the windows the estimator accepted.”

Report both:

- **Accuracy conditional on acceptance:** median, P95 and maximum absolute angle
  error, signed bias and uncertainty calibration.
- **Coverage and failures:** accepted/eligible, rejected by reason, missed bursts,
  false valid bearings on controls, wrong source association and detection latency.

The initial proposed LOS gate is **median absolute error ≤5°, P95 ≤10° and
accepted coverage ≥90%**, for isolated eligible bursts at SNR ≥10 dB over ±60°.
Apply this to each declared band and report each angle/session; a pooled pass must
not conceal a failed cell. Proposed reference stability gates are in Steps 6–9.

Use session-aware confidence intervals, not a bootstrap that treats thousands of
adjacent overlapping windows as independent trials. Three sessions are a minimum
repeatability check; report uncertainty and collect more sessions if the interval
is too wide for the intended claim. Give numerators and denominators for all rates.

Do not hide front/back ambiguity by choosing whichever mirrored prediction is
closer to truth without reporting that scoring convention. The primary test uses
the declared front half-plane and signed broadside convention; the rear control
must explicitly retain both possible solutions.

For O4 classification, separately report precision, recall, false alarms per hour,
abstention rate and the labeling method. Freeze train/test splits by recording,
session and site; no overlapping windows across splits. A good angle from an
unknown RF burst is not a correct O4 classification.

### Hopping comes last

After fixed-frequency acceptance, test each intended channel and transition with
a frequency/configuration-keyed calibration bank only where Step 8 established
reuse. Measure tune settling, time off channel, missed bursts and per-channel
angle error. Do not reuse a 2.4 GHz calibration at 5.8 GHz. Repeat transport and
full GUI soak if rate, wire format or link changes.

## 16. Evidence format, stop conditions and completion checklist

For each step save a small report with this schema:

```text
test_id / date_utc / operator
git_head / dirty_patch_or_source_snapshot / executable_versions
radio_and_board_serials / profile / rig_and_cable_ids / source_settings
preconditions / requested_action / actual_configuration
session_id / config_epoch / calibration_id / reference_method
input_power_and_uncertainty / waveform / occupied_band / geometry_and_uncertainty
sample_ranges / raw_artifact_paths / hashes / log_paths
metric_values / numerator_denominator / thresholds / confidence_intervals
result: PASS | FAIL | BLOCKED | NOT_RUN
failure_reason / scope_of_validity / next_action
```

Keep compact CSV/JSONL results, phase/error plots, representative paired IQ,
manifests, calibration files and native screenshots. Record disk budget before
long tests. Keep failed-run artifacts; they explain the accepted operating limits.

Stop the affected phase test if power is unknown, either channel clips or unlocks,
samples become discontinuous, metadata is inconsistent, the rig changes, or phase
jumps cannot be explained. Fix/recalibrate and start a separately identified run.
Independent software work can continue while equipment or reference access is missing.

- [ ] Build and capture provenance are reproducible.
- [ ] Dual-channel transport and application pairing pass.
- [ ] Split-reference short-term stability and calibration hold-out pass.
- [ ] Drift/age, timed retuning and lifecycle policies are measured.
- [ ] Frequency response, skew and supported passband are characterized.
- [ ] Persistent paired live service and reference transfer are verified.
- [ ] Fault injection clears invalid bearings and preserves incomplete evidence.
- [ ] Ten-minute integrated paired RX/DSP/GUI soak passes.
- [ ] Antenna rig, sign, geometry and OTA reference are surveyed.
- [ ] Held-out field accuracy, coverage and control results meet the declared envelope.
- [ ] Decoded packet/bearing association is verified; O4 identity claims remain separate.
- [ ] Final report states limitations and supported settings; progress files are updated.

Only then enable operational live bearings for the accepted profile. Other bands,
rigs, gain/path settings and reuse policies stay explicitly unverified.

## Source and implementation notes

- [Ettus UHD 4.1 synchronization manual](https://files.ettus.com/manual_archive/v4.1.0.4/html/page_sync.html): timed starts/tunes and residual calibration; retrieved for this plan.
- [Ettus UBX specification](https://kb.ettus.com/UBX) and [handling guide](https://kb.ettus.com/UBX_Getting_Started_Guides): input limits and board properties. Direct page retrieval was blocked; official indexed excerpts confirmed the cited limits.
- Local commands were checked against the current `iq_record_pair`, `df_bearing`
  and `droneid_ui` help/source. Commands are procedures to run later; creating this
  document did not repeat RF tests or enable live DF.
- Relevant code: `tools/iq_record_pair.cpp`, `tools/df_bearing.cpp`,
  `src/sdr/usrp_source.cpp`, `src/o4/aoa.cpp`, `src/ui/receiver_worker.cpp`.
- Proposed persistent bench runner, phase-report analyzer, fault injection and
  latency instrumentation are requirements in this plan, not existing CLI features.
