<#
.SYNOPSIS
Copy a sample folder onto the T-DSP Teensy's SD card, auto-detecting
whether the card is mounted as a drive letter (card reader path) or
exposed via MTP (companion firmware path).

.DESCRIPTION
Searches first for a mounted volume whose label matches the given
pattern (default "T-DSP*" or "*Teensy*"). If found, does a regular
filesystem copy with Robocopy.

If no such volume exists, falls back to MTP: enumerates portable
devices via Shell.Application, finds one whose name matches the
pattern, navigates into its first storage object, removes any
pre-existing folder of the same name as the source's top-level
directory, and CopyHere's the new content. CopyHere is async on MTP,
so the script polls for completion.

Windows-only (the MTP path uses Shell.Application COM, which has no
non-Windows analog).

.PARAMETER SourcePath
Local folder to copy. Its TOP-LEVEL folder name becomes the
destination folder name on the card root. Example: passing
"C:/tmp/t-dsp-samples/samples" lands as "/samples" on the card.

.PARAMETER NamePattern
Glob-style pattern matched against drive labels and MTP device
names. Default matches "T-DSP*" and "*Teensy*".

.EXAMPLE
.\push_to_teensy.ps1 -SourcePath C:\tmp\t-dsp-samples\samples
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourcePath,

    [string]$NamePattern = "T-DSP*|*Teensy*"
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $SourcePath -PathType Container)) {
    Write-Error "Source path does not exist or isn't a folder: $SourcePath"
    exit 1
}
$sourceFull = (Resolve-Path $SourcePath).Path
$sourceName = (Split-Path $sourceFull -Leaf)

function Test-NameMatch {
    param([string]$Name, [string]$Pattern)
    foreach ($p in $Pattern -split '\|') {
        if ($Name -like $p) { return $true }
    }
    return $false
}

# ---------- Path 1: mounted drive letter ----------------------------------
$drives = Get-Volume -ErrorAction SilentlyContinue |
    Where-Object { $_.DriveLetter -and $_.FileSystemLabel } |
    Where-Object { Test-NameMatch $_.FileSystemLabel $NamePattern }

if ($drives) {
    $vol = $drives | Select-Object -First 1
    $driveRoot = "$($vol.DriveLetter):\"
    Write-Host "Found mounted volume: $($vol.FileSystemLabel) at $driveRoot"
    $dest = Join-Path $driveRoot $sourceName
    Write-Host "Copying $sourceFull -> $dest"
    # Robocopy is more reliable than Copy-Item for large recursive trees;
    # /MIR mirrors the source (deletes orphaned files in dest).
    & robocopy.exe $sourceFull $dest /E /NFL /NDL /NJH /NJS /NP | Out-Null
    # Robocopy exits 0..7 on success; >=8 on failure.
    if ($LASTEXITCODE -ge 8) {
        Write-Error "Robocopy failed with exit code $LASTEXITCODE"
        exit 1
    }
    Write-Host "Done (drive-letter path)."
    exit 0
}

Write-Host "No mounted volume matched '$NamePattern'; trying MTP."

# ---------- Path 2: MTP device --------------------------------------------
$shell = New-Object -ComObject Shell.Application

# 0x11 is the CSIDL for "My Computer". It contains drives + portable devices.
$myComputer = $shell.NameSpace(0x11)
if (-not $myComputer) {
    Write-Error "Could not open the 'My Computer' namespace."
    exit 1
}

$device = $null
foreach ($item in $myComputer.Items()) {
    if ($item.IsFolder -and (Test-NameMatch $item.Name $NamePattern)) {
        $device = $item
        break
    }
}

if (-not $device) {
    Write-Error @"
No mounted drive AND no MTP device matched '$NamePattern'.

Things to check:
  * Is the Teensy plugged in and running the t-dsp_mtp_disk firmware?
    (USB descriptor must include MTP — the audio firmware does NOT.)
  * Open File Explorer, look under 'This PC'. Do you see a drive or
    portable device with one of these names: T-DSP SD, Teensy?
  * Volume label: when formatting an SD card to use with a card reader,
    label it 'T-DSP SD' so this script picks it up automatically.
"@
    exit 1
}

Write-Host "Found MTP device: $($device.Name)"
$deviceFolder = $device.GetFolder
if (-not $deviceFolder) {
    Write-Error "Could not access MTP device folder."
    exit 1
}

