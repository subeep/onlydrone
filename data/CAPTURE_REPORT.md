# Phase 1 capture report

Fill this in from `burst_scan` output on real drone captures. The goal is to confirm or
correct the §2 constants in `DRONEID_BUILD_PLAN.md` with evidence.

## Rig (verified 2026-10-26)
- X310 serial **326CF02** @ `addr=192.168.10.2`, 2× UBX-160 v2.
- Host link 10GbE; **MTU 1500** (raise to 9000 before wideband). At 15.36 MSPS sc16,
  narrowband capture runs with **0 overflows** at MTU 1500.
- Master clock **184.32 MHz passed in device args** → exact **15.36 MSPS** (÷12). Setting
  it after `make()` is ignored by the X310 (leaves 200 MHz → 15.3846 MSPS, odd decim).

## Baseline (no drone) — 2.4595 GHz, 0.5 s
- 440 bursts, median 94.7 µs, median period 0.389 ms, occupied BW ~14.6 MHz.
- Interpretation: ambient Wi-Fi/BT. This is the clutter the DroneID detector must reject.

## Mini 2 (OcuSync 2) — captured 2026-10-26
Sweep: 5× 8 s slices on 2.4 GHz (2410–2470). 0 overflows, 15.36 MSPS sc16. Files in `data/mini2/`,
spectrograms in `data/mini2/analysis/`.

| center (MHz) | bursts/8s (+12dB,100µs) | median period (ms) | notes |
|---|---|---|---|
| 2410 | 1280 | 3.65 | |
| 2425 | 3074 | 1.92 | |
| 2440 | 2577 | 1.54 | strong recurring structured bursts (see spectrograms) |
| 2455 | 2716 | 2.29 | |
| 2470 | 2037 | 3.07 | |

**Finding:** unlike the ambient baseline, the Mini 2 shows **strong, structured, recurring full-band
bursts** on 2.4 GHz (its OcuSync link) plus fuzzy recurring sub-band blocks — drone presence is obvious
in the spectrograms. But the specific DroneID beacon still can't be isolated from the OcuSync video/control
bursts by energy/visual means.

- DroneID channel(s) actually used: TBD — requires ZC matched filter to localize.
- Matches §2 (9–10 MHz BW, 8–9 symbols)? Cannot confirm by eye; needs the detector.

## Phase 2 reference validation (2026-10-26)
- Synthetic frames pass detection, turbo decoding and CRC-24. The saved, centered
  RUB-SysSec Mini 2 reference (`mini2_ref2.iq`, 223,488 samples at 15.36 MSPS) now
  gives **10 detected bursts, 10 CRC-24-valid payloads**, with or without LPF.
- The prior real-data failure was **integer-carrier CFO ambiguity**, not demonstrated
  fractional timing error. CP correlation measures CFO modulo 15 kHz: the actual
  residual -57.47 kHz appeared as +2.53 kHz. Root 600 still correlated at 0.78–0.96,
  while the wrong carrier alignment corrupted equalization and all payloads.
- Joint timing/frequency matching against both ZC roots at their known separation
  resolves this ambiguity. Search covers ±16 carriers around the CP estimate.
- A 64 kB excerpt is retained in `tests/data/mini2_centered.sc16`; its exact payload,
  serial, sequence, timestamp and phone coordinates are regression-tested. See
  `tests/data/README.md` for provenance and hashes. A second, unmodified 50-MSPS
  excerpt now tests integrated preprocessing against the identical payload.
- Corrected v2 parser offsets and phone latitude/longitude order. The reference
  has phone coordinates but zero drone/home coordinates. Nonzero altitude, speed
  and yaw units remain provisional; inner CRC-16 and legacy frame variants are pending.
- Previous local sweeps reported ZC maxima around 0.05–0.11, with no detected
  beacon, including a centered 30-second dwell and an outdoor sweep. These results
  do not establish why the local beacon was absent. GPS gating was an earlier
  hypothesis, contradicted as a universal requirement by this public reference.

## Overall next step
Obtain a CRC-valid local Mini 2 capture. Integrated wideband centering/resampling
and the decoder now have a real-data regression baseline.
The remaining local-capture acceptance work is distinct from the resolved reference
CRC failure. Do not treat energy-only detections as proof of a DroneID beacon.

