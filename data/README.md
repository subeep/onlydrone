# data/ — captures and analysis

Raw IQ captures (gitignored: `*.iq`, `*.meta`) and the capture report live here.
One subdirectory per drone label, e.g. `data/mini2/`, `data/air3s/`.

## How to capture (needs the drone flying)

DroneID may only transmit with motors armed / controller linked, so **fly the drone**
while capturing. Receive-only; nothing is transmitted.

```bash
# Mini 2 (OcuSync 2) across the 2.4 GHz band
scripts/capture.sh 24 mini2 20 30

# Air 3S (O4) across 2.4 and 5.8 GHz
scripts/capture.sh 24 air3s 20 30
scripts/capture.sh 58 air3s 20 40
```

## How to analyze

```bash
build/burst_scan data/mini2/<file>.iq --thresh-db 10 --min-us 20
build/burst_scan data/mini2/<file>.iq --json > data/mini2/<file>.scan.json
```

Then fill in `CAPTURE_REPORT.md` and reconcile against §2 of the build plan. Update the
plan's constants if the measurements differ — that is the whole point of Phase 1.

## What to look for (DroneID signature vs. ambient Wi-Fi)

- DroneID bursts: roughly **9–10 MHz** occupied bandwidth, **8–9 OFDM symbols**, a
  repeating burst cadence, two Zadoff-Chu sync symbols. (All to be confirmed here.)
- Ambient 2.4 GHz Wi-Fi/BT looks like many wider (~14–20 MHz) or very short bursts with
  irregular timing — the no-drone baseline. Capture that baseline first for comparison.