# Navigate into the first storage object (most MTP devices have exactly one).
$storage = $null
foreach ($child in $deviceFolder.Items()) {
    if ($child.IsFolder) { $storage = $child; break }
}
if (-not $storage) {
    Write-Error "Device exposes no storage."
    exit 1
}
Write-Host "Using storage: $($storage.Name)"
$root = $storage.GetFolder

# NOTE: We intentionally do NOT delete an existing folder of the same
# name on the device first. InvokeVerb("delete") on MTP folders can hang
# indefinitely (it sometimes shows a confirmation dialog the script
# can't dismiss; sometimes it just blocks silently). CopyHere with
# FOF_NOCONFIRMATION merges into the existing folder and overwrites
# same-named files silently, which is good enough — leftover unrelated
# files (e.g., old test tones) are harmless to the slot's bank scanner
# unless they happen to share a filename with a new sample.
#
# If you really need a clean replacement, delete the folder via
# File Explorer on the host before running this script.

# Helper: recursively count files in an MTP folder.
function Get-MtpFileCount {
    param($Folder)
    if (-not $Folder) { return 0 }
    $count = 0
    $stack = New-Object System.Collections.Stack
    $stack.Push($Folder)
    while ($stack.Count -gt 0) {
        $f = $stack.Pop()
        foreach ($e in $f.Items()) {
            if ($e.IsFolder) { $stack.Push($e.GetFolder) }
            else             { $count++ }
        }
    }
    return $count
}

# Snapshot the existing file count BEFORE we start CopyHere so the
# completion target is (initial + new), not just (new). Without this,
# merging into a folder that already has files makes the count check
# pass immediately and the script declares "Done" before any data has
# transferred.
$existingFolder = $null
foreach ($entry in $root.Items()) {
    if ($entry.IsFolder -and $entry.Name -eq $sourceName) { $existingFolder = $entry; break }
}
$initialCount = if ($existingFolder) { Get-MtpFileCount $existingFolder.GetFolder } else { 0 }

$expectedNew = (Get-ChildItem -Path $sourceFull -File -Recurse).Count
$targetCount = $initialCount + $expectedNew
if ($initialCount -gt 0) {
    Write-Host ("Existing files in /{0}: {1}; merging in {2} new (target {3})" -f $sourceName, $initialCount, $expectedNew, $targetCount)
}

# CopyHere with FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI.
$FOF_FLAGS = 0x14
Write-Host "Copying $sourceFull -> /$sourceName via MTP (this can take a while)..."
$root.CopyHere($shell.NameSpace($sourceFull), $FOF_FLAGS)

# Poll. Wait for count to grow up to the target. Note: when files in
# the source already exist with the same name in the destination, MTP
# may overwrite-in-place rather than adding new entries, so the count
# can stop short of $targetCount. We accept "stable for 5 seconds at a
# count >= initial + 1" as completion-ish, capped by deadline.
$deadline = (Get-Date).AddMinutes(30)
$lastCount = -1
$stableSince = $null
while ((Get-Date) -lt $deadline) {
    $remoteFolder = $null
    foreach ($entry in $root.Items()) {
        if ($entry.IsFolder -and $entry.Name -eq $sourceName) { $remoteFolder = $entry; break }
    }
    if ($remoteFolder) {
        $count = Get-MtpFileCount $remoteFolder.GetFolder
        if ($count -ne $lastCount) {
            Write-Host "  ...$count / $targetCount files"
            $lastCount = $count
            $stableSince = Get-Date
        }
        if ($count -ge $targetCount) {
            Write-Host "Done (MTP path)."
            exit 0
        }
        # Stability fallback: count stopped growing for 8 seconds AND we've
        # at least seen one new file land. Lets pure-overwrite transfers
        # complete cleanly.
        if ($count -gt $initialCount -and $stableSince -and `
            ((Get-Date) - $stableSince).TotalSeconds -ge 8) {
            Write-Host "Done (MTP path; count stable at $count after grow from $initialCount)."
            exit 0
        }
    }
    Start-Sleep -Milliseconds 500
}

Write-Error "MTP copy did not complete within timeout. Files transferred so far: $lastCount / $targetCount"
exit 1
