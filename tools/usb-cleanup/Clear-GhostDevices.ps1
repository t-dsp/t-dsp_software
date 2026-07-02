<#
.SYNOPSIS
  List and remove "ghost" devices — hardware that is no longer connected but is
  still enumerated in Windows Device Manager (e.g. the pile of phantom COMx
  ports left behind after repeatedly reconnecting a Teensy / ESP32 / CP210x).

.DESCRIPTION
  Ghost devices are those whose driver node still exists but whose hardware is
  not currently Present. They hog COM-port numbers and can occasionally confuse
  re-enumeration. This script finds them (Get-PnpDevice where -not Present) and
  removes each with `pnputil /remove-device` (built into Windows 10/11 — no
  download). Removal needs Administrator rights; the script self-elevates.

  SAFE BY DEFAULT: only targets the "Ports" class (COMx / LPT) ghosts. Widen with
  -Include, -AllClasses, or narrow with -Match.

.PARAMETER List
  Only list matching ghost devices; do not remove anything.

.PARAMETER Include
  Extra device classes to target in addition to "Ports" (e.g. USB, HIDClass).

.PARAMETER AllClasses
  Target ghosts of every class (overrides -Include / the Ports default).

.PARAMETER Match
  Regex to filter by InstanceId — e.g. '16C0' (Teensy), '10C4' (CP210x),
  'VID_16C0|VID_10C4'.

.PARAMETER Force
  Remove without the per-device confirmation prompt.

.EXAMPLE
  .\Clear-GhostDevices.ps1 -List
  Show ghost COM ports without touching anything.

.EXAMPLE
  .\Clear-GhostDevices.ps1 -Match 'VID_16C0|VID_10C4' -Force
  Remove only ghost Teensy / CP210x devices, no prompts.

.EXAMPLE
  .\Clear-GhostDevices.ps1 -AllClasses -List
  Audit every disconnected device on the machine.
#>
[CmdletBinding(SupportsShouldProcess, ConfirmImpact = 'High')]
param(
  [switch]$List,
  [string[]]$Include = @(),
  [switch]$AllClasses,
  [string]$Match,
  [switch]$Force
)

$ErrorActionPreference = 'Stop'

function Test-Admin {
  $id = [Security.Principal.WindowsIdentity]::GetCurrent()
  (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
    [Security.Principal.WindowsBuiltinRole]::Administrator)
}

# Re-launch elevated for the actual removal (listing is fine unprivileged).
if (-not $List -and -not (Test-Admin)) {
  Write-Host "Removing devices needs Administrator — relaunching elevated..." -ForegroundColor Yellow
  $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
  if ($Include)    { $argList += @('-Include', ($Include -join ',')) }
  if ($AllClasses) { $argList += '-AllClasses' }
  if ($Match)      { $argList += @('-Match', "`"$Match`"") }
  if ($Force)      { $argList += '-Force' }
  Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $argList
  return
}

# Collect ghosts: enumerated but not currently present.
$ghosts = Get-PnpDevice | Where-Object { -not $_.Present }

if (-not $AllClasses) {
  $classes = @('Ports') + $Include | Select-Object -Unique
  $ghosts = $ghosts | Where-Object { $_.Class -in $classes }
}
if ($Match) {
  $ghosts = $ghosts | Where-Object { $_.InstanceId -match $Match }
}

$ghosts = @($ghosts | Sort-Object Class, FriendlyName)

if ($ghosts.Count -eq 0) {
  Write-Host "No matching ghost devices found. Nothing to do." -ForegroundColor Green
  return
}

Write-Host "`nGhost (disconnected) devices matched: $($ghosts.Count)`n" -ForegroundColor Cyan
$ghosts | Select-Object Class, FriendlyName, InstanceId | Format-Table -AutoSize

if ($List) {
  Write-Host "`n(-List mode: nothing removed.)" -ForegroundColor Yellow
  return
}

if (-not $Force) {
  $ans = Read-Host "`nRemove these $($ghosts.Count) device(s)? [y/N]"
  if ($ans -notmatch '^[Yy]') { Write-Host 'Aborted.'; return }
}

$removed = 0; $failed = 0
foreach ($g in $ghosts) {
  $label = if ($g.FriendlyName) { $g.FriendlyName } else { $g.InstanceId }
  if ($PSCmdlet.ShouldProcess($label, 'Remove ghost device')) {
    try {
      # pnputil is built in on Win10/11. /remove-device takes the instance ID.
      $out = & pnputil.exe /remove-device "$($g.InstanceId)" 2>&1
      if ($LASTEXITCODE -eq 0) {
        Write-Host ("  removed: {0}" -f $label) -ForegroundColor Green
        $removed++
      } else {
        # Fallback to the PnpDevice cmdlet if available.
        Remove-PnpDevice -InstanceId $g.InstanceId -Confirm:$false -ErrorAction Stop
        Write-Host ("  removed (cmdlet): {0}" -f $label) -ForegroundColor Green
        $removed++
      }
    } catch {
      Write-Host ("  FAILED: {0} -> {1}" -f $label, $_.Exception.Message) -ForegroundColor Red
      $failed++
    }
  }
}

Write-Host "`nDone. Removed $removed, failed $failed." -ForegroundColor Cyan
if ($failed) { Write-Host "Tip: some nodes need a Device Manager 'Scan for hardware changes' or a reboot to fully clear." -ForegroundColor Yellow }
