# Experimental O4 candidate detector

Updated 28 October 2026. The receiver now has a signal-structure detector in
addition to energy detection. It finds a repeated structure observed in the
local Air 3S 5.8 GHz recordings without requiring payload decoding.

Its output is **`o4_candidate`**, not confirmed O4 identity, a model prediction,
unique-drone count, range or direction. The current profile is `o4_structure_v2`;
the original frozen `air3s_structure_v1` results are preserved separately.
Other OFDM systems can share these characteristics; distinguishing all O4/O4+
devices from all other transmitters remains an independent validation task.

## What changed

V2 retains the original strong-evidence path and adds a weak-evidence path:
lag correlation >0.015, lag prominence >3, cyclic correlation >0.01,
cyclic prominence >2 and harmonic correlation >0.005. Clipping must remain
≤0.001. The current window must match and at least **8 of 10 complete windows**
must support it, requiring **160 ms** of continuous data. This adds sensitivity
while demanding longer persistence and greater cyclic prominence. JSON records
`weak_structure_match` and `weak_supporting_windows` separately. Both histories
reset on gaps and every sweep retune. Strong-path fields keep their original meaning.

The added thresholds were selected after examining the three v1 external misses;
therefore rerunning those files is development/regression evidence. A separate
preselected set of files and archive members was evaluated after freezing v2.

## Current v2 results (28 October 2026)

| Evaluation | O4-labeled files with candidates | Clearly non-O4 controls | Scope |
|---|---:|---:|---|
| Local Air 3S/Mini 2/baseline, 187.009 s | 4/9 Air 3S recordings; 809 candidate windows | 0/7,187 windows over 115.009 s | Development sessions |
| External regression, 60 × 0.32 s | 11/14 | 0/540 windows in 27 controls | Inspected during v2 development |
| New files/members, 35 × 0.64 s | **7/8** | **0/960 windows in 24 controls** | File-held-out, not session-held-out |

The new set includes six Mini 4 Pro files (all six with candidates) and two
Avata 2 members (one with 29/40 candidate windows; the other with zero).
The three new O4+ files had zero candidates; O4+ is **not validated/supported**.
The external regression also includes four positive `mavic3S` files whose model
label is ambiguous; they remain `unverified/other`, not relabeled as correct O4.
The local 2.4 GHz Air 3S captures still have no candidates.

Artifacts: `data/analysis/o4_v2_local_20261028/`, `o4_v2_external_20261028/`,
and `o4_v2_holdout_20261028/` (under `data/analysis/`). The holdout manifest and
source/binary hashes were saved before evaluating its cases and were verified
unchanged afterward. Selection and reproduction: `scripts/validate_o4_holdout.py`.
No large IQ files were downloaded or original datasets modified.

**7/8 is recording-prefix coverage, not 87.5% drone identity accuracy.** Labels
indicate a drone was present in a recording, not exactly which samples contain
its signal. Related files can share sessions and devices. Candidate windows are
correlated and cannot be counted as drones. One Avata 2 miss, missed local bands,
O4+ and independent event-level false-alarm/recall testing remain open.

The remainder documents the original v1 development and its preserved results.

Previously, every energy burst was `unknown_rf`. Energy alone detects baseline
Wi-Fi too. `rf_analyze` retains that useful behavior and its RF metrics.

The new `O4CandidateDetector` examines continuous IQ in non-overlapping **16 ms**
windows, so it can find structured continuous links as well as bursts. It is
shared by `o4_detect`, the normal DJI GUI replay/live worker, and paired replay.
The detector runs on the DSP side of the live queue, not the radio receive loop.
It does not modify decoded telemetry, DF classification or ODID/ASTM parsing.

The GUI shows historical candidate-window counts separately from the drone table.
Candidate evidence never creates a serial, drone track or bearing. A reconnect
clears its counters. A sample discontinuity clears detector history and partial
windows. Remaining IQ shorter than 16 ms is not classified or padded.

## Evidence profile and decision

Development used the first **0.32 seconds** of the local captures. Their measured
lag/cyclic correlations suggested these hypotheses:

- A normalized autocorrelation peak near **33.333 microseconds** (rate / 30,000 samples).
- A cyclic correlation component at **28 kHz**, with a **56 kHz** harmonic.
- A localized peak relative to nearby lags, rather than high correlation at all
  lags from a tone or narrowband interferer.
- A localized cyclic component relative to nearby frequencies, rather than broad
  leakage from a single short transient.

These are empirical feature hypotheses, **not claimed universal O4 PHY constants**.
They were not taken as an identity guarantee from a protocol specification. No
filename, dataset label, absolute RF center, gain or absolute received power is
used in the decision.

The implementation removes the window mean and evaluates
`conj(x[n]) * x[n + lag]`. Correlations are normalized by the two segments' energy.
It compares lag magnitude against the median at offsets ±1 and ±2 microseconds,
and 28 kHz magnitude against the median at 26, 27, 29 and 30 kHz.

Frozen development thresholds for profile v1:

| Quantity | Required value |
|---|---:|
| Clipped sample fraction | ≤0.001 |
| Normalized lag correlation | >0.03 |
| Lag prominence | >3 |
| Normalized 28 kHz cyclic correlation | >0.02 |
| Cyclic prominence | >1.5 |
| Normalized 56 kHz harmonic | >0.01 |
| Temporal support | Current window matches, and ≥3 of the most recent 5 windows match |

Five complete windows are required before a candidate can appear: 80 ms of
signal, plus any delivery/worker latency. Correlation values are evidence metrics,
not confidence probabilities. Input rate is validated in [10,250] MSPS; synthetic
rate tests cover 11.52, 15.36 and 20 MSPS. Wider-band files are accepted but cost
more CPU and have not received equivalent identity/generalization validation.

