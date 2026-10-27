# punjdrone — Direction-Finding Software Gates Progress (`df_argon.md`)

Updated: 2026-10-27. Tracks completion of the software-first direction-finding gates (**DF-1** through **DF-4**) defined in `docs/DIRECTION_FINDING_PLAN.md` while keeping live hardware bearings fail-closed until bench (**DF-5**) and field (**DF-6**) acceptance gates pass on the physical X310 / dual-UBX-160 rig.

Compatible with `UiModel::load_checklist_file("df_argon.md")` and `./build/droneid_ui --checklist df_argon.md`.

## Gate Summary — Software Complete, Hardware Deferred

| Gate | Scope | Status | Verification Evidence |
|---|---|---|---|
| **DF-1** | Paired data contract, manifest-last recorder, deterministic replay | **Completed (Software)** | `DfGate1.PairedBlockAndManifestTransaction`, `df_bearing --manifest` CLI roundtrip |
| **DF-2** | Persistent calibration lifecycle (`Disconnected → Uncalibrated → Calibrating → Valid → Stale/Error`) | **Completed (Software)** | `DfGate2And3.CalibrationLifecycleAndQualityGates`, session/epoch/rig/LO-lock enforcement |
| **DF-3** | Shared paired DSP, bounded paired queue, isolated burst bearing association | **Completed (Software)** | `UiWorker.PairedQueueDropsWholePairNeverSingleChannel`, `UiWorker.PairedChunkIndependenceAndPassbandRejection` |
| **DF-4** | GUI integration, stale-arrow invalidation, operational JSONL event & rejection logging | **Completed (Software)** | `UiModelDf.InvalidationClearsStaleArrowsAndTracksRejectionReasons`, `UiWorker.PairedReplayAssociatesDecodedAndUnknownRfBearingsAndRejectsMalformedPairs` |
| **DF-5** | Hardware synchronization, timed-retune repeatability, 10-minute 1GbE `sc8` soak | **Deferred (No Hardware)** | Requires X310 (`326CF02`) + 2× UBX-160 + external splitter/attenuated source |
| **DF-6** | Surveyed LOS antenna field calibration, error envelope, hopped DF bank | **Deferred (No Hardware)** | Requires rigid matched-polarization antenna baseline and outdoor LOS range |

## DF-1 — Paired Data Contract and Deterministic Replay

- [x] Introduce `PairedBlock` (`include/droneid/aoa.h`, `src/o4/aoa.cpp`) with equal-length `ch0`/`ch1` vectors, mandatory positive `first_device_timestamp_ns` (missing or `<= 0` timestamps are rejected rather than fabricated as zero), `start_sample`, `sample_rate`, `center_hz`, `session_id`, `config_epoch`, `rig_id`, `channel_order`, and `has_gap`/`has_overflow` flags.
- [x] Implement versioned `PairManifest` (`schema_version = 1`) recording `complete` flag, `status_reason`, `session_id`, `config_epoch`, `rig_id`, `cable_id`, `radio_serial`, daughterboard IDs, clock/time sources, per-channel gains/antennas/order, `spacing_m`, `orientation_deg`, `first_device_timestamp_ns`, `capture_host_unix_ns` (freshness metadata only), `sample_count`, `overflows`, `discontinuities`, and 64-bit FNV-1a file checksums (`ch0_fnv1a64`, `ch1_fnv1a64`).
- [x] Upgrade `tools/df_bearing.cpp` and `SidecarMeta::load` to validate `PairManifest` (`--manifest PATH`) as well as `.meta` sidecar timestamps, sample counts, channel indices, and equal on-disk file sizes, while retaining `--aligned` explicitly as an `"unverified_aligned_assertion"` diagnostic mode that is never promoted to `"manifest_verified"` provenance.
- [x] Harden `tools/iq_record_pair.cpp` with a bounded writer queue (`kMaxWriterQueue = 64`) separating UHD RX from disk I/O, streaming 64-bit FNV-1a checksum computation, and a manifest-last transaction that writes `PREFIX.pair.toml` (`complete = true`) only after both `.partial` data files and `.meta` sidecars close and rename cleanly (publishing `PREFIX.pair.incomplete.toml` with the failure reason on interruption, timeout, gap, overflow, or disk backpressure).
- [x] Verify DF-1 against synthetic and real-IQ paired fixtures covering known angles, phase bias, clipping, sample skew, channel swap, delay slope, timestamp gaps, missing metadata, unequal EOF, and corrupted checksums.

