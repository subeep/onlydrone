# UI/backend and local-IQ verification — 26–27 October 2026

## Acceptance summary

**Offline UI/backend verification passes. Full family/live acceptance does not yet pass.**

| Requirement | Result |
|---|---|
| Build native C++ GUI and exercise actual backend | Passed, including native framebuffer inspection |
| O2 decoding | CRC-valid Mini 2, Mavic Air 2 and Mini 3 payloads; supported v2 telemetry parsed |
| O3 decoding | Air 2S: two CRC-valid v2 payloads; Mini 3 Pro selected captures yielded none |
| O3+ decoding | Mavic 3 and Mavic 3 Pro: two CRC-valid payloads each, but unsupported telemetry formats |
| O1 decoding | Follow-up passed: Tampere Mavic Pro full one-second file, explicit channel shift, one CRC-valid v2 packet |
| O4 identification accuracy | Not established; backend emits unknown RF, not O4 predictions |
| Live X310 + transmitting drone | Deferred explicitly by operator; no radio accessed in this run |
| GUI direction finding | Disabled until valid same-session calibration is integrated |

No large IQ files were downloaded. Existing external data was read without modifying it.
The external sda1 partition became unmounted after the sweeps completed, then was remounted by the
operator on 27 October. O1 follow-up succeeded; see `data/analysis/o1_followup_20261027/REPORT.md`.

## Envelope and DF follow-up — 27 October

The saved quick/deep/O1 reports contain **22 unique transport payloads**, all of which also pass
the inner CRC16 over the declared logical length. `data/analysis/envelopes_20261027/REPORT.md`
lists per-model formats: v2 (79/91 bytes), v3 (112), INFP (131/138), auxiliary type 0x11 (144).
INFP appears in Mavic 3/O3+ and Mini 5 Pro/O4+ data: it is not an O4-only marker.
These checks strengthen corruption detection but neither authenticate identity nor establish flight truth.
Unsupported formats still have no invented serial/GPS. No O3+ telemetry/decryption claim is added.

CLI/UI now expose envelope format and inner-checksum status; v2 parsing requires a recognized
logical length and valid inner checksum. Rebuilt release: 12/12 CTest; Python corpus/O1: 6/6 + 2/2.
Nine retained clips (eight deep-corpus plus O1) match exact payloads and envelope diagnostics through
both executables. This replays bounded retained IQ, not a new full-corpus sweep.

`docs/DIRECTION_FINDING_PLAN.md` defines software, bench and field gates. It corrects the earlier
overbroad retune statement: UBX/X310 supports timed LO synchronization, but reuse of calibration
remains unverified on this rig. GUI DF stays disabled; no radio was accessed in this follow-up.

## Coverage and reproducibility

- External inventory: **808 data/archive files, 593.18 GB decimal**. Inventory is not test coverage.
- Quick sweep: **76/76 cases**, zero runner errors, up to 0.1 seconds per case; 7.419 seconds total.
- Deeper sweep: **45/45 cases**, zero runner errors, up to 1.4 seconds per case; 35.600 seconds total.
- These sweeps overlap: **76 distinct source paths**, about **38.519 seconds** of unique prefix coverage.
  Archive coverage is the selected first MAT/IQ member only, not every member. Some selected files
  were shorter than the cap and were read completely. No whole-dataset or statistical coverage claim.
- Input formats/rates were checked against local scripts and metadata. Family labels are not IQ-derived;
  corrected Mini 3/O2 and Mini 5 Pro/O4+ mappings are documented in `MODEL_FAMILIES.md`.
- Reports retain original paths, member names, clip hashes, rates/centers, tested durations, every
  centering pass and decoder/RF logs. All counts below are observed results, not expected transmit counts.

Commands (run from the repository with NumPy/h5py and unrar available):

```bash
python scripts/validate_corpus.py --output data/analysis/corpus_20261026 --seconds 0.1 --per-group 2 --jobs 2
python scripts/validate_corpus.py --output data/analysis/corpus_deep_20261026 --seconds 1.4 --per-group 1 --jobs 2 --datasets DroneRFa RFUAV KULeuven Tampere
python scripts/validate_corpus.py --output data/analysis/corpus_20261026 --refresh-report
python scripts/validate_corpus.py --output data/analysis/corpus_deep_20261026 --refresh-report
python scripts/verify_ui_corpus.py data/analysis/corpus_deep_20261026
```

The working Python environment used here is `/home/sudeep/Pictures/modelv1/.venv/bin/python`.
`--refresh-report` corrects report labels/payload interpretation without rereading IQ or rerunning DSP.

## Decoded packet evidence

| Source/model | Family label | CRC-valid distinct payloads | Supported-v2 serial | Interpretation |
|---|---|---:|---|---|
| Tampere Mavic Pro (27 October follow-up) | O1 | 1 | `08RDE310010218` | Full-file explicit shift; exact CLI/UI match |
| RUB Mini 2 | O2 | 8 (10 packet detections) | `SysSecWasHere ` | Existing reference; all 10 rows reach UI |
| RUB Mavic Air 2 | O2 | 1 | `1WNBH3900201N1` | Existing reference; UI count 1 |
| KU Leuven Mini 2 RC | O2 | 1 | `3NZCJ4F004DKPT` | New external-corpus decode |
| RFUAV Mini 3 | O2 | 2 | `687XM7A0020319` in one payload | One v2 payload and one unsupported payload |
| DroneRFa Air 2S | O3 | 2 | `3YTBJ3Q0030J2X` | New external-corpus v2 decode |
| DroneRFa Mavic 3 | O3+ | 2 | Not parsed | Header includes `49 4e 46 50`; no fabricated serial/GPS |
| RFUAV Mavic 3 Pro | O3+ | 2 | Not parsed | Version-3-looking header; v2 parser deliberately refuses it |
| KU Leuven Matrice RC | Enterprise/unverified numeric generation | 1 | `1ZNBHC400C00QN` | Dataset README identifies Matrice 300; not relabeled O3 |
| KU Leuven MavicRC1 | Unverified model/generation | 1 | `4GCCJBFR0B00C1` | Not counted as O1 without model provenance |
| DroneRFa Mini 5 Pro | O4+ (outside strict O4) | 1 | Not parsed | CRC-valid transport bits; no telemetry/decryption claim |

