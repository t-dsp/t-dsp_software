@echo off
REM One-click: remove ALL ghost Teensy (VID_16C0) and CP210x (VID_10C4) devices
REM across every class (COM ports + composite/HID/MIDI/WPD children). Self-elevates.
REM Unplug the Teensy and ESP32 first so nothing live gets removed.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Clear-GhostDevices.ps1" -Match "VID_16C0|VID_10C4" -AllClasses -Force
pause
