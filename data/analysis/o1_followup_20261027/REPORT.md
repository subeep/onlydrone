# O1 Mavic Pro follow-up — 27 October 2026

## Result

**The existing decoder successfully decodes this O1-labeled Mavic Pro recording.**
One outer-CRC-24-valid v2 packet is recovered from the entire 1.0-second file:

- Dataset: Tampere, `DJI_mavic_pro_2G.bin`, little-endian interleaved int16 IQ, 120 MSPS.
- Serial: `08RDE310010218`; sequence: 495.
- Payload length: 176 bytes; exact bytes in `decoded_packets.jsonl` and decoder/UI logs.
- Drone, pilot and home coordinates are zero: this is **not verified GPS position telemetry**.
- Input raw SHA-256 and spectral regions: `spectral_regions.json`.

## Why the earlier test failed

The original `--auto-center` preview selected approximately **+17.49 MHz**, within the strong
wideband link activity. The decodable burst occurs around **0.510 seconds**, near **+4.5 MHz**
relative to the capture center. The actual absolute capture center remains unknown.

The new read-only full-band diagnostic found 414 energy-connected RF regions and one search hint
matching the selected duration/bandwidth bounds (597 µs, 10.55 MHz). Its estimated center was
+4.5703125 MHz. Testing that shift and nearby hypotheses recovered the same CRC-valid payload.
Three of five nearby frequency hypotheses succeeded; these are duplicate decodes of one packet.

The cause in **this recording** is channel selection, not a demonstrated O1 FEC/layout incompatibility.
The existing v2 parser works without a decoder patch. Auto-centering still chooses strongest activity;
it has not been changed into a universal DroneID channel search.

## Independent checks

1. Entire original file, explicit shift −4.5703125 MHz: **1 candidate, 1 CRC-valid packet**.
2. Entire original file through `droneid_ui --check`: **120,000,000 samples, 1 packet, 1 track, count 1**.
3. Full-file CLI and UI payload hex compared byte-for-byte: **identical**.
4. Small retained excerpt: SHA-256 checked; CLI/UI exact payload comparison passes.
5. Native GUI rendered the excerpt and displayed the serial, sequence and count: `ui-o1.png`.
6. Standard corpus harness rerun: auto-centering **0 CRC**, explicit shift **1 CRC**.
   Evidence: `../o1_confirmed_20261027/results.json` and `ui_verification.json`.
7. Regression checks: release CTest **12/12**, corpus harness **5/5**, new spectral diagnostics **2/2**.

Energy counts differ between full-file `rf_analyze` and UI windows because their adaptive noise
estimates and segmentation windows differ. Those counts are neither unique packets nor drone counts.

## Reproduce

From the repository root, using Python with NumPy/SciPy/h5py/Matplotlib:

```bash
python scripts/investigate_o1.py --decode
./build/droneid_decode /media/sudeep/Storage/datasets/Tampere_DroneRF/DJI_mavic_pro_2G.bin --format sc16 --rate 120000000 --freq-offset -4570312.5 --thresh 0.3 --secs 0
./build/droneid_ui --check --replay /media/sudeep/Storage/datasets/Tampere_DroneRF/DJI_mavic_pro_2G.bin --format sc16 --rate 120000000 --freq-offset -4570312.5
python scripts/validate_corpus.py --output data/analysis/o1_confirmed_20261027 --seconds 1 --datasets Tampere --model-filter mavic_pro --extra-offset -4570312.5
python scripts/verify_ui_corpus.py data/analysis/o1_confirmed_20261027
```

No source dataset was modified; no IQ was downloaded. Retained excerpts are for local evaluation
only, pending license review. The spectral search is a bounded heuristic: weak/overlapping bursts
can be missed, search hints are capped at 100, and this one capture does not establish fleet-wide recall.
O3+ telemetry parsing, O4 identity accuracy and physical live/DF acceptance remain open.
