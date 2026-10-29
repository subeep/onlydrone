# Automatic sweep verification — 28 October 2026

Implemented an interleaved 20 MSPS / 200 MHz receiver schedule, GUI source mode
and headless `droneid_sweep` tool. The balanced plan has 33 entries and a
28.43–29.75-second cycle budget before processing; the expanded thorough plan
has 51 entries and a 69.29–71.33-second budget. These include command lead,
settling and bounded jitter, not measured hardware execution time.

- Linux: 18/18 CTest suites pass (`linux-ctest.log`).
- Windows: successful x64 cross-build (`windows-build.log`).
- Updated Windows under Wine: 15/15 core checks (`windows/checks.json`), plus
  3/3 plan/invalid-settle/O4-GUI checks (`windows-additional-checks.json`).
- Windows O4 CLI and GUI: 13 candidates / 20 windows, 4,915,200 samples from
  the same local 15.36 MSPS Air 3S prefix. No fabricated identity/track.
- Additional shared-rate worker tests: real Mini 2 fixture resampled to 20 MSPS
  decodes with valid inner CRC16, synthetic Wi-Fi RID decodes at 20 MSPS, and
  packet totals remain correct after the bounded UI log fills. All 13 worker
  cases pass on Linux and Windows/Wine (`worker-shared-rate.log` and
  `windows-worker-shared-rate.log`).
- Actual Windows GUI with Automatic sweep selected rendered and was inspected
  (`windows-sweep.png`). Its missing-UHD message is expected in the Wine environment.
- Attempted one receive-only fast cycle on the real X310 at 23:15 IST.
  UHD discovery failed: no device at `192.168.10.2`. **No dwell was executed**;
  no sweep continuity, live recall or settling result is claimed (`live-fast.log`).

Plan JSONL files were generated without hardware. Source tests check frequency
coverage, complete-cycle rotation, time budgets, late first timestamps, gaps,
overflow and partial-capture rejection, and O4 history isolation across retunes.
Full acceptance still needs the connected X310 and known transmitting sources;
native Windows radio operation and physical direction finding remain unverified.

On 29 October the operator confirmed that the X310 is disconnected and requested
software completion first. No further radio test was attempted. The source and
packaged Windows operator guides were synchronized, and the packaging script now
copies that guide even when the distribution folder starts empty.
Final package auditing also found seven helper executables retained from the
earlier distribution. The packager now refreshes all 17 shipped executables and
rejects any unmatched executable. Final software handoff checks and the fresh
18/18 Linux test log are in `../software_handoff_20261029/`.
