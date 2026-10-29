# Final software and connected-radio verification — 29 October 2026

## Delivered behavior

The right polar panel has been replaced by a native OpenStreetMap view. Clicking
any packet row retains that exact packet's information and payload. Valid drone
coordinates center a red drone marker; blue pilot/operator and green home markers
are separate. No selection or no valid drone GPS defaults to Punjab, India
(31.1471 N, 75.3412 E). Unknown/unparsed/O4-only evidence never creates a position.
The details panel is scrollable and includes checksum, source, serial/type,
sequence, receipt time, coordinates, altitude/height and raw hex when available.

Map tiles load in a separate worker with bounded requests/PNG decoding/GPU cache,
visible OSM attribution, seven-day disk caching and an online toggle. Map imagery
is not bundled. The map plots transmitted coordinates; it does not authenticate
or independently measure the transmitter's physical location. The ESP32 test
messages contain configured coordinates near Greenwich, not the Punjab default.

## Software evidence

| Check | Result | Evidence |
|---|---|---|
| Linux final CTest | 18/18 suites pass | `linux-ctest.log` |
| Python validation utilities | 14/14 tests pass | `python-tests.log` |
| Windows/Wine executable checks | 17/17 pass | `windows/checks.json` |
| Shared UI worker | 15 cases pass on Windows (also covered by Linux CTest) | `windows/test_ui_worker.log` |
| Packet/model/map state | 12 cases pass on Windows | `windows/test_ui_model.log` |
| DJI decoder + concurrent FFT | 14 cases pass on Windows | `windows/test_dji.log` |
| Real-IQ CLI/UI packet parity | 9/9 retained clips, payload/checksum agreement | `corpus/ui_verification.json` |
| Recorded real ESP32 IQ | 11 frames in 1 second, one expected track | `odid-replay.log` |
| Native packet click: valid drone GPS | Passed; selected details + map | `native-valid-location.png` |
| Windows/Wine packet click: valid drone GPS | Passed on final build, including cached map with online downloads disabled | `windows-map-final-check.json`, `windows-map-final.png` |
| Native click: pilot-only Mini 2 | Passed; details/pilot coordinates, Punjab default, no false drone marker | `map-native-retry-checks.json`, `native-pilot-only.png` |
| Native click: unsupported CRC-valid Mavic 3 payload | Passed; raw payload/checksums, no invented telemetry | `map-native-retry-checks.json`, `native-unsupported.png` |
| O4 regression | All 35 cases successful; every per-case count matches the frozen prior run | `o4-regression/results.json` |

The packet corpus covers retained O1/O2/O3/O3+ transport examples. Unsupported
formats remain explicitly unparsed despite valid transport/inner checksums.
This rerun verifies those retained captures, not every drone model or every file.

O4 still detects 7/8 O4-labeled prefixes, including all six Mini 4 Pro prefixes
and one of two Avata 2 prefixes. The 24 clear non-O4 controls produce zero
candidate windows out of 960; the three O4+ prefixes produce no candidates.
These are recording-prefix regression results, not population-wide accuracy or
independent per-drone identification. No thresholds were tuned during this review.
No IQ datasets were downloaded.

Initial GUI automation attempts targeted a different open receiver window or
used an incorrect desktop-scaled pointer position. The native retries target the
exact process/window and send ordinary X11 client-coordinate mouse events into
GLFW/ImGui. An unsupported fixture also required its recorded -11.5 MHz frequency
shift. Final successful selection evidence is identified above; the earlier
failed attempts are not counted as passes. Windows is tested under Wine, not a
native Windows hardware installation.

## Real X310 observations

X310 serial `326CF02`, address `192.168.10.2`, RX2/channel 0, gain 20 dB,
receive-only, sc16 wire samples over the existing network. Logs retain the UHD
configuration and its UBX analog-bandwidth clamp to 160 MHz.

