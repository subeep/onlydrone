# Windows x64 application

Build date: 28 October 2026. Extract the complete `punjdrone-windows-x64.zip`
directory, then double-click **droneid_ui.exe**. This is a native Windows PE x64
program, with the GUI, DJI decoder, O4 candidate detector and Wi-Fi Remote ID
decoder compiled in. A console also opens for diagnostics. No Python is needed.

Requirements: Windows x64, an x86-64 CPU with SSE4.1, and an OpenGL 3.0-capable
graphics driver. The supplied binary has been exercised under Wine on Linux;
native Windows hardware acceptance is still required. It is unsigned.

## IQ replay

1. Select **IQ replay**, click **Browse**, and choose a raw IQ recording.
2. Set the actual recording format (`sc16`, `sc8`, or `cf32`), sample rate in
   MSPS, and center frequency in MHz. Data are little-endian interleaved I/Q.
   A `<filename>.meta` sidecar overrides rate, format and center frequency.
3. Enable **Auto-center DJI bursts** for wideband DJI DroneID captures where the
   burst is offset from the recording center. It is not needed for O4 evidence.
4. Click **Connect**. Disconnect before changing settings. Zero duration means
   the full file, or continuous operation for live reception.

HDF5/MAT and compressed archives must first be exported to raw IQ. They are not
raw files and must not be renamed to `.iq`. Existing Python dataset adapters
remain in the source package; no datasets were downloaded for this release.

The package includes a small licensed Mini 2 regression excerpt. Run
`Replay-example.cmd`: one CRC-valid payload/track should appear. This is a
reference fixture, not a live drone. O4 appears in a separate candidate-window
counter; it does not invent an identity, position, or bearing.

## Live X310 reception

1. Install the official **64-bit UHD 4.x runtime** and its required dependencies.
   Use a UHD/FPGA pair compatible with your radio. The adapter has been exercised
   with UHD 4.1.0.5 on Linux; each Windows runtime/radio combination needs a check.
   Do not update the radio FPGA just to launch this application.
2. The default DLL location is `C:\Program Files\UHD\bin\uhd.dll`. For another
   installation, set `DRONEID_UHD_LIBRARY` to the **absolute** DLL path before
   launching, for example in PowerShell:

   ```powershell
   $env:DRONEID_UHD_LIBRARY = 'D:\UHD\bin\uhd.dll'
   .\droneid_ui.exe
   ```

   UHD's dependent DLLs must be installed alongside it or in a Windows loader
   search directory. UHD is loaded only when connecting: replay works without it.
   Error 126 means the DLL or one of its dependencies could not be loaded; 193
   commonly indicates a 32/64-bit mismatch. Missing required C exports are named.
3. Connect the X310 Ethernet port and configure the dedicated PC adapter for
   the radio subnet (the existing bench uses PC `192.168.10.1`, radio
   `192.168.10.2`). Check reception with UHD's own utility first and allow its
   required network traffic through the host firewall.
4. Select **Live X310**, device `addr=192.168.10.2`, antenna `RX2`, center frequency
   and gain. The normal profile is 15.36 MSPS, 184.32 MHz master clock, fc32 host
   and sc16 wire. **Wi-Fi Remote ID** selects the separate 20 MSPS / 200 MHz
   profile. The MSPS field is read-only for live profiles.
5. Click **Connect**, confirm sample counts advance, and watch **overflows**,
   **timestamp gaps**, and **dropped DSP windows**. A healthy network stream alone
   does not establish full detection coverage. Disconnect and reconnect once.
6. For acceptance, run a known transmitting drone for at least ten minutes,
   require zero unexplained gaps/drops, compare expected packet/evidence counts,
   and test disconnect/recovery. Repeat with O4-on/O4-off and Wi-Fi-only controls.

The C API backend is receive-only. It supports radio metadata, timed starts,
tuning, overflow accounting and timestamps. FPGA version is explicitly marked
unknown because this C interface does not expose the property-tree value.
**Live direction finding remains disabled** until its existing physical
calibration acceptance gates are met.

The DLL loader uses official C declarations and frees UHD allocations through
UHD functions, avoiding a dependency on the MSVC C++ ABI. Required symbols and
the UHD major version are checked before opening hardware. This is not proof
that every UHD 4.x release is compatible.

## Build and reproduce

On Linux install the POSIX-thread MinGW-w64 x64 C/C++ compilers, CMake, Make,
curl and tar. Run `bash scripts/build_windows.sh` with those tools on PATH.
It verifies pinned GLFW 3.4 and FFTW 3.3.10 source hashes, builds static libraries,
then enables the UI and dynamically loaded UHD C API. It downloads about 5 MB
of dependency source, no IQ. For an offline build put the supplied dependency
archives in `DRONEID_WINDOWS_DEPS` first. The tested compiler is MinGW GCC 10.3.

FFTW, GLFW, libstdc++ and the MinGW runtime are linked statically. Ordinary
Windows system DLLs and graphics drivers remain OS dependencies. UHD is an
optional external runtime. The portable FFT fallback also builds, but is slower
and is not the backend selected for the packaged executable.

Run `python scripts/verify_windows.py --build build/windows` on Windows for
headless checks. On Linux add `--runner /path/to/wine64` with a configured Wine
environment. Full native Linux validation is `ctest --test-dir build
--output-on-failure`.

The matching source archive includes project source, patches, licenses, tests,
and the pinned GLFW/FFTW dependency archives. Keep it with the binary if you
redistribute the GPL components. No UHD runtime DLL is redistributed.

Primary build references: [GLFW compilation](https://www.glfw.org/docs/latest/compile_guide.html),
[FFTW on Windows](https://www.fftw.org/install/windows.html),
[UHD C API](https://files.ettus.com/manual/page_c_api.html),
[UHD Windows build guide](https://files.ettus.com/manual/page_build_guide.html).
Validation evidence and outstanding hardware checks are in
`data/analysis/windows_20261028/REPORT.md` in the workspace.
