# punjdrone — build progress

## Verification run — 2026-10-26–27 (offline checks completed; acceptance gaps remain)
- [x] Operator confirmed no X310/drone access: live hardware tests are deferred, not counted as passed.
- [x] Fixed UI track-count resets and verified Mini 2 (10 packets) / Mavic Air 2 (1 packet) through UI workers.
- [x] Separated live RX and DSP with a bounded drop-counted queue; added overlap and timestamp-gap resets.
- [x] Release 12/12 CTest, headless ASan 11/11, and corpus-harness 5/5 tests pass.
- [x] Native Mini 2 and unsupported-payload rendering inspected; 8/8 retained real-IQ excerpts match
      CLI/UI payloads exactly. Unsupported CRC payloads are logged without invented telemetry/tracks.
- [x] Local corpus sweeps: 76 quick + 45 deeper cases, no runner errors; 76 unique source paths,
      38.519 seconds unique prefix coverage. No IQ downloads; not whole-corpus coverage.
- [x] New CRCs: Air 2S (2), Mavic 3 (2), Mavic 3 Pro (2), Mini 3 (2), KU Leuven Mini 2/Matrice/MavicRC1.
      O3+ formats remain unsupported telemetry; detailed evidence: `docs/VALIDATION_20261026.md`.
- [x] O1 Mavic Pro resolved on 2026-10-27 after remount: one CRC-valid v2 packet, serial
      `08RDE310010218`. Auto-center chose other link activity; explicit -4.5703125 MHz shift decodes.
      Full-file CLI/UI payloads match exactly (120M samples, one track/count 1); native UI inspected.
      Evidence: `data/analysis/o1_followup_20261027/REPORT.md`; diagnostic tests 2/2 pass.
- [ ] O4 identity accuracy: backend currently abstains (`unknown_rf`); energy activity is not O4 identification.
- [x] 2026-10-27 envelope audit: all 22 unique saved transport payloads also pass inner CRC16.
      CLI/UI show format/checksum separately; supported v2 parsing requires inner CRC and valid length.
      Nine real-IQ clips agree exactly across CLI/UI; release 12/12 and Python 6+2 tests pass.
      Evidence: `data/analysis/envelopes_20261027/REPORT.md`. O3+ v3/INFP telemetry remains unsupported.
- [x] Direction-finding completion plan: `docs/DIRECTION_FINDING_PLAN.md`, DF-1 through DF-6,
      software-first milestones, calibrated paired pipeline, bench/field gates and proposed error targets.
- [ ] Live DF same-session calibration: cross-session reuse lacks validated phase-repeatability/provenance;
      GUI path fails closed. UBX/X310 timed LO synchronization is supported, not yet measured on this rig.

Checklist shared between the operator and the coding agent. The checklist UI panel was removed;
the agent updates this file as work lands. States:
`- [ ]` todo · `- [/]` in progress · `- [x]` done. Lines that aren't checkboxes are left untouched.

See [`DRONEID_BUILD_PLAN.md`](DRONEID_BUILD_PLAN.md) for the detail behind each item.

## Phase 0 — Scaffold
- [x] CMake build (C++17, Ninja), feature toggles for UHD/UI/SQLite/GTest
- [x] Core types + protocol constants (`types.h`)
- [x] Lock-free SPSC ring buffer + tests
- [x] Leveled logger (`logging.h`)
- [x] Config loader (TOML-subset) + tests
- [x] `IqSource` interface + `FileSource` (cf32/sc16/sc8) + tests
- [x] No-op `Pipeline` (sample count + mean power)
- [x] `iq_replay` tool (file -> ring -> pipeline)
- [x] `iq_gen` tool (synthetic IQ for tests; never transmitted)
- [x] App entry (`droneid`) loads config, `--no-ui`/`--version`
- [x] README with host setup (MTU/sysctl) + build/run
- [x] **Accept:** `iq_replay` streams a file through a no-op pipeline; `ctest` green (3/3)

