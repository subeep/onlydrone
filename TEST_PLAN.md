# Offline corpus validation and remaining acceptance

## Open Drone ID Wi-Fi (28 October)

Three new hermetic suites (evidence tier 1, **synthetic IQ only**). Release CTest passes 16/16:
- `test_odid`: F3411 messages from hand-assembled bytes (scaling, unknown markers, range
  rejection), auth pages, binary IDs, Message Pack structure/content rules, partial rejection,
  encoder round trip, CRC-32, and beacon/NAN MPDU layouts and rejection reasons.
- `test_odid_wifi`: PLCP CRC-16 standard example, OFDM scrambler sequence, convolutional
  encoder generators, every OFDM rate × beacon/NAN, DSSS 1/2 Mb/s × long/short preamble,
  23.04/25/50 MSPS input with CFO and clock offset, `freq_offset`, scrambler-seed transparency,
  FCS failures, non-ODID/malformed frames, back-to-back frames, stream chunking across window
  edges (each frame exactly once at the right sample), truncation, and noise/tone/silence.
- `test_odid_cli`: `odid_gen` → `odid_decode` output, JSONL, exit codes and invalid arguments.
  When the UI is built, `droneid_ui --check --odid` shows `ODID/ASTM` packets and tracks, and the
  same file in default (DJI) mode yields none.

Hardware tiers (28 October, `data/analysis/odid_20261028/`): the 20 MSPS transport, a real
ESP32-S3 transmitter (CLI, GUI replay, 30 s live GUI) and on-air third-party Wi-Fi decoding all
pass. Still open: ≥ 5 min soak, reconnect, other channels/5 GHz, range and sensitivity.

## X310 transport and capture checks (28 October)

See `docs/X310_BENCH_20261028.md` and `data/analysis/x310_bench_20261027/`.
The 600-second two-channel UHD transport benchmark passes at 11.52 MSPS/sc16.
Bounded application captures check measured hardware metadata and paired continuity.
The single-channel GUI live backend receives three seconds without gaps or drops;
uncalibrated paired replay accepts no bearings. Release CTest passes 13/13, including
11.52 MSPS resampling and synthetic CRC decode. A known split reference, physical
retune/phase tests, paired live service/soak and measured field angles are still required.

## DF integration review (27 October)

Release CTest now includes `test_df_cli` when Python 3 and the UI executable are
available: historical-time CLI/UI angle agreement, legacy-calibration rejection,
zero-gain mismatch and expiry advancing with sample position. Core/worker/model
regressions also cover manifest inconsistencies, missing provenance, symmetric
out-of-passband energy, original-rate paired slices and escaped JSONL strings.
Run `python -m unittest discover -s tests -p test_df_cli.py` standalone if needed;
`DRONEID_BUILD_DIR` selects another build directory. Current review and remaining
hardware/software acceptance gaps: `docs/DF_REVIEW_20261027.md`.

## Implemented

`scripts/validate_corpus.py` reads existing local data only. No IQ downloads, source modifications or
bulk extraction. NumPy, h5py and `unrar` are required; decoder/RF binaries must already be built.

```bash
python scripts/validate_corpus.py --output data/analysis/corpus_quick --seconds 0.1 --per-group 2 --jobs 2
python scripts/validate_corpus.py --output data/analysis/corpus_deep --seconds 1.4 --per-group 1 --jobs 2 --datasets DroneRFa RFUAV KULeuven Tampere
```

Default external root: `/media/sudeep/Storage/datasets`. Repo references and recent local captures
are also inventoried. `--root` selects another mount. `--seconds` is capped at two seconds per selected
case; this is **not whole-corpus validation**. Reports retain paths, archive members, actual sampled
duration, rate/center, clip SHA-256, per-channel logs and errors.

Adapters verified against local acquisition code/metadata:
- DroneRFa/DRFF-R2: HDF5 `RF0_I`, `RF0_Q`, `Fs`, `CenterFrequence`; selected single-drone groups only.
- KU Leuven: first MAT member per selected ZIP, compound real/imag `uhd_samps`, 100 MSPS, 2.44 GHz.
  Only that member is temporarily extracted, with a 512 MiB cap.
- RFUAV: bounded IQ stream from the first selected member; Complex Float XML rate/center.
- Tampere: little-endian interleaved int16, 120 MSPS at 2.4 GHz / 200 MSPS at 5.8 GHz.
  Absolute centers are not established; auto-centering and explicit documented offset hypotheses are used.
