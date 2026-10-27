# Phase 4–6 offline RF smoke check — 26 October 2026

New `rf_analyze` read these four recordings completely, with no input changes:

| Recording | Samples | RF events | Truncated events |
|---|---:|---:|---:|
| No-drone baseline, 2.4 GHz | 46,137,344 | 1,450 | 44 |
| Air 3S, 2425 MHz | 122,880,000 | 4,617 | 60 |
| Air 3S, 5745 MHz | 122,880,000 | 7,026 | 2 |
| Latest centered Mini 2 sweep, 2459.5 MHz | 122,880,000 | 289 | 0 |

Noise was estimated separately for each file using the 20th percentile of
32-sample average powers in the first 1,048,576 samples. This is an exploratory
threshold estimate, not a calibrated noise-only measurement. It is especially
unreliable for continuous transmitters or varying gain. The threshold is 10 dB
above that estimate. Counts depend on gating and segmentation, and must **not**
be interpreted as packets, drones, O4 identifications or directly comparable
presence scores across different frequencies/gain settings.

The baseline also produces substantial activity. The implementation deliberately
reports `unknown_rf` for all events and null for identified drone count/bearing.
SNR and bandwidth measurements do not validate an O4 classifier. No session
fingerprint or O4 payload field has been invented.

`results.json` records exact paths, command lines, thresholds and sample counts.
Files `0.jsonl` through `3.jsonl` contain event metrics in the table order.
The smoke check is reproducible with `scripts/check_rf_metrics.py`.

Release and no-UHD AddressSanitizer builds pass all nine test suites (leak
detection disabled for the sandbox's ptrace limitation). The UHD-enabled build
also compiles the timed-hop and paired-capture tools. No radio was opened and no
physical bearing, catch-rate, Wi-Fi separation or coherent-capture acceptance
claim is made. See `docs/PHASE456.md` for the operational limitations.
