@echo off
rem Quick self-test: replay the bundled Mini 2 excerpt and confirm decoding works.
rem Expect: one CRC-valid packet and one track. This is a reference fixture, not a live drone.
cd /d "%~dp0"
echo.
echo === punjdrone Mini 2 replay self-test ===
echo Running headless decode check (no window)...
echo.
droneid_ui.exe --replay "example\mini2_centered.sc16" --format sc16 --rate 15360000 --check
echo.
echo If you saw 1 CRC packet / 1 track above, the build works on this PC.
echo To use the full GUI, just double-click droneid_ui.exe .
echo.
pause
