# Final verification summary

Date: 29 October 2026. This summarizes the automated checks behind the current
Windows package. It is **software/Wine evidence only** — no native Windows hardware
acceptance and no radio reception is claimed. Raw logs are under `data/analysis/`.

## Build provenance
- Windows x64 binaries built with MinGW-w64 from the current working tree
  (base commit `bb6bf23` plus local uncommitted changes: automatic sweep, map view,
  and a thread-safe FFTW plan cache in `src/dji/dsp.cpp`).
- The packaging script refuses to ship any `.exe` whose bytes differ from the
  current `build/windows/` build, so the package always matches what was tested.

## Linux (native) tests
- **18 / 18 CTest suites pass** (`data/analysis/software_handoff_20261029/linux-ctest.log`),
  including the real Mini 2 decode (`test_dji`), resampler/FFT, DF paired-pipeline,
  ODID Wi-Fi, O4 detector and the sweep CLI.
- Source integrity: 189 source files verified; archive CRC and SHA-256 checks pass.

## Windows binaries under Wine
Fresh `build/windows/` binaries (rebuilt after the FFT change) were executed under
`wine64` and all passed (`data/analysis/final_20261029/windows/checks.json`):
- **13 unit-test suites** green: `test_ring_buffer`, `test_file_source`, `test_config`,
  `test_receiver`, `test_ui_model`, `test_odid`, `test_odid_wifi`, `test_ui_worker`,
  `test_dji` (14/14 cases), `test_resampler`, `test_rf_metrics`, `test_o4_detector`.
- **Functional checks** green: Mini 2 wideband replay decodes; missing-input and
  invalid-rate return correct non-zero exit codes; `droneid_sweep.exe --profile
  balanced` runs; invalid sweep settle is rejected.

## Capability status (unchanged by this build)
- **DroneID O1/O2/O3 (v2):** decoded to telemetry (serial/GPS). Real Mini 2 10/10 CRC.
- **O3+/O4+ (Mavic 3, Mavic 3 Pro, Mini 5 Pro):** DroneID envelope detected and
  CRC-valid, but payload format unsupported — no telemetry fields.
- **O4 (Air 3S, Mini 4 Pro, Avata 2):** structural **candidate** detection only,
  not confirmed identity; payload encrypted/undecoded. Recording-level labels,
  not held-out session identity accuracy.
- **Wi-Fi Remote ID (ASTM F3411):** decoded; previously hardware-verified on an
  ESP32 bench transmitter.
- **Direction finding:** software complete and fail-closed; physical calibration
  and measured angular error still pending.

## Explicit limits
- Wine execution is not native Windows acceptance; live X310/UHD cannot be
  Wine-tested and needs a real Windows host.
- O4 identity, O4+ decode and physical DF acceptance are incomplete.
- A valid CRC detects corruption; it does not authenticate identity or prove a
  payload is unencrypted.
