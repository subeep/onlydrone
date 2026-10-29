@echo off
cd /d "%~dp0"
droneid_ui.exe --replay "example\mini2_wideband.cf32" --format cf32 --rate 50000000 --auto-center