- Recent local recordings: int16 IQ with sidecar metadata. RUB references: original 50 MSPS cf32.

Decode attempts use auto-centering plus known DroneID channels fully within the capture passband.
The strongest RF channel is not necessarily a DroneID channel. Unique payload counts deduplicate
identical hex across passes; they are not an estimate of transmitted packet count or decode recall.
Small local-only excerpts of newly decoded packets are retained for UI/backend replay verification.
Dataset redistribution licenses are not verified; these excerpts are not promoted to CI fixtures.

## Evidence tiers

1. Hermetic CTest: DSP/FEC, resampler, file/CLI handling, model and worker regressions.
2. Existing real golden fixtures: exact payload/serial validation without the external drive.
3. Bounded external sweep: outer CRC-24, supported-v2 parsing and exact UI/CLI payload comparison.
4. Native GUI framebuffer inspection with real offline input.
5. Live X310/drone and direction-finding acceptance: **deferred at operator request**.

## Accuracy rules

- Report CRC-valid transport payloads separately from parsed telemetry. Unsupported payloads remain
  visible but create no fabricated track/GPS. No independent flight telemetry ground truth is supplied.
- O4 backend emits `unknown_rf`, not O4 predictions: O4 classifier accuracy is **unestablished**.
- Measure the deliberately naive `any RF => O4` proxy against labeled non-O4 and background windows
  to expose false positives, not claim classifier accuracy. O4+ is excluded from strict O4 metrics.
- Model-family corrections and sources: `docs/MODEL_FAMILIES.md` (Mini 3 is O2; Mini 5 Pro is O4+).
- CRC failure does not prove encryption or drone absence. CRC success on an O4-labeled recording
  must be investigated, not automatically treated as a defect. No generation is inferred from CRC.

## Still required

- Expand beyond selected prefixes/first archive members; alternate bands and repeated flights.
- Expand O1 beyond the successfully decoded Tampere Mavic Pro file; validate unsupported v3/other layouts.
- Dedicated Wi-Fi-only/analog-FPV controls, annotated bursts and independent held-out sessions.
- Implement/validate an O4 identity classifier before claiming detection accuracy or drone count.
- Mixed-drone, SNR, interference and longer-duration stress testing; exclude ambiguous labels.
- Ten-minute live UI test with a transmitter: CRCs, receive gaps, drops, reconnect and shutdown.
- Same-session phase calibration and calibrated two-antenna tests; no unverified cross-session file cal.
  Complete DF-1–DF-6 in `docs/DIRECTION_FINDING_PLAN.md`: paired contract/replay, calibration lifecycle,
  shared DSP/association, GUI, hardware bench, then measured field accuracy/hopping.

Current results: `docs/VALIDATION_20261026.md`. CardRF remains excluded; do not download its large archive.

## O1 follow-up (27 October)

`scripts/investigate_o1.py --decode` performs bounded full-band spectral search on the local Tampere
Mavic Pro recording. It found a CRC-valid v2 packet missed by strongest-channel auto-centering.
The same full-file packet reaches the UI, serial `08RDE310010218`. See
`data/analysis/o1_followup_20261027/REPORT.md` for the exact payload/commands and search limitations.
The corpus harness now accepts `--model-filter` and repeatable `--extra-offset` arguments for
explicitly testing documented channel hypotheses; the decoder itself was not changed for this fix.

## Envelope follow-up (27 October)

```bash
python scripts/audit_packet_envelopes.py data/analysis/corpus_20261026 data/analysis/corpus_deep_20261026 data/analysis/o1_confirmed_20261027 --output data/analysis/envelopes_20261027
python scripts/verify_ui_corpus.py data/analysis/corpus_deep_20261026
python scripts/verify_ui_corpus.py data/analysis/o1_confirmed_20261027
```

The audit uses saved hex only and deduplicates complete transport payloads. All 22 unique payloads
pass inner CRC16. CLI/UI comparisons now include format and inner-checksum status (9/9 retained clips).
CTest covers the real Mini 2 golden checksum, supported v2 lengths, unknown/unsupported formats,
corrupted inner data and invalid logical lengths. Python envelope tests match those guards.
INFP occurs in O3+ and O4+ labels: never use it alone as an O4 classifier. No payload decryption or
v3 telemetry mapping is claimed. Decoder output remains raw transport evidence when parsing fails.
