@echo off
REM Double-click launcher: list + remove ghost COM-port devices (elevates as needed).
REM For other options run Clear-GhostDevices.ps1 directly (see README.md).
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Clear-GhostDevices.ps1" %*
pause