## DF-2 — Persistent Calibration Lifecycle

- [x] Implement `DfCalState` (`Disconnected → Uncalibrated → Calibrating → Valid`, with explicit reason-carrying transitions to `Stale` or `Error`) and `DfCalibrationService` in `include/droneid/aoa.h` and `src/o4/aoa.cpp`, shared across CLI and UI workers.
- [x] Bind `PhaseCal` and `DfCalibrationService` to `session_id`, `config_epoch`, `rig_id`, `center_hz`, `passband_hz`, `sample_rate`, `gain_db`, `channel_order`, `reference_method`, and dual-channel `lo_locked` state; automatically transition to `Stale` or `Error` and suppress bearings on retune (`on_retune`), configuration change (`on_config_change`), sample gap/overflow (`on_gap_or_overflow`), LO unlock (`on_lo_unlock`), disconnect (`on_disconnect`), or calibration age expiration.
- [x] Validate same-session reference windows in `complete_calibration`: require both LOs locked, minimum mean power (`>= 1e-7`), zero clipping (`clipped_fraction == 0`), high coherence (`>= 0.98` for `"splitter"`, `>= 0.92` for `"ota_broadside"`), sub-window circular phase dispersion `<= 0.087 rad` (~5°), and passband differential group-delay slope `<= 0.35 rad`.
- [x] Support both `"splitter"` (`--splitter-calibrate`) and `"ota_broadside"` (`--ota-calibrate`) reference methods as distinct labeled provenance modes in `PhaseCal`, `df_bearing`, and `droneid_ui`.
- [x] Enforce passband validity and linear delay-slope phase compensation in `estimate_bearing` and `DfCalibrationService::estimate_block_bearing`, rejecting bursts whose spectral centroid falls outside the calibrated passband.

## DF-3 — Shared Paired DSP and Burst Association

- [x] Implement `PairedWindowAssembler` and `BoundedPairedQueue` (`src/ui/receiver_worker.h`, `src/ui/receiver_worker.cpp`) so `ch0` and `ch1` are queued and windowed as an indivisible pair; queue backpressure drops the oldest pair as a unit, increments `dropped_pairs`, and flags `after_drop_or_gap` on the next window so downstream state resets cleanly.
- [x] Implement `dji::decode_paired_samples` (`include/droneid/dji.h`, `src/dji/decode.cpp`) using a single frequency/auto-centering decision on `ch0` and matched `dsp::ChannelResampler` instances on `ch0` and `ch1` with shared sample clocks—never running independent auto-centering, CFO corrections, or AGC across channels.
- [x] Extract identical `[start_sample, end_sample)` intervals from both channels for every decoded DroneID burst and associate calibrated bearings with the burst's sample range and decoded serial track while keeping ZC correlation/CFO metrics separate from angle coherence/uncertainty.
- [x] Enforce SNR (`min_snr_db = 6.0`), receiver clipping (`<= 0.1%`), inter-channel coherence (`>= 0.80`), 4-subblock circular phase dispersion (`<= 0.35 rad`), temporal burst isolation (`>= 64` quiet samples between energy bursts, rejecting truncated/overlapping bursts as `"overlapping_or_multipath_burst"`), and endfire-scaled angular uncertainty ($\sigma_\theta \propto 1/\cos\theta$).
- [x] Estimate and attach bearings to isolated undecoded RF bursts (`classification = "unknown_rf"`) without fabricating a drone serial, O4 classification, or GPS coordinate.

## DF-4 — GUI Integration and Operational Logging

