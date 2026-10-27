# Direction-finding completion plan

Updated 2026-10-27. Phase 6 follows `progress.md`, not the original build-plan numbering.
This is an implementation and acceptance plan, **not a claim of hardware completion**.
The operator has deferred X310/drone access. Work through the software gates now;
do not enable live bearings until the hardware gates pass.

## Deliverable and limits

Deliver a receive-only X310/two-UBX system that displays a calibrated, time-stamped
**signed angle relative to antenna-pair broadside** for an isolated received burst,
with quality, ambiguity, calibration age and rejection reason. Attach a decoded
serial only when the same burst supplies validated telemetry. For undecoded
signals, show an RF event, not an invented drone identity. O4 classification is a
separate prerequisite for labeling that event O4, not for estimating its angle.

One two-element baseline cannot provide unambiguous 360-degree azimuth, elevation,
range or GPS position. The measured phase gives a direction-cosine projection;
without a plane/elevation constraint its solutions form a cone. A horizontal-plane
display must explicitly retain front/back ambiguity. Resolving that ambiguity
requires another independent baseline, controlled rotation with suitable source
stability, or independently validated directional-antenna information. A later
multi-station position solution is outside this phase.

The current scalar estimator uses `sum(conj(ch0) * ch1)` and
`asin(wrapped_phase * wavelength / (2*pi*spacing))`. Confirm physical channel
polarity with a source at a known positive angle; do not infer orientation from
cable labels alone. Near endfire, small phase errors cause large angle errors.

## Existing code versus missing work

| Component | Present | Gap before acceptance |
|---|---|---|
| `src/sdr/usrp_source.cpp` | One multi-channel stream, common timed tune/start | LO-lock checks, verified clock/sync configuration, explicit stream/session epochs |
| `tools/iq_record_pair.cpp` | Paired cf32, continuity checks, metadata | Session/rig provenance, bounded writer queue, transactional completion manifest, interruption handling |
| `src/o4/aoa.cpp`, `include/droneid/aoa.h` | Scalar phase calibration, coherence/freshness/spacing guards | Session validation, uncertainty, clipping/SNR gates, differential delay and burst association |
| `tools/df_bearing.cpp` | Offline scalar calibration and angle | `--aligned` is an operator assertion, not independently verified alignment; CLI currently passes zero timestamps to estimator |
| `src/ui/receiver_worker.cpp` | Live single-channel decoder; DF fails closed | Persistent paired worker and same-session calibration state machine |
| `src/ui/app.cpp`, `src/ui/ui_model.*` | Direction display primitives | Validity/ambiguity/status presentation and verified paired replay |
| `tests/test_receiver.cpp` | Synthetic angle/phase wrap/rejection tests | Paired pipeline, lifecycle/fault injection and real measured angle evidence |

The recorder currently writes synchronously in the receive loop. Its two renames
and metadata writes are not an atomic pair transaction; interruption can publish
a shorter capture. Do not treat its existence as a loss-proof recording service.
Existing mono-channel datasets can exercise detection/decoding, **not physical
AoA accuracy**. Deriving a second channel synthetically from them only tests software.

## Important synchronization correction

