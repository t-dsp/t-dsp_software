# usb-cleanup

Remove **ghost devices** — hardware that's no longer plugged in but is still
enumerated in Windows Device Manager. Repeatedly reconnecting a Teensy / ESP32 /
CP210x (as we do during ESP32 bring-up) leaves behind a pile of phantom `COMx`
ports that hog port numbers and can occasionally confuse re-enumeration.

## Native script (no download) — `Clear-GhostDevices.ps1`

Built on `pnputil` (ships with Windows 10/11) + `Get-PnpDevice`. Self-elevates
for removal. **Safe by default: only touches the `Ports` class (COMx) ghosts.**

```powershell
# List ghost COM ports (no changes)
.\Clear-GhostDevices.ps1 -List

# Remove ghost COM ports (prompts; self-elevates)
.\Clear-GhostDevices.ps1

# Only ghost Teensy / CP210x devices, no prompt
.\Clear-GhostDevices.ps1 -Match 'VID_16C0|VID_10C4' -Force

# Audit every disconnected device on the machine
.\Clear-GhostDevices.ps1 -AllClasses -List
```

Or just double-click **`clear-ghost-com-ports.cmd`**.

Relevant VIDs for this project: **`VID_16C0`** = Teensy/PJRC, **`VID_10C4`** =
Silicon Labs CP210x (ESP32-DevKitC USB-serial).

> Tip: if a node lingers after removal, open Device Manager →
> *Action → Scan for hardware changes*, or reboot.

## Alternative: Uwe Sieber's Device Cleanup Tool (GUI, external download)

A tiny (~40 KB) portable, reputable tool that lists only non-present ("ghost")
devices sorted by name/class/last-used and bulk-removes them:
<https://www.uwe-sieber.de/misc_tools_e.html> (v1.2.1). Good for interactive
one-off cleanup. It also ships a scriptable CLI, **`DeviceCleanupCmd.exe`**.

We don't vendor that binary here (external third-party `.exe` = a deliberate
call). If you drop `DeviceCleanupCmd.exe` into this folder, it can be scripted,
e.g. remove all ghosts of a class:

```cmd
DeviceCleanupCmd.exe Ports
```
