# Windows port and sweep verification, 28 October 2026

The native x64 PE GUI and command-line tools were cross-compiled with MinGW GCC
10.3 (POSIX threads), GLFW 3.4 and static single-precision FFTW 3.3.10/SSE2.
The dynamic receive-only UHD C API adapter loads x64 UHD separately; replay
needs no UHD installation. `imports.txt` lists only normal Windows system DLLs.

## Executed checks

- Before sweep integration: **15/15 Windows checks under Wine 6.0.3**. Twelve
  C++ test executables plus exact Mini 2 GUI-backend replay, missing-input and
  invalid-rate checks. See `checks.json`, `verification.log` and individual logs.
- Correct 50 MSPS Mini 2 replay: one outer-CRC-valid packet, parsed v2 telemetry,
  valid inner CRC16 and one `SysSecWasHere` reference-fixture track.
- Missing UHD DLL: clean `status=Error`, explanatory install/path message,
  no samples or false connected state (`missing-uhd.log`).
- Windows GUI rendered under Wine and visually inspected (`windows-gui.png`).
  This screenshot predates the added Automatic sweep controls.
- Native Linux dynamic-C-API build: **17/17 CTest** before sweep integration.
- Updated native Linux build including sweep: **18/18 CTest**. New profile,
  coverage, rotation, timing, continuity, O4 retune-reset and CLI checks pass.
- Python corpus/DF/etc. tests: **14/14** before sweep integration.
- Updated Windows GUI and `droneid_sweep.exe` compile successfully. All 15 core
  checks passed again after integration, plus three additional checks: sweep plan,
  invalid-settle rejection and O4 GUI replay. The Windows O4 CLI and GUI both
  report 13 candidate windows / 20 evaluated windows on the same 0.32-second
  Air 3S prefix. The new sweep controls were rendered and visually inspected.
  See `../sweep_20261028/windows/`, `windows-additional-checks.json` and
  `windows-sweep.png` in that directory. Native Windows radio tests remain pending.

## Actual X310 observations through the new C API adapter

Receive-only, UHD 4.1.0.5-3, 184.32 MHz master clock / 15.36 MSPS, on Linux:

| Run | Samples | Radio overflows | Timestamp gaps | Dropped DSP windows |
|---|---:|---:|---:|---:|
| Portable FFT, 2 s | 30,720,000 | 0 | 0 | 20 |
| FFTW, 5 s | 76,800,000 | 0 | 0 | 10 |

The FFTW check ran alongside builds/evaluations, so it does not isolate steady-state
CPU capacity. The requested idle-host repeat was **not executed**: automatic
approval review failed because of a usage limit. This was a review-service failure,
not a finding that reception was unsafe. No radio checks bypassed that block.
After the advertised reset time passed, a normally approved bounded sweep was
attempted at 23:15 IST. UHD could not find the X310 at `192.168.10.2`; no dwell
was captured. Evidence: `../sweep_20261028/live-fast.log`.

These checks prove basic C API radio transport on Linux. They do not establish
complete live detector coverage, Windows X310 operation, known-drone recall or
the new 20 MSPS sweep's hardware settling/continuity. No drone transmission was
controlled during these checks. Sweep listening intentionally pauses during DSP
and reports the blind intervals instead of hiding them as queue drops.

## Delivery and remaining acceptance

`dist/punjdrone-windows-x64.zip` contains GUI/tools, licenses, instructions and a
small licensed replay example. `dist/punjdrone-windows-source.zip` contains matching
working-tree source plus pinned GLFW/FFTW archives. `BUILD_MANIFEST.json` hashes
the binaries and source snapshot; `dist/SHA256SUMS.txt` hashes both archives.

Still required: native Windows with compatible UHD and X310, new sweep hardware
validation, at least ten minutes of load/recovery testing, controlled O4-on/off
and mixed-source tests, and physical DF calibration/known-angle verification.
Follow `docs/WINDOWS.md`, `docs/AUTOMATIC_SWEEP.md` and the existing DF runbook.