## Integrated wideband follow-up (2026-10-26)
- Restored the pinned, unmodified `mini2_sm` reference (SHA-256 and reproduction
  command in `data/reference/README.md`). Direct cf32 input at 50 MSPS with
  `--auto-center --secs 0` selects a -9,671,021 Hz shift and produces **10/10
  CRC-valid candidates**, both with and without LPF. Explicit -9.6 MHz shift also
  works. There is no preprocessing scratch binary in this path.
- The resampler applies the frequency shift before anti-alias filtering and
  conversion to 15.36 MSPS. Fractional phase and filter history persist across
  reads. Detection indices map back to original input samples; `--secs` honors
  the rate from sidecar metadata. `--secs 0` means the entire file.
- Automatic centering selects the strongest 10 MHz band in a bounded prefix.
  It is suitable for this isolated reference, not a signal classifier or a
  hopping/multi-channel receiver. Explicit offsets remain preferable in clutter.
- Re-ran all five files under `data/mini2/sweep_20261026T092618Z/`, **8 seconds
  per channel**, using `--secs 0 --thresh 0.3`. Every channel (2399.5, 2414.5,
  2429.5, 2444.5 and 2459.5 MHz) returned **0 candidates / 0 CRC-valid**.
  These were offline replays; no new RF capture or live-radio validation was performed.

## Air 3S (O4) — captured 2026-10-26 (video link pinned manual to 5799–5809 MHz)
Sweep: 5× 8 s slices on 2.4 GHz (2410–2470), 4× 8 s on 5.8 GHz (5745–5835). 0 overflows, 15.36 MSPS sc16.
Files in `data/air3s/`; spectrograms in `data/air3s/analysis/`.

| band | center (MHz) | bursts/8s | median burst (µs) | period (ms) | occ BW (MHz) | notes |
|---|---|---|---|---|---|---|
| 2.4 | 2410 | 6778 | 53 | 0.11 | 15.3 | congested |
| 2.4 | 2425 | 9550 | 87 | 0.32 | 15.2 | congested |
| 2.4 | 2440 | 17950 | 44 | 0.14 | 15.1 | most congested |
| 2.4 | 2455 | 6029 | 97 | 0.32 | 14.9 | congested |
| 2.4 | 2470 | 6299 | 48 | 0.17 | 15.1 | congested |
| 5.8 | 5805 | 8207 | 69 | 0.48 | 12.7 | **video link** (pinned 5799–5809) — clearly visible |

**Finding:** energy/envelope detection cannot isolate DroneID here. 2.4 GHz is saturated with Wi-Fi/BT
(irregular, mostly full-15 MHz bursts — see `air3s_24_2440_1s_zoom.png`). The 5.8 video link is obvious
(`air3s_58_5805_videolink.png`), confirming the capture chain works. No clean ~10 MHz beacon repeating at
~1–2/s is visible by eye.

**Conclusion:** need the Zadoff-Chu matched-filter detector (Phase 2) to confirm whether O4 DroneID bursts
are present — a power gate is the wrong tool in this clutter. Raw captures are archived to test the
detector offline (no re-flying needed). The Mini 2 (O2, documented format) is the right ground-truth to
validate that detector first.

- Burst shape vs. Mini 2: TBD (capture Mini 2)
- Screenshot's 0xAA/0x87 / session-hashcode claim: still unverifiable from RF alone; defer until decoder exists.

## Two-channel coherent (DF baseline) — TODO
- Splitter one source into RX0 (Radio#0) and RX1 (Radio#1), same center.
- Measured inter-channel phase (stable? value): 

## Complete local-data recheck (2026-10-26)

Replayed all 21 local recordings: 11 Mini 2, 9 Air 3S, and one baseline.
51 full-file decode passes plus 21 prefix frequency scans completed successfully.
Mini 2 and baseline yielded no candidates at threshold 0.3. Air 3S yielded 183
candidate hits summed across passes, but no CRC-valid payload; this count includes
duplicate observations across passes. Two strong Air 3S examples poorly matched
the required second ZC sequence. The reference control passed 10/10 during this run.

See [the complete report](analysis/local_recheck_20261026/REPORT.md), including
coverage limitations, per-file JSON results, and full decoder logs. The newest
local IQ file found was modified at 14:57:16 IST on 26 October 2026.

## Decisions / plan updates
- [ ] §2 constants confirmed, or updated to: 
- [ ] DroneID center frequencies to dwell on (Phase 4):
