# Open Drone ID (ASTM F3411 Remote ID) — Wi-Fi Beacon and NAN

An add-on receiver for standard Remote ID broadcasts, alongside the DJI DroneID decoders. It is
receive-only and opt-in: no existing decoder, CLI or UI default changes. The operator's test
transmitter uses **Wi-Fi Beacon and Wi-Fi NAN**; Bluetooth (BT4 legacy, BT5 Long Range) is not
implemented.

## Gates (28 October 2026)

| Gate | State | Evidence |
|---|---|---|
| Software: F3411 parser, 802.11 MAC framing, OFDM + DSSS PHY, stream windowing, CLI, opt-in UI | **Done, synthetic IQ only** | `test_odid` (16 cases), `test_odid_wifi` (14), `test_odid_cli`; 16/16 CTest |
| Interoperability with independent 802.11 transmitters | **Done on air** (OFDM 6/12/24/48, DSSS 1/2 Mb/s long preamble) | `data/analysis/odid_20261028/baseline_ch6_phy_tally.log`; the gr-ieee802-11 cross-check could not run (local install crashes) |
| Hardware: X310 at 200 MHz master clock / 20 MSPS on 1GbE | **Passed**, 60 s | `data/analysis/odid_20261028/transport_20msps_60s.log` |
| Hardware: decode the operator's transmitter from recorded IQ | **Passed**: ESP32-S3, beacon + NAN, DSSS 1 Mb/s, ch 6 | `data/analysis/odid_20261028/README.md` |
| Acceptance: live GUI shows the transmitter with correct identity/position | **Passed for a 30 s bench run** | same; ≥ 5 min soak, reconnect, other channels and range remain open |

Decode fraction on busy channel 6 was about 70%, limited by collisions with other Wi-Fi. Range,
sensitivity, 5 GHz and OFDM-rate or short-preamble ODID transmitters remain unmeasured.

## What is checked against the standards

- F3411 message layouts and scaling: hand-built byte vectors in `tests/test_odid.cpp`, written
  independently of the encoder. Validity rules follow opendroneid-core-c (reference only).
- 802.11 DSSS PLCP CRC-16: matches the worked example in the IEEE 802.11 DSSS clause
  (header `0101 0000 ... 0011 0000 0000` → CRC `0101 1011 0101 0111`).
- 802.11 OFDM scrambler: matches the standard's 127-bit sequence. Convolutional encoder:
  matches generators 133/171 (octal).
- Everything else in the OFDM chain (training symbols, interleaver, pilots, constellation
  mapping, SIGNAL layout) is checked only by self-consistency between the fixture
  synthesizer and the receiver. On air (28 October), frames from independent Wi-Fi devices
  decoded with valid FCS at OFDM 6/12/24/48 Mb/s (BPSK to 64-QAM) and DSSS 2 Mb/s (DQPSK). That
  confirms these constants and the DQPSK rotation sense. Still not exercised on air: OFDM 9/18/36/54,
  DSSS short preamble.

## Architecture

```
IQ (>= 20 MSPS, centred on a Wi-Fi channel)
  ├─ resample to 20 MSPS ─ OFDM: STS autocorrelation → LTS timing/CFO → SIGNAL → equalize,
  │                         pilot phase/slope tracking → soft Viterbi → descramble   (6–54 Mb/s)
  └─ resample to 22 MSPS ─ DSSS: Barker sync → DBPSK/DQPSK → descramble → SFD → PLCP CRC-16
                            (1/2 Mb/s, long + short preamble; CCK 5.5/11 counted, not decoded)
→ 802.11 FCS (CRC-32) → beacon vendor IE FA:0B:BC/0x0D or NAN SDF (service id 88:69:19:9D:92:09)
→ message counter + F3411 Message Pack → UasData
```

| File | Role |
|---|---|
| `include/droneid/odid.h`, `src/odid/messages.cpp` | F3411 messages 0-5 + Message Pack, validity rules, fixture encoders |
| `include/droneid/odid_wifi.h`, `src/odid/wifi_mac.cpp` | MPDU parse (beacon, NAN action), FCS, fixture MPDU builders |
| `src/odid/wifi_ofdm.cpp`, `wifi_dsss.cpp`, `wifi_dsp.cpp` | PHY receivers, fixture synthesizers, resampler, FFT |
| `src/odid/wifi_decoder.cpp` | Block decode (OFDM and DSSS run concurrently), `OdidWifiStream` windowing |
| `src/ui/odid_worker.cpp` | Opt-in replay/live workers publishing `ODID/ASTM` packets and tracks |
| `tools/odid_decode.cpp`, `tools/odid_gen.cpp` | Offline decoder CLI; synthetic fixture generator |

The library has no FFTW dependency and always builds. `OdidWifiStream` decodes 2^21-sample
windows with 16 ms of overlap. A frame is reported only by the window that owns its estimated
preamble start, and repeats within 64 samples are suppressed. A frame longer than the overlap
(above about 2000 bytes at 1 Mb/s) cannot be decoded across a window edge. Throughput on this
host is about 0.7 s of wall time per second of 20 MSPS input (two threads).

Validity: frames must pass the 802.11 FCS. Packs with a wrong single-message size, a count
outside 1-9, truncation, nested packs or more than the allowed number of any message type are
rejected whole. Inside a valid pack, a message with an out-of-range latitude, longitude,
direction or timestamp, or an unsupported protocol version (>2), is rejected and counted, and the
rest are kept. Authentication pages are parsed but never cryptographically verified.

## Usage

```bash
# Offline: decode a recording (centre on the Wi-Fi channel, >= 20 MSPS; .meta sidecar honoured)
./build/odid_decode capture.iq --format sc16 --jsonl capture.jsonl
# Synthetic fixture (never transmitted): 3 beacons, OFDM 6 Mb/s, 20 MSPS cf32
./build/odid_gen /tmp/odid.cf32 --phy ofdm --mbps 6 --transport beacon --count 3
# GUI replay / headless check, and live (X310, 200 MHz master clock, 20 MSPS, channel 6 default)
./build/droneid_ui --odid --replay capture.iq --format sc16
./build/droneid_ui --odid --device addr=192.168.10.2 --freq 2437e6 --gain 20
```

`odid_decode` prints one line per frame and a summary of PHY/MAC counters (`ofdm_sync`,
`ofdm_signal_ok/bad`, `dsss_sync`, `dsss_header_ok/bad`, `fcs_ok/fail`, `non_odid`,
`odid_rejected`). These tell you whether a silent capture is on the wrong channel, too weak, or
clipping. `--freq-offset HZ` recentres a channel that is off-centre in a wider capture.
`--min-frames N` makes the exit status 3 when fewer frames are decoded.

## Known limits

- One 20 MHz channel at a time; no channel hopping. NAN uses channel 6 (2437 MHz) or 149
  (5745 MHz). Beacons may be on any channel the transmitter is configured for.
- Not decoded: HT/VHT/HE PPDUs, CCK 5.5/11 Mb/s, BLE transports.
- In `--odid` mode the GUI runs only the ODID decoder. DJI decoding and the unknown-RF energy
  summary are not active there.
- Identity comes from the broadcast and is not authenticated. Tracks are keyed by UAS ID, or by
  transmitter MAC until a Basic ID is seen. A drone that sends both DJI DroneID and Remote ID
  appears as two tracks, one per source.