## Phase 1 — Capture & characterize (Mini 2 + Air 3S)
- [x] `UsrpSource` (Radio#0, master_clock_rate 184.32e6 via device args, 15.36 MSPS, addr=192.168.10.2)
- [x] `iq_record` live capture with metadata sidecar
- [x] Capture scripts for 2.4 GHz and 5.8 GHz DroneID center frequencies (`scripts/capture.sh`)
- [x] Burst-analysis tool `burst_scan`: bandwidth, symbol-count est, burst period, occupancy (FFTW)
- [x] Hardware smoke test: 15.36 MSPS exact, 0 overflows; burst_scan found 440 ambient bursts @ 2.4595 GHz
- [x] Capture report scaffold + baseline (`data/CAPTURE_REPORT.md`)
- [x] Record Mini 2 in flight — 2.4 GHz (5 slices) captured, 0 overflows; strong structured bursts seen
- [x] Dwell capture 30 s on 2.4595 GHz (centered) — still 0 bursts; scan peak corr 0.059 (noise)
- [ ] **Capture with GPS lock**: repeat outdoors with a verified fix to validate telemetry.
      GPS gating is NOT established: the public Mini 2 reference carries phone coordinates
      despite having no drone position fix. The cause of our missing local beacon remains unknown.
- [x] Band sweep: `scripts/sweep_droneid.sh` captures every DroneID channel centered + decodes each
      (proven 15.36 path) — catches the beacon on whatever channel the drone uses
- [x] Outdoor GPS-locked 5-channel sweep: still NO beacon (max ZC corr ~0.11 = noise; ref hits 0.9+).
      Our local Mini 2 isn't emitting a detectable legacy DroneID beacon on 2.4 — try 5.8 band, verify
      GPS lock quality, or this unit/firmware/region may differ. Decoder itself is proven on the ref.
- [x] Verified wideband capture streams clean at MTU 1500 (61.44 & 92.16 MSPS, 0 overflows)
- [/] `channelize` tool (wideband DDC): works for presence detection; clean decode through decimation
      needs more work (ISI at symbol boundaries) — sweep script is the reliable decode path for now
- [x] Record Air 3S (O4) in flight — 2.4 (5 slices) + 5.8 (4 slices) captured, 0 overflows
- [x] `spectrogram` tool (STFT -> PNG via FFTW+zlib); 5.8 video link confirmed, 2.4 congested
- [/] DroneID presence: drones clearly present, but beacon not isolable by energy/visual — needs ZC correlator
- [ ] Two-channel coherent capture (both UBX, splitter) for DF phase-cal baseline
- [/] Fill capture report — done for captures; §2 constants NOT yet confirmed (needs ZC detector)
- [ ] **Accept:** Mini 2 bursts isolated matching §2 — BLOCKED on Phase 2 ZC matched filter

## Phase 2 — Offline decoder (no live radio)
- [x] Ported full chain from GPL reference proto17/dji_droneid (+ vendored turbofec, CRCpp)
- [x] `frame_gen` synthetic DroneID bursts (payload in == out)
- [x] Burst detector (ZC xcorr root 600, normalized, FFT overlap-add)
- [x] OFDM demod (CP strip, 1024 FFT, STO via CP, coarse CFO, ZC channel eq, QPSK)
- [x] FEC: LTE Gold descramble, rate de-match, turbo, CRC-24-LTEA
- [x] Payload parser + unit scaling (`proto/frame.cpp`) — field offsets to confirm on a real frame
- [x] `droneid_decode` tool (+ `--scan` freq-offset diagnostic)
- [x] **Validated end-to-end on synthetic**: detect corr 0.99, turbo+CRC OK, serial recovered exactly
- [x] Fixed turbofec SSE requirement (no non-SSE fallback → zeros); 4/4 tests pass
- [x] **Detector validated on REAL DJI data** — RUB-SysSec `mini2_sm` reference (DJI Mini 2, 50 MSPS):
      resampled to 15.36 and detected 10 bursts at corr 0.78–0.96. create_zc, OFDM sync, detection all
      confirmed correct on genuine DroneID.
- [x] **Found: real captures have a large carrier offset** — `mini2_sm` signal sits at +9.67 MHz; must
      locate the ~10 MHz band and CENTER it before resampling/decode (center-at-DC slices the signal).
      Our own field captures had no detected beacon; the cause is still unconfirmed.
- [x] **Real-burst CRC fixed (2026-10-26)**: CP CFO is ambiguous modulo 15 kHz. Joint matching
      of roots 600 and 147 resolves timing and integer-carrier offset (initially ±16 carriers). The saved
      centered `mini2_ref2.iq` had a -57.47 kHz residual CFO, previously mistaken for +2.53 kHz.
      All 10 detected bursts now pass CRC-24, with and without LPF. Fractional timing was not
      the cause of these failures; no timing-search or phase-walking patch was needed.
- [x] **Mavic Air 2 reference CRC fixed (2026-10-26)**: auto-centering leaves -485.94 kHz
      residual CFO, outside the original ±240 kHz search. Expanded joint-ZC search to ±64
      carriers (±960 kHz) and compensated the LPF's 25-sample delay, which otherwise pushes
      alignment outside the window. The sample decodes with the existing version-2 parser.
      Added synthetic regressions for both signs of this larger offset, with/without LPF.
- [x] Golden real-IQ test (64 kB fixture), positive/negative CFO regressions, and file-chunk tests.
      Fixed whole-file default, skipped chunk-boundary samples, and final sample-limit handling.
      Release CTest: 4/4 passing (7 DJI cases). Clean Debug AddressSanitizer build: 4/4 passing
      with leak detection disabled because LeakSanitizer cannot run under this environment's ptrace.
- [x] Correct v2 telemetry field boundaries, phone coordinate order, and 64-bit GPS timestamp;
      verified serial/sequence/phone location against the real fixture. Motion units remain provisional.
- [ ] Support explicit 8-symbol legacy frames, inner 16-bit CRC, and validate nonzero motion units.
- [x] Integrated streaming centering/resampling: original 50-MSPS `mini2_sm` now yields
      10/10 CRC-valid detections with `--auto-center` or explicit `--freq-offset -9600000`.
      FIR anti-alias filtering, persistent history and absolute sample timing support fractional
      ratios; memory stays bounded on files. Sample indices and limits use original input units.
- [x] Raw wideband golden fixture, 61.44/92.16-MSPS synthetic decode, passband/alias rejection,
      chunk-equivalence and CLI metadata/duration tests. Release CTest: 6/6 passing.
      Clean Debug AddressSanitizer: 6/6 passing (`detect_leaks=0`, same ptrace limitation).
      Reject numerical near-silence in FFT correlation to prevent false peaks from FIR tails.
- [x] Rechecked all five saved outdoor centered Mini 2 sweeps, full 8 s each, threshold 0.3:
      still zero candidates and zero CRC-valid frames. This does not identify the cause of absence.
- [x] Full data-folder recheck: all 21 local IQ files, 51 full-file decode passes and 21 prefix
      scans. Mini 2: zero candidates; Air 3S: 183 candidate hits across passes, zero CRC-valid;
      baseline: zero candidates. Two Air 3S examples fail the second-ZC match. Reference: 10/10.
      Evidence and limitations: `data/analysis/local_recheck_20261026/REPORT.md`.
- [/] **Accept:** saved centered Mini 2 reference yields 10/10 CRC-valid detected bursts, phone GPS
      only. Upstream's README reports 7/9 for `mini2_sm`; its separate 14/18 example is not this
      fixture's acceptance count. Raw-rate preprocessing is now validated; a CRC-valid local capture remains.

## Phase 3 — Live narrowband (1GbE) + UI shell
- [x] Native operator UI (`droneid_ui`, Dear ImGui + GLFW/OpenGL3) under `-DDRONEID_WITH_UI=ON`.
      UI reads the public decode/parse APIs. Thread-safe `UiModel`; render
      thread reads snapshots, worker thread decodes. Verified: builds, `--frames` renders real frames
      on a display, `--check` decodes the reference headless (packets=1, tracks=1).
- [x] UI shell: USRP/source bar (state dot, label, rate/center/gain/ref/overflow), drones table,
      decoded-packets log, direction compass. (Right-side checklist panel removed per operator request;
      the `UiModel` checklist parser/`test_ui_model` stay for possible later use.)
- [x] Connect/Disconnect/Reconnect **wired to the worker** — start / stop / restart either the replay
      source (`--replay FILE`) or the **live radio** (`--device ARGS [--freq] [--gain] [--ant]`);
      "No source" / "No UHD" shown gracefully. Headless model/worker/CLI tests; 12/12 release CTest.
- [x] **Live streaming-decode loop wired to `UsrpSource`**: separate RX/DSP threads, bounded queue,
      overlapping windows and timestamp-gap resets; samples/gaps/drops shown. Compiled with UHD.
      Historical X310 two-second smoke test is not acceptance of this revised pipeline. No hardware
      available for the current verification run; no device contacted.
- [x] Decoded-packets panel live from either source.
- [ ] Live DroneID frame decode in the UI: deferred until radio/transmitter available; soak/resync pending.
- [x] RF metrics wired into the drones panel: `EnergyBurstDetector` + `measure_rf` over the sample
      stream → an **"unknown RF" activity line** (burst count, peak RSSI, last SNR/BW), honestly *not*
      a drone count. Verified offline; revised live path still needs hardware acceptance.
- [ ] GUI DF disabled (fails closed): calibration-session/retune repeatability is unverified. Integrate a
      same-session phase reference before exposing bearings. Paired-IQ core tools remain available.
- [ ] ImPlot polar view (deferred; compass currently drawn with ImDrawList)
- [ ] **Accept:** live Mini 2 decode, zero overflows over 10 min on 1GbE budget; UI drives the radio

## Phase 4 — Channel dwell/hop (1GbE coverage)
- [x] Headless `droneid_hop`: band schedules, guarded UHD timed tuning/start, bounded dwell decode,
      RX timestamp/overflow rejection, JSONL visit and hit reports. UHD build tested; not live-tested.
- [/] Dwell scheduler across known DroneID channels (timed tuning): hardware settling/coverage pending.
- [/] Dwell time from supplied period and conditional random-phase catch model implemented/tested;
      actual local DroneID period and measured catch rate remain unverified. Decode adds logged gaps.
- [ ] **Accept:** detects Mini 2 on any DroneID channel; report shows catch rate

## Phase 5 — O4 path (Air 3S)
- [x] Bounded streaming energy detector + `rf_analyze` JSONL RSSI/SNR, 99%-power bandwidth,
      spectral centroid and clipping metrics; chunk/continuous-input tests and four full local-file checks.
      All RF events are `unknown_rf`: baseline has activity too. Counts are not unique drones.
- [ ] Burst classifier (bandwidth/period/CFO signature); detect + count drones
- [/] RF metrics: RSSI/SNR implemented; source-specific CFO, proximity and verified fingerprint pending.
- [ ] Drones-detected panel populated (incl. undecoded O4)
- [ ] **Accept:** reliably flags Air 3S, separates from Wi-Fi, logs metrics; no false position


## Phase 6 — Direction finding (core, both antennas)
- [x] Detailed completion plan with dependencies and explicit acceptance gates: `docs/DIRECTION_FINDING_PLAN.md`.
- [x] `iq_record_pair`: two-channel UHD RX, common timed tuning/start, sc8 wire/cf32 disk,
      sample continuity checks, paired metadata; compiled only, hardware coherence not yet verified.
- [/] Dual coherent RX: common time configured; independent UBX LOs still require physical calibration.
- [x] Calibrated phase-difference bearing API + `df_bearing` CLI, coherence/spacing/freshness guards,
      front/back ambiguity explicit; synthetic-angle and CLI calibration roundtrip tests.
- [/] Splitter/known-source calibration file workflow and `config/x310_df.toml` added;
      actual same-session physical calibration and measured angular error remain pending.
- [ ] Bearings for both O2/O3 (decoded) and O4 (raw burst)
- [ ] Direction panel live; sc8 on 1GbE / sc16 on 10GbE
- [ ] **Accept:** repeatable bearing to Mini 2 and Air 3S after cal, with measured error envelope

Implementation/usage: `docs/PHASE456.md`. Phase numbering follows this checklist;
wideband/Wi-Fi/SQLite (called Phase 6 in the original plan) is not included here.
Initial Phase 4–6 release/no-UHD AddressSanitizer checks were 9/9; the integrated release now passes
12/12 suites (earlier headless ASan: 11/11). Local RF smoke-check evidence:
`data/analysis/phase456_rf_20261026/REPORT.md`. Phase 3 GUI/replay and live plumbing are implemented;
live hardware and calibrated DF acceptance remain open.