- [x] Extend `UiModel` (`src/ui/ui_model.h`, `src/ui/ui_model.cpp`) with `DfUiStatus`, `DfBearingEvent`, `set_bearing_detailed`, `invalidate_bearings`, and `df_summary_jsonl` without introducing backend dependencies into `droneid_ui_model`.
- [x] Wire `paired_replay_worker` and `src/ui/app.cpp` (`draw_direction`, `draw_drones`, `--check`) to display calibration state (`Disconnected`, `Uncalibrated`, `Calibrating`, `Valid`, `Stale`, `Error`), reference method (`splitter` vs `ota_broadside`), calibration age, rig ID/epoch/orientation, primary broadside needle (scaled by coherence), mirrored front/back ambiguity needle (`180° - θ`), and angular uncertainty (`+/- deg`), while immediately hiding stale arrows on disconnect, cancellation, or invalidation.
- [x] Emit per-burst `df_bearing` JSONL records and aggregate `df_summary` JSONL rejection breakdowns (`accepted_bearings`, `rejected_bearings`, `rejected_uncalibrated`, `rejected_coherence`, `rejected_clipping`, `rejected_dispersion`, `rejected_snr`, `rejected_passband`, `rejected_overlap`, `dropped_pairs`) via `--df-jsonl FILE` and `droneid_ui --check`, with `"absolute_position":null` on all records.
- [x] Verify CLI/UI agreement across `df_bearing` and `droneid_ui --check` on paired fixtures while keeping live hardware DF (`--device ... --df`) fail-closed until **DF-5** bench approval.

## DF-5 & DF-6 — Deferred Hardware Bench and Field Gates

- [ ] **DF-5 (Bench):** Verify X310 (`326CF02`) + 2× UBX-160 `lo_locked` sensors, measure split-tone residual phase stability ($\le 1^\circ$ std dev target at high SNR with $\le -35\text{ dBm}$ input per port), run 100 timed-retune repeatability cycles, and complete a 10-minute two-channel `sc8` receive/DSP/UI soak with zero unhandled overflows or silent bearing errors.
- [ ] **DF-6 (Field):** Survey a rigid matched-polarization antenna baseline ($\le \lambda/2$), collect $\ge 100$ isolated bursts per angle across $-60^\circ \dots +60^\circ$ in $\ge 3$ independent sessions (plus Wi-Fi/multipath controls), verify $\le 5^\circ$ median / $\le 10^\circ$ P95 LOS error over $\pm 60^\circ$, and only then enable live GUI bearings and calibrated hop-bank reuse.

## Modified Files & Verification Summary

| File | Summary of Non-Breaking Changes |
|---|---|
| `include/droneid/aoa.h`, `src/o4/aoa.cpp` | Added `SidecarMeta`, `PairedBlock`, `PairManifest`, 64-bit FNV-1a helpers, `DfCalState`, `DfCalibrationService`, and extended `PhaseCal` / `Bearing` (mirrored angle, endfire uncertainty, clipping & phase-dispersion gates). |
| `include/droneid/dji.h`, `src/dji/decode.cpp` | Added `PairedDecodedBurst` and `decode_paired_samples` for shared-centering, matched-resampler paired burst extraction. |
| `src/dji/dsp.cpp` | Preserved `fftw3f` path when `<fftw3.h>` is present and added an exact radix-2 / Bluestein FFT fallback when `<fftw3.h>` is absent so headless environments can compile and run all DSP/decoder suites. |
| `tools/df_bearing.cpp` | Added `--manifest`, `--ota-calibrate`, `--session-id`, `--config-epoch`, `--rig-id`, sidecar timestamp/count/overflow validation, and `mirrored_deg`/`uncertainty_deg`/`provenance` output fields. |
| `tools/iq_record_pair.cpp` | Added bounded writer queue (`64` blocks), streaming FNV-1a checksums, and manifest-last transactional completion (`PREFIX.pair.toml` vs `PREFIX.pair.incomplete.toml`). |
| `src/ui/ui_model.h`, `src/ui/ui_model.cpp` | Added `DfUiState`, `DfUiStatus`, `DfBearingEvent`, `set_bearing_detailed`, `invalidate_bearings`, and JSONL event/summary serializers. |
| `src/ui/receiver_worker.h`, `src/ui/receiver_worker.cpp` | Added `PairedSampleWindow`, `PairedWindowAssembler`, `BoundedPairedQueue`, and `paired_replay_worker` associating bearings with decoded DroneID bursts and isolated `unknown_rf` bursts. |
| `src/ui/app.cpp` | Added DF calibration state/age/method/counter display, splitter/OTA/cancel buttons, mirrored ambiguity needle, stale-arrow clearing, and `--pair-manifest` / `--replay-ch1` / `--df-cal-samples` / `--df-jsonl` flags. |
| `scripts/validate_corpus.py`, `tests/test_corpus_harness.py` | Guarded optional top-level `h5py` import so non-HDF5 scripts (`investigate_o1.py`, `audit_packet_envelopes.py`) and unit tests run when `h5py` is not installed. |
| `tests/test_receiver.cpp`, `tests/test_ui_model.cpp`, `tests/test_ui_worker.cpp`, `tests/test_*.cmake` | Added unit and integration tests for DF-1 through DF-4 while keeping all existing test cases 100% green. |

