# Open Drone ID Wi-Fi — device verification, 28 October 2026

First hardware run of the ASTM F3411 Wi-Fi decoder (`docs/OPEN_DRONE_ID.md`), following
`docs/ODID_VERIFICATION_RUNBOOK.md`. Receive-only: the X310 transmitted nothing (`benchmark_rate`
reports zero transmitted samples, and the application has no TX path). Times are UTC
(host logs show IST, UTC+5:30).

## Setup

| Item | Value |
|---|---|
| Receiver | X310 `326CF02` (HG image, 1 GbE), UBX-160 ch0 RX2, UHD 4.1.0.5-3 |
| Profile | `config/x310_odid_wifi.toml`: 200 MHz master clock, 20 MSPS, sc16, 2437 MHz (ch 6), gain 20 dB |
| Transmitter | Operator's ESP32-S3 Remote ID bench transmitter, Wi-Fi Beacon + NAN, 2–5 m from antenna |
| Reference values | UAS ID `ESP32S3RIDD682C4`, MAC `70:af:09:d6:82:c4`, operator ID `TEST-OPERATOR-0001`, self-ID "newrock ODID bench test", operator 51.4779, -0.0015. **Operator confirmed** these match the device configuration. The drone position is the device's configured (simulated) track, not a live GNSS fix |
| Code | `fa10c5d` plus uncommitted ODID working tree (`provenance.log`) |

## Results

| Check | Result | Evidence |
|---|---|---|
| Transport, 200 MHz MCR / 20 MSPS sc16, 60 s | 1,200,969,626 samples; zero drops, overruns, sequence errors, timeouts; zero TX samples | `transport_20msps_60s.log` |
| Baseline, transmitter off, 2 s (07:39:47) | 0 ODID frames; 146 FCS-valid frames from ordinary Wi-Fi; 0 overflows | `baseline_ch6_*.log` |
| Real-signal PHY coverage (baseline) | FCS-valid frames from independent devices at OFDM 6, 12, 24, 48 Mb/s and DSSS 1, 2 Mb/s (long preamble) | `baseline_ch6_phy_tally.log` |
| Transmitter capture, 10 s (07:42:20) | **141 ODID frames**, one UAS, 0 rejected messages, 0 overflows: 69 beacon + 72 NAN, all DSSS 1 Mb/s long preamble | `odid_ch6_decode.log`, `odid_ch6_stats.log` |
| Identity / content | UAS ID, MAC, operator ID, self-ID, operator position match the device configuration (operator-confirmed) | `odid_ch6_decode.log` |
| GUI replay (`--odid --check`) | 141 packets, 1 track `ESP32S3RIDD682C4` `source=ODID/ASTM`; equals the CLI | `ui_replay_check.log` |
| GUI replay, native window | Drone row (`ODID/ASTM Helicopter…`, `gen ODID/ASTM wifi_nan`, count 141) and packet rows with position/operator | `ui_replay.png` |
| Live GUI, 30 s headless (07:49:5x) | **404 ODID packets**, 1 track, 600,000,000 samples, 0 overflows / gaps / dropped windows | `ui_live_check2.log` |
| Live GUI, native window, 25 s | Connected, ref locked, 0 overflows / gaps / drops, track count rising (156 at capture) | `ui_live.png`, `ui_live_window.log` |

## Observations (not acceptance criteria)

- **Decode fraction about 70%.** Each transport's message counter advanced 99 times in 9.9 s (about
  10 frames/s each), and 69 beacon + 72 NAN frames were decoded. Live gave 404 in 30 s
  (about 67%). 27 of the transmitter's frames demodulated with an FCS failure. Byte diffs show
  localized error bursts in otherwise-correct frames.
- **Cause of the failures: collisions, not timing.** Good frames show a steady −2.3 ppm
  symbol-clock offset, matching the +5.1 kHz (+2.1 ppm) carrier offset, so the clocks are locked.
  In failed frames, the Barker structure degrades partway through, and the spectrum there becomes
  flat across the 20 MHz band: another station's OFDM frame is overlapping. Channel 6 was busy
  (about 1,800 other 802.11 frames per 10 s). The remaining ~30 missing frames are consistent
  with collisions during the preamble.
- Median SNR estimate 14 dB; median CFO +5.1 kHz. No OFDM or short-preamble frames from this device.
- The transmitter stopped broadcasting between about 07:43 and 07:49 UTC. `ui_live_check.log` (0 ODID
  frames, otherwise clean) and `recheck_ch6_*` record that interval. After the operator restarted
  it, `recheck2_ch6_*` shows 50 frames in 3 s. Those first live logs are not a decoder failure.

## Not verified here

- Long-duration live operation (≥ 5 minutes), reconnect, cable removal.
- Channels other than 6; 5 GHz (NAN ch 149); OFDM-rate or short-preamble ODID transmitters.
- Range/sensitivity; decode fraction on a quiet channel; a second independent ODID receiver.
- An independent 802.11 implementation cross-check (real third-party Wi-Fi decoding above is
  strong evidence for the PHY constants, but CCK, HT and DSSS short preamble are still unexercised
  on air).

## Files

IQ recordings and the JSONL are kept locally (git-ignored); checksums are in `sha256sums.txt`.
Nearby networks' SSIDs were deliberately not recorded. The scratch analysis programs (PHY tally,
per-frame drift/spectrum diagnostics) were one-off diagnostics and are not part of the repository.