UBX on X300/X310 supports phase synchronization through timed tuning; it is not
correct to say every UBX retune necessarily produces an unrecoverable random
phase. Timed stream starts also align DSP CORDICs. Residual phase depends on
frequency and drifts, so calibration remains necessary. The application must
invalidate calibration after unverified retunes/reopens until repeatability on
the actual rig is measured. Hardware capability is not an application guarantee.
See [Ettus synchronization guidance, UHD 4.1](https://files.ettus.com/manual_archive/v4.1.0.4/html/page_sync.html).

Audit installed UHD 4.1.0.5/FPGA and actual daughterboard/master-clock settings
before relying on timed UBX synchronization. The current configuration is
184.32 MHz master clock / 15.36 MSPS. Check the UBX driver's clock constraints and
the actual achieved RF/DSP tunes; do not assume that integer sample-rate decimation
alone proves LO synchronization. If a different supported clock/rate is needed,
resample both channels together using identical phase/state. Do not silently
change the established single-channel decoder configuration.

For one X310, use the motherboard's common timebase for both RX channels; external
PPS is not a substitute for RF-phase calibration. Multiple motherboards would
require a separately designed reference/time distribution system.

## Execution order and acceptance gates

### DF-1 — Paired data contract and deterministic replay (software now)

- [ ] Introduce a paired block containing equal-length channel vectors, device
  start tick/sample index, actual rate/frequency, session ID, configuration epoch,
  channel order and gap/overflow flags. A missing device timestamp is invalid,
  not a fabricated zero timestamp.
- [ ] Add a versioned capture manifest with radio/daughterboard IDs, UHD/FPGA
  versions, clock/time sources, actual gains/bandwidth/antennas/tunes, rig/cable IDs,
  antenna spacing/orientation, first device time, sample count and checksums.
  Host wall time is freshness metadata, not sample alignment evidence.
- [ ] Teach paired replay/`df_bearing` to validate the manifest and sample counts.
  Keep legacy `--aligned` available only as explicitly unverified diagnostic input;
  never promote it to live-ready calibration provenance.
- [ ] Make recorder completion a manifest-last transaction: both files and
  sidecars must be closed successfully before publishing a complete manifest.
  Cancellation, disk errors or overflow leave incomplete artifacts with reasons.
- [ ] Add synthetic paired fixtures: known angles, phase bias, noise, clipping,
  sample skew, channel swap, delay slope, timestamp gaps, missing metadata,
  unequal EOF, chunk boundaries and reconnects. Use small generated fixtures.

**Gate:** deterministic chunk-independent replay; malformed pairs never yield a
valid bearing. Existing decoder/UI suites remain green. No hardware needed.

### DF-2 — Persistent calibration lifecycle (software now, bench later)

- [ ] Implement `Disconnected → Uncalibrated → Calibrating → Valid`; transitions
  to `Stale` or `Error` carry reasons. The radio remains open from reference
  measurement through target reception. UI and CLI use the same service.
- [ ] Bind calibration to session/configuration epoch and rig identity, RF center,
  effective passband, actual rate, gain settings, channel order and reference.
  Reconnect, unverified tune, rate/gain/antenna/cable/reference changes, missing
  time, stream discontinuity, lock loss or expiration suppress bearings.
  A later validated recovery policy may distinguish sample gaps from LO changes.
- [ ] Capture a splitter-fed reference in the same session, verify both LO locks,
  adequate power, no clipping, stable phase and coherence. Reject unstable
  reference windows; store sample count, phase dispersion and validity limits.
- [ ] Support a known-broadside OTA reference as a separately labeled method.
  A splitter characterizes electronics/cables, not antenna coupling/patterns.
  Switching reference to antennas must preserve the session and use characterized
  switch/cable paths; unplug/replug is not automatically phase-repeatable.
- [ ] Initially retain scalar calibration only for its measured narrowband range.
  For wider signals measure differential group delay and phase versus frequency;
  correct the complex response or reject windows outside the calibrated passband.
  No arbitrary center-tone correction for unvalidated wideband O4 data.

**Gate:** simulated lifecycle/fault tests prove fail-closed behavior; physical
reference stability remains explicitly unverified until DF-5.

### DF-3 — Shared paired DSP and burst association (software now)

- [ ] Keep both channels in one bounded queue. Any queue drop rejects the whole
  pair and resets both processing paths; never drop channels independently.
- [ ] Use one frequency/centering decision and matched DDC/filter/resampler state.
  Do not run independent auto-centering, CFO corrections or AGC on the channels.
  Share the sample clock/index through every processing stage.
- [ ] Find an isolated burst once and extract precisely the same interval from
  both channels. Associate a bearing with the decoder's original sample range,
  not simply the most recent serial. Carry CFO/detector and angle quality separately.
- [ ] Add SNR, clipping, coherence, phase dispersion, passband validity and
  uncertainty gates. Account for endfire sensitivity; smoothing must reset on
  gaps/source changes and must not hide ambiguous/multiple emitters.
- [ ] Raw O4-candidate events can receive an angle without payload decoding;
  retain `unknown_rf` until independently validated identity evidence exists.
  High coherence alone does not prove a single source or an O4 drone.

**Gate:** paired synthetic and phase-shifted real-IQ software tests preserve
known angles across chunking and reject injected faults. These tests do not
count toward measured RF angular accuracy.

### DF-4 — GUI integration and operational logging (software now)

- [ ] Connect the paired service to `UiModel` without blocking render/receive
  threads. Add paired replay, Calibrate/Cancel controls and reference instructions.
- [ ] Show measured angle with mirrored/ambiguous solution, quality, calibration
  state/age, band, rig orientation and timestamp. Hide stale arrows on disconnect,
  configuration changes, failed calibration or loss of an associated event.
- [ ] Emit JSONL events with burst ID/sample interval, session/calibration IDs,
  angle, uncertainty, ambiguity and rejection reason. Export aggregate rejection
  counts, not only accepted estimates. No GPS dot derived from an angle alone.
- [ ] Exercise lifecycle controls, shutdown/reconnect, invalid configuration and
  backpressure in worker tests and native GUI replay. Live DF remains disabled
  by default until the bench profile is approved.

**Gate:** headless CLI/UI output agrees for paired replay; native display shows
invalid/ambiguous states honestly. DF-1 through DF-4 are the next coding sequence.

### DF-5 — Hardware synchronization and calibration bench (deferred)

Required: X310 with two supported UBX RX chains, known rig/cables, suitable
splitter/reference source and attenuators, and safe measured input levels.
Keep application operation RX-only. UBX's documented maximum input is **−15 dBm**;
use substantial margin (initial target at most −35 dBm at each receiver input),
verify splitter/attenuator losses and avoid overload. Confirm `lo_locked` for
both channels rather than treating an internal reference setting as RF lock.
See [UBX specifications](https://kb.ettus.com/UBX).

- [ ] Record configuration/version/sensor inventory; measure supported clock and
  timed-sync behavior before selecting the DF operating profile.
- [ ] With a split off-center tone, measure residual differential phase mean and
  standard deviation over time, warmup and gain settings. Start with a proposed
  ≤1° phase standard-deviation gate on a clean high-SNR reference, not an angular
  accuracy claim. Adapt the method in [Ettus phase-alignment tests](https://files.ettus.com/manual/page_rdtesting.html)
  to this one-board/two-channel rig.
- [ ] Test at least 100 same-frequency timed-retune cycles and repeated
  start/stop/reopen/power-cycle cases separately. Store every residual and failure.
  Only permit calibration reuse for the exact lifecycle/configurations proven
  repeatable; otherwise recalibrate. No calibration bank across untested bands.
- [ ] Run a 10-minute continuous receive/DSP/UI soak, with no overflow, sample
  gaps or paired queue drops. Stress disk/backpressure separately and verify
  visible invalidation, never a silent plausible bearing.

At 15.36 MSPS, two sc8 channels require **61.44 MB/s (491.52 Mb/s)** before network
overhead; two sc16 channels require 122.88 MB/s (983.04 Mb/s), too close to 1GbE
capacity once overhead is included. Host cf32 pairs are 245.76 MB/s; ten minutes
of continuous disk capture is **147.456 GB**. Use bounded diagnostic snippets and
summary logs for the soak, not mandatory full-rate archival. Actual throughput
and CPU capacity must be measured. 10GbE/wider-band work is a later profile.

**Gate:** documented stable calibration and continuity on the actual rig. Any
failed clock/synchronization/thermal condition blocks live bearing acceptance.

### DF-6 — Antenna field acceptance, then hopping (deferred)

- [ ] Build a rigid matched-polarization antenna pair and measure phase-center
  baseline/orientation. Require spacing ≤ half the shortest wavelength used.
  At 2.4595 GHz this is about 61 mm; at 5.7965 GHz about 25.9 mm. A 50 mm baseline
  suits 2.4 GHz only; a 25 mm baseline can cover both example bands but has less
  phase sensitivity at 2.4 GHz. Verify antenna coupling and bandwidth on the rig.
- [ ] Use surveyed far-field LOS source angles at −60°, −45°, −30°, −15°, 0°,
  +15°, +30°, +45°, +60°, with measured rig/source orientation and elevation.
  Test reversed channel order and the mirrored half-plane explicitly.
- [ ] Collect at least 100 isolated bursts per angle/band in at least three
  independent calibrated sessions, including an O2/O3 decoded source and an
  undecoded O4-labeled source when available. Include Wi-Fi/no-drone and
  two-emitter/multipath controls; these are not valid single-source angle truth.
- [ ] Proposed initial LOS targets: median absolute error ≤5°, 95th percentile
  ≤10° over ±60° at SNR ≥10 dB, and ≥90% accepted coverage of eligible isolated
  bursts. Report per-angle/band/session results, rejected counts, false valid
  bearings on controls, confidence intervals and latencies. Freeze thresholds
  before held-out evaluation; targets are not achieved measurements.
- [ ] Only after fixed-channel acceptance, integrate timed hopping and a
  frequency/configuration-keyed calibration bank whose reuse passed DF-5.
  Measure settling and coverage gaps. Re-test each frequency and report coverage
  separately from angle accuracy; missing a transmission is not a correct angle.

**Completion gate:** reproducible bench/field report, retained bounded paired
evidence with provenance, GUI/backend agreement, fault recovery, measured error
envelope and documented operating limits. Unique 360° bearing remains outside
the two-antenna guarantee.

## Parallel dependency: honest O4 validation

The saved corpus proves RF activity, not O4 classification accuracy. `INFP`
records occur in Mavic 3 (O3+) and Mini 5 Pro (O4+) captures, so that marker cannot
be an O4-only rule. Build an event-level labeled set with O4 and Wi-Fi/other-drone
controls, split by recording/session/site (never overlapping windows from the
same capture across train/test), freeze a classifier and report precision,
recall, false alarms per unit time and abstention coverage on held-out data.
O4+ stays a separate class. Dataset-level model labels are not per-burst ground
truth. O3+ payload schema work must likewise stay separate from measured angle.

## Documentation and evidence policy

For each milestone update `progress.md`, `TEST_PLAN.md`, `docs/GUI.md` and this
checklist. Store commands, versions/configuration, test outputs and small
diagnostic artifacts under a dated `data/analysis/df_*` directory. Preserve
source IQ read-only; do not download large captures. Mark software gates,
hardware bench gates and operational acceptance separately. Do not replace an
unmeasured result with an expected value or set a deadline dependent on unavailable hardware.