### Executed Test Suites (2026-10-27)

| Suite | Cases | Result | Coverage |
|---|---:|---|---|
| `test_ring_buffer` | 5 | **5/5 PASS** | SPSC lock-free ring buffer wrap, clamp, threaded lossless streaming |
| `test_file_source` | 3 | **3/3 PASS** | `cf32`, `sc16`, partial block handling |
| `test_config` | 2 | **2/2 PASS** | TOML-subset loader and missing-file rejection |
| `test_receiver` | 5 | **5/5 PASS** | Hop schedule + `Direction` + **DF-1** manifest/block + **DF-2/DF-3** lifecycle & quality gates |
| `test_ui_model` | 8 | **8/8 PASS** | Checklist parser + packet/track ring + **DF-4** stale-arrow invalidation & JSONL counters |
| `test_resampler` | 4 | **4/4 PASS** | Streaming continuity (15.36/50/61.44/92.16 MSPS), alias rejection, passband preservation |
| `test_rf_metrics` | 3 | **3/3 PASS** | Tone metrics, chunk-independent `EnergyBurstDetector`, invalid input guards |
| `test_dji` | 12 | **12/12 PASS** | Turbo, scrambler, integer CFO, real Mini 2 (`mini2_centered.sc16` & `mini2_wideband.cf32`), `v2`/`v3`/`infp`/`cryp` envelopes |
| `test_ui_worker` | 9 | **9/9 PASS** | Replay worker + **DF-3** `BoundedPairedQueue` + **DF-3/4** paired Mini 2 & `unknown_rf` bearings + chunk independence + passband rejection |
| CLI & Python checks | 14 | **14/14 PASS** | `droneid_decode`, `droneid_hop`, `rf_analyze`, `df_bearing`, `droneid_ui --check`, `audit_packet_envelopes.py` (22/22 `OK`), `test_corpus_harness.py`, `test_o1_diagnostics.py` |

## Usage Reference (Software DF Gates)

```bash
# 1. Calibrate from a manifest-verified paired capture (splitter or OTA broadside):
./build/df_bearing --manifest /tmp/paired.pair.toml --splitter-calibrate \
  --samples 16000 --spacing-m 0.05 --measured-unix-ns 1700000000000000000 > /tmp/phase-cal.toml

# 2. Estimate bearing on a target window in the same session:
./build/df_bearing --manifest /tmp/paired.pair.toml --cal /tmp/phase-cal.toml \
  --start-sample 16000 --samples 8000

# 3. Run paired DF replay through the UI worker (headless check + JSONL export):
./build/droneid_ui --check --pair-manifest /tmp/paired.pair.toml \
  --df-cal-samples 4096 --df-ref splitter --df-jsonl /tmp/df-events.jsonl

# 4. Launch native GUI with df_argon.md checklist and paired replay:
./build/droneid_ui --pair-manifest /tmp/paired.pair.toml \
  --df-cal-samples 4096 --checklist df_argon.md
```
