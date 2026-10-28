punjdrone — Windows x64 package
================================

Passive DJI DroneID + Wi-Fi Remote ID (ASTM F3411) receiver. Receive-only.

QUICK START
-----------
1. Double-click  droneid_ui.exe
   (Unsigned build: on the SmartScreen warning click "More info" -> "Run anyway".)
2. To confirm it works, double-click  Replay-example.cmd
   You should see 1 CRC-valid packet and 1 track from the bundled Mini 2 excerpt.

FULL INSTRUCTIONS
-----------------
- WINDOWS_RUN_GUIDE.md  = step-by-step guide (what to install, offline replay, live X310, troubleshooting)
- WINDOWS.md            = build/acceptance reference and live-radio detail

OFFLINE REPLAY needs nothing but these files.
LIVE X310 RADIO additionally needs the official 64-bit UHD 4.x runtime installed
(see WINDOWS_RUN_GUIDE.md, section 5).

CONTENTS
--------
  droneid_ui.exe          main GUI application
  tools\                  optional command-line tools (decoders, recorders, analysis)
  example\                a small licensed Mini 2 IQ excerpt + .meta sidecar
  Replay-example.cmd      one-click self-test
  WINDOWS_RUN_GUIDE.md    run/install guide
  WINDOWS.md              reference
  COPYING                 GPL-3.0 license
  THIRD_PARTY.md          third-party components and licenses
  SOURCE.txt              where to get the source (GPL requirement)

STATUS
------
This build has been exercised under Wine on Linux. Native Windows hardware
acceptance and live X310 reception on Windows are still pending. It is unsigned.

License: GPL-3.0-or-later. See COPYING.