Raw packet exports:
- `data/analysis/corpus_20261026/decoded_packets.jsonl`
- `data/analysis/corpus_deep_20261026/decoded_packets.jsonl`
- `data/analysis/o1_confirmed_20261027/decoded_packets.jsonl`

All entries passed the decoder's outer CRC-24. CRC-valid bytes are **not equivalent to parsed or
decrypted telemetry**. No independent flight-coordinate/motion ground truth is available; parser
units and unsupported layouts remain separate acceptance work. Missing CRCs do not prove encryption,
drone absence, or incompatibility. An O4/O4+ recording returning valid CRC bits is not inherently a bug.

Explicit channel shifts matter: KU Leuven Mini 2 needed −19.5 MHz, Matrice/MavicRC1 +25.5 MHz,
DroneRFa Air 2S/Mavic 3 −11.5 MHz, and RFUAV Mavic 3 Pro +3.5 MHz. Auto-centering selected other
activity in these wideband recordings. Exact flags are saved in each retained excerpt's result entry.

## UI verification and fixes

- Fixed track counts being reset by metric updates; repeated replay accumulates counts consistently.
- Extracted a shared headless worker and separated live RX from DSP with a bounded, drop-counted queue.
- Added overlapping windows and timestamp/overflow gap resets; synthetic boundary bursts decode once.
- Replay streams bounded blocks, respects metadata rate and cancellation, and reports completion as
  disconnected/offline rather than implying active radio reception. Removed simulated packet delay.
- Stop/reconnect no longer synchronously join a running worker on the rendering thread.
- Invalid/missing/nonfinite CLI options fail; live check duration is based on received samples after
  connection. Repeated RX timeouts fail rather than passing with zero samples.
- CRC-valid unsupported payloads are visible with raw hex and blank telemetry; no invented drone track.
- GUI DF fails closed because reopening/retuning invalidates the earlier file-based phase calibration.

Validation evidence:
- **12/12 release CTest suites passed**, UI/UHD build enabled.
- **11/11 AddressSanitizer suites passed** in the headless/non-UHD build during the run
  (`ASAN_OPTIONS=detect_leaks=0`; environment ptrace prevents LeakSanitizer). The later unsupported
  synthetic worker regression was additionally run in the release suite.
- **5/5 corpus-harness tests passed**: formats/scaling, ZIP safe destination, model labels,
  unsupported payload handling and accuracy abstention.
- **8/8 retained real-IQ excerpts** match SHA-256 and produce exactly the same payloads through
  `droneid_decode` and `droneid_ui --check`; each matches its original sweep's expected payload.
  This includes parsed and unsupported formats. Excerpts stay local pending license review.
- Actual native 1280×800 GUI renders inspected for Mini 2 telemetry and Mavic 3 Pro unsupported packet.
  Screenshots: `data/analysis/ui_verification_20261026/ui-verified.png` and `ui-unsupported.png`.
- Release test log and full reference UI logs: `data/analysis/ui_verification_20261026/`.
- External excerpt comparisons: `data/analysis/corpus_deep_20261026/ui_verification.json`.

GPU rendering was checked manually from captured framebuffers, not via automated clicking. Hardware
button interactions, long-running receive throughput and recovery are not claimed as tested.

## O4 accuracy — why acceptance remains open

The actual backend abstains from identity classification for every energy event (`unknown_rf`).
There is no O4 classifier to assign a meaningful accuracy score; identity prediction coverage is zero.

To expose the energy-only limitation, the harness evaluates the deliberately incorrect rule
**“any RF activity means O4”** against explicit family/background labels:

| Sweep | O4-positive windows | Non-O4/background windows | Activity TP / FN | Activity FP / TN | Proxy precision |
|---|---:|---:|---|---|---:|
| Quick | 13 | 37 | 13 / 0 | 37 / 0 | 26.0% |
| Deeper | 3 | 20 | 3 / 0 | 20 / 0 | 13.0% |

Thus **100% activity recall also has 100% false-positive rate** in these selected windows. These are
not O4 classifier accuracy results. The deep cases overlap quick cases and must not be treated as
independent held-out samples. O4+ and ambiguous family labels are excluded from strict O4 metrics;
RUB positive fixtures are excluded from this proxy's negative set. Non-DJI controls are real RF,
not silence. More controls, per-burst labels and independent sessions are needed for genuine accuracy.

## Next acceptance work

1. Expand O1 coverage beyond this one Mavic Pro recording. The 27 October explicit-channel follow-up
   recovered a valid packet: auto-centering had selected other link activity, not the DroneID channel.
   Absolute center metadata and generalized automatic beacon selection remain unverified.
2. Validate v3/other payload layouts before exposing O3+ telemetry; never interpret them as v2.
3. Implement an O4 identity classifier and validate against annotated positives, Wi-Fi/FPV/background
   negatives and held-out sessions. Energy detection alone does not meet this requirement.
4. On restored hardware access, run known-transmitter live decoding and a ten-minute loss/drop/reconnect
   test. Integrate same-session DF calibration and validate the two-antenna rig separately.