Constant carrier frequency offset changes correlation phase rather than the
feature magnitude. Tests include a 1.2 MHz shift. Substantial clock mismatch,
frequency-selective interference, mixed transmitters, different link modes,
partial-band reception and changing numerology can still cause misses or false matches.

## Usage

```bash
cmake --build build -j 2
./build/o4_detect data/air3s/air3s_58_5805e6_20261026T074519Z.iq > /tmp/o4-evidence.jsonl
./build/o4_detect capture.cf32 --format cf32 --rate 15360000 --seconds 1
./build/droneid_ui --check --replay data/air3s/air3s_58_5805e6_20261026T074519Z.iq --secs 1
./build/droneid_ui --replay data/air3s/air3s_58_5805e6_20261026T074519Z.iq --secs 1
```

Sidecars override format/rate in the same way as the existing file tools. The
default CLI limit is the whole file. Every complete window produces JSONL with
sample coordinates, component metrics, clipping, structure match, persistence,
candidate status, reason and `identity_confirmed:false`. The final summary includes
unclassified tail samples and leaves identified-drone count/identity accuracy null.

To reproduce the complete local evaluation without downloading or changing IQ:

```bash
python3 scripts/validate_o4_detector.py --output data/analysis/o4_validation_repeat
```

This streams the C++ tool over all local Air 3S/Mini 2/baseline IQ, including the
separate 20-MSPS Wi-Fi baseline if available. It retains commands, metadata,
coverage, all window evidence, rejection counts, timing and a report. The exploratory
NumPy script `scripts/explore_o4_structure.py` is development tooling, not the
production detector or an independent accuracy benchmark.

## Measured local results

Full-file evaluation: **22 recordings, 187.009 seconds**. Logs and per-file results:
[`data/analysis/o4_validation_20261028/REPORT.md`](../data/analysis/o4_validation_20261028/REPORT.md).

| Capture group | Candidate windows / evaluated windows | Interpretation |
|---|---:|---|
| Air 3S, pinned video link at 5805 MHz | 280 / 500 | Structure detected repeatedly in the known active-link recording |
| Air 3S, 5745 MHz | 283 / 500 | Structure evidence; not independent event-level source identification |
| Air 3S, 5835 MHz | 230 / 500 | Structure evidence; not independent event-level source identification |
| Air 3S, 5775 MHz | 3 / 500 | Sparse candidate evidence |
| Air 3S, five 2.4 GHz files | 0 / 2,500 | No candidate; these congested captures cannot establish absence of the drone |
| Mini 2 controls plus no-drone/Wi-Fi baselines | 0 / 7,187 | No candidate false alarms observed over 115.009 seconds of these controls |

The 796 total candidate windows are correlated observations, not 796 drones or
independent detections. The positive labels are recording-level model presence;
there is no independent ground truth for every RF window. Thus 280/500 is **not
O4 recall**, and 0/7,187 is **not a guaranteed real-world false-positive rate**.

Development used the first 0.32 s of these same sessions. The report separately
counts candidates after that prefix, but those later windows are temporal extension,
not a held-out session/site/model test. No thresholds were tuned on the later full-file
evaluation. The external disk was unmounted during this initial run, then remounted;
the later external and v2 results are reported above. Independent sessions/sites and
event-level Wi-Fi/FPV ground truth remain required.

The C++ detector processed 187.009 s in approximately 47.43 s aggregate elapsed
tool time on this host. This is an offline detector-only measurement, not a GUI/live
pipeline throughput guarantee. The one-second pinned-link sample produces exactly
**62 windows, 51 structure matches and 48 candidates** in both CLI and GUI workers.

## Verification and remaining acceptance

- C++ tests exercise structured synthetic input at three rates, carrier offset,
  arbitrary input chunking, persistence reset on a gap, noise, AM tone, Wi-Fi-like
  OFDM, another OFDM numerology, clipping, invalid samples and short input.
- Worker integration tests require detector/GUI counts to agree across the worker's
  own overlapping decode windows and forbid a fabricated drone track.
- Release tests include the existing DJI, paired DF and ODID suites.
- Final release CTest: **17/17 suites pass**. Separate no-UHD/no-window
  AddressSanitizer + UndefinedBehaviorSanitizer build: **2/2 targeted suites pass**
  (`test_o4_detector`, `test_ui_worker`; leak detection disabled). Logs are in
  `data/analysis/o4_development_20261028/`.
- Native replay checks display evidence separately from empty identity/position tables.
- Later Windows-port checks used the connected X310 receive-only through the UHD
  C API adapter: zero radio overflows/gaps, but DSP drops were observed during
  concurrent build/evaluation work. Full-load/recovery acceptance remains open;
  see `data/analysis/windows_20261028/REPORT.md`. No transmission was performed.

To graduate from candidates to a validated identity detector:

1. Expand the completed external inventory with verified event labels and additional
   O4/O4+ waveform modes; preserve the frozen v1/v2 results.
2. Establish independent sessions/sites/models for development and held-out
   evaluation; never split overlapping windows across those groups.
3. Add controlled O4-on/O4-off, Wi-Fi-only, other-drone and mixed-source tests with
   event-level annotations. Preserve ambiguous examples as unknown.
4. Evaluate the frozen feature profile and any subsequent classifier on those
   groups, reporting false alarms/hour, coverage, precision/recall where labels
   support them, abstentions and performance versus SNR/channel width.
5. Investigate alternative waveform modes for the missed bands/models rather
   than lowering thresholds until all energy is labeled O4.
6. Recheck complete GUI/live CPU load and gap handling; keep DF calibration and
   O4 identity acceptance separate.

The absence of a candidate is not evidence that no drone is present.
