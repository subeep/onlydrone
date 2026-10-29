# Running punjdrone on Windows — step-by-step guide

A plain, start-to-finish guide for running `droneid_ui.exe` on a Windows PC.
For build internals and acceptance detail, see [`WINDOWS.md`](WINDOWS.md).

There are **three ways** to use it, with different requirements:

| Mode | What it does | Extra install needed |
|---|---|---|
| **Offline replay** | Open a recorded IQ file, decode DJI DroneID / Wi-Fi Remote ID, show the O4 candidate counter | **Nothing** — just the `.exe` |
| **Live X310** | Receive from the USRP X310 radio in real time | The **UHD 4.x runtime** (plus radio networking) |
| **Automatic sweep** | Retune through DJI DroneID, O4 and Wi-Fi Remote ID frequencies | The same UHD runtime and networking; see `AUTOMATIC_SWEEP.md` |

Start with offline replay — it needs nothing but the file and confirms the app runs on your machine.

---

## 1. Requirements

Your Windows PC needs:

- **Windows 10 or 11, 64-bit** (x64).
- An **x86-64 CPU that supports SSE4.1** — any Intel/AMD desktop or laptop from the last ~12 years.
- A **graphics driver with OpenGL 3.0** (the GUI is OpenGL). If the window fails to open, update your GPU driver (Intel/AMD/NVIDIA) first.

You do **not** need Python, Visual C++ redistributables, or any installer for offline use — the program is statically linked and only uses DLLs that ship with Windows (`KERNEL32`, `USER32`, `GDI32`, `SHELL32`, `COMDLG32`, `MSVCRT`, `OPENGL32`).

---

## 2. Get the files onto Windows

Copy the Windows build folder to your PC (USB stick, network share, or download). At minimum you need **`droneid_ui.exe`**. The other `.exe` tools (`o4_detect.exe`, `odid_decode.exe`, `df_bearing.exe`, `droneid_decode.exe`, `rf_analyze.exe`, `iq_record_pair.exe`, …) are optional command-line tools; the GUI is the main program.

Put `droneid_ui.exe` in its own folder, e.g. `C:\punjdrone\`.

> **Note:** the build is currently **unsigned** and has been tested under Wine, not yet on native Windows hardware. Treat the first run as a test.

---

## 3. First run

1. Double-click **`droneid_ui.exe`**.
2. Windows SmartScreen may warn: *"Windows protected your PC."* This is expected for an unsigned app. Click **More info → Run anyway**. (Your antivirus may also ask — allow it.)
3. The GUI window opens, and a **console window** also opens beside it for diagnostic text. That console is normal — leave it open.

If the window opens, you're ready. If it flashes and closes, read the console text (OpenGL or SSE errors point to the GPU driver or an unsupported CPU).

---

## 4. Offline replay (no radio)

This plays back a recorded IQ capture.

1. In the top bar, choose **IQ replay**, click **Browse**, and pick a raw IQ file.
2. Set the recording's real parameters:
   - **Format**: `sc16`, `sc8`, or `cf32` (little-endian interleaved I/Q).
   - **Sample rate** in MSPS.
   - **Center frequency** in MHz.
   - If a `<filename>.meta` sidecar sits next to the file, it overrides these automatically.
3. For wideband DJI captures where the burst is off-center, tick **Auto-center DJI bursts**. (Not needed for O4 evidence.)
4. Click **Connect**. Leave duration at **0** to play the whole file. Click **Disconnect** before changing any setting.

What you should see:
- **Decoded packets / Drones** populate for DJI DroneID (O1/O2/O3) and Wi-Fi Remote ID (ASTM F3411) captures.
- The **O4 candidate** counter increments separately — it never invents a serial, position, or bearing.
- Click a **Decoded packets** row to show packet details and its reported position on the
  **OpenStreetMap** panel. Without valid drone coordinates, the map defaults to Punjab,
  India. Live bearings still require hardware calibration; the map does not infer them.
- Uncached map tiles need internet. **Online tiles** can be switched off for cached tiles
  only. See [`PACKET_MAP.md`](PACKET_MAP.md) for controls, attribution and cache locations.

> You need an actual IQ recording to see anything — the app is a receiver, not a simulator. HDF5/`.mat`/zip files must be exported to raw IQ first; do **not** just rename them to `.iq`.

---

## 5. Live X310 reception (needs UHD installed)

Only do this if you have the USRP X310 radio. Offline replay does **not** need any of the following.

### 5a. Install the UHD runtime
1. Download and install the official **Ettus UHD 4.x, 64-bit** Windows runtime (for example UHD 4.1.x) from Ettus Research. Install its dependencies too.
2. After install, the main library is typically at **`C:\Program Files\UHD\bin\uhd.dll`**. The app looks there by default.
3. If your UHD is elsewhere, point the app at the exact DLL before launching, in PowerShell:
   ```powershell
   $env:DRONEID_UHD_LIBRARY = 'D:\UHD\bin\uhd.dll'
   .\droneid_ui.exe
   ```
4. Make sure UHD's own folder (with its dependent DLLs) is on your `PATH`, or its DLLs sit next to `uhd.dll`. UHD only loads when you **Connect** — replay works without it.

### 5b. Connect the radio
1. Plug in the X310 Ethernet and set your PC's network adapter for the radio subnet. The reference bench uses PC `192.168.10.1`, radio `192.168.10.2`.
2. Verify the link with UHD's own tool first (e.g. `uhd_find_devices`) and allow its traffic through the Windows firewall.

### 5c. Run it
1. In the app choose **Live X310**, device `addr=192.168.10.2`, antenna `RX2`, and set center frequency and gain.
2. The standard DJI profile is **15.36 MSPS / 184.32 MHz master clock**. Selecting **Wi-Fi Remote ID** switches to the **20 MSPS / 200 MHz** profile. (The sample-rate field is read-only for live profiles.)
3. Click **Connect** and confirm sample counts advance. Watch **overflows**, **timestamp gaps**, and **dropped DSP windows** — these should stay at zero on a healthy stream.

> Live **direction finding is disabled** until its physical calibration gates are met. A clean network stream is not the same as validated detection coverage — run a known transmitter for real acceptance.

---

## 6. Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| "Windows protected your PC" | Unsigned app — **More info → Run anyway**. |
| Window flashes then closes | Read the console. OpenGL error → update GPU driver. SSE/illegal-instruction → CPU lacks SSE4.1. |
| Nothing appears on replay | Wrong format/rate/center, or the file isn't raw IQ (HDF5/`.mat`/zip need exporting first). |
| Connect fails, **error 126** | `uhd.dll` or one of its dependency DLLs couldn't load — check `DRONEID_UHD_LIBRARY` and that UHD's DLLs are on `PATH`. |
| Connect fails, **error 193** | 32/64-bit mismatch — install the **64-bit** UHD runtime. |
| "Missing required C export" | UHD version too old/incompatible — install a supported UHD 4.x. |
| No radio found | Wrong subnet/IP or firewall — confirm with `uhd_find_devices` first. |

---

## 7. What's verified vs. not

- The binary is a genuine **PE32+ x86-64** Windows program with the GUI, DJI decoder, O4 candidate detector, and Wi-Fi Remote ID decoder compiled in.
- It has been exercised **under Wine on Linux**. **Native Windows hardware acceptance (and live X310 on Windows) is still pending.**
- It is **unsigned**.

Treat the first native-Windows and first live-radio runs as validation steps, not a finished acceptance.