| Run | Duration/rate | Result |
|---|---|---|
| Initial balanced sweep | 33 visits, 20 MSPS | 33 valid, zero overflows, no decoded packets/candidates; before source activation |
| Second balanced sweep | 33 visits, 20 MSPS | 33 valid, zero overflows, no decoded packets/candidates |
| Remote ID reception soak | 600 s, 20 MSPS, 2437 MHz | 12,000,000,000 samples; 939 ESP32 frames; zero overflows, gaps or dropped processing chunks |
| DJI processing, initial | 60 s, 15.36 MSPS | Radio clean; 108 DSP windows dropped during ancillary GUI verification |
| DJI processing, idle repeat before fix | 60 s, 15.36 MSPS | Radio clean; 150 DSP windows dropped |
| DJI processing, after fix | 60 s, 15.36 MSPS | 921,600,000 samples; zero packet/RF drops, gaps or overflows; all 3,750 O4 windows processed |
| DJI processing, extended after fix | 600 s, 15.36 MSPS | 9,216,000,000 samples; zero packet/RF drops, gaps or overflows; all 37,500 O4 windows processed |

The two sweeps took about 61 seconds each including processing, not the advertised
28–30-second listening/scheduling budget alone. Full per-dwell records are in
`live-sweep.jsonl` and `live-sweep-final.jsonl`. No live catch-rate claim follows
from a zero-hit scan without controlled source/channel truth.

The Remote ID soak decoded `ESP32S3RIDD682C4` from Beacon/NAN frames. The transmitter
was enabled partway through the run. The log retains the newest 500 packet rows,
while the track and decoder totals record all 939 frames. Its throughput result
is ten minutes of reception, not ten minutes of continuously powered source
recall. See `live-odid-600s.log` and `live-soak-runs.json`.

A later live GUI test showed Connected, 20 MSPS and clean sample counters, but
zero Remote ID packets during that particular view; ordinary Wi-Fi FCS-valid
traffic was present. Therefore `live-map.png` proves live GUI streaming/map
rendering, not a successful live packet click. Source/model/channel clarification
was requested; the operator subsequently confirmed that no transmitter was on.
The zero-hit runs are therefore not detection failures against an active known
source. The positive click screenshots above use real recorded IQ. A positive
live click remains deferred until a transmitter is powered again.

## Throughput correction

The original single DSP thread performed DJI decoding, O4 structure analysis and
RF spectral measurements serially. Both pre-fix runs failed the zero-drop gate.
The new live path has independent bounded packet and RF/O4 queues sharing
immutable IQ buffers, so RF work cannot evict packet-decoder input. Total and
RF/O4 drops are reported separately; absolute sample indices preserve O4 gap
resets. Both consumers drain or stop before receiver teardown.

FFTW plans are reused in bounded per-thread caches keyed by transform size,
direction and input/output alignment. Planning/destruction is synchronized;
new-array execution remains independent, following
[FFTW's thread-safety guidance](https://www.fftw.org/fftw3_doc/Thread-safety.html).
Tests exercise independent queue overflow, shared-buffer lifetime, and concurrent
FFT round trips through cache eviction. Packet payload parity still passes.

## Remaining release limits

- Extended post-fix DJI throughput passed. Final archive integrity is recorded separately in `package-audit.json`.
- A positive live packet click needs a powered transmitter; the operator confirms
  none is currently on. Live decoding and recorded-IQ clicking have separately
  passed. Known-source reconnect/recovery and unplug tests remain.
- No native Windows + UHD/X310 acceptance has been performed. System DLL imports
  only, including the newly used `WINHTTP.dll`, are listed in `windows-imports.txt`.
- O4 candidate classification has known misses; O4+ remains unresolved. CRC-valid
  proprietary payloads do not imply supported telemetry parsing.
- Physical DF calibration/known-angle accuracy, broader model/channel/range
  coverage, and controlled mixed-source recall/false-alarm trials remain open.

Evidence filenames in this report refer to the repository directory
`data/analysis/final_20261029/`; raw IQ recordings are not redistributed in the
application ZIP. The matching source archive and binary hashes accompany it.
