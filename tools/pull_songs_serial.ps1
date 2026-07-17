<#
.SYNOPSIS
  Pull the SD-resident MIDI songs OFF the Teensy over USB CDC and save them into
  the repo. The read-direction complement of push_dir_serial.ps1.

.DESCRIPTION
  Uses the mix-kit firmware's catalog + generic file-read primitives (no reflash,
  no MTP):
    1. '@GETCAT'  -> device re-scans the card and replies '@SONGS=n1|n2|...'
                    (display names = '/songs/<name>.mid' minus the extension).
    2. For each name, '@READ=/songs/<name>.mid' streams the file back as base64
       frames ('@FB' begin / '@FD' data / '@FE' end), which we reassemble and
       decode to the exact original bytes.

  Built-in / baked test songs in the catalog have no SD file; their '@READ'
  returns '@FERR ... not found' and they are silently skipped. Files present at
  the card ROOT ('/<name>.mid') are tried as a fallback.

.EXAMPLE
  ./pull_songs_serial.ps1 -Port COM4
  ./pull_songs_serial.ps1 -Port COM4 -OutDir ../projects/spike_esp32_bt_spdif_mix_kit_f32/assets/songs

.NOTES
  Windows PowerShell 5.1. The Teensy does NOT reset when the port is opened, so
  this is safe against a live board. Close any serial monitor first (it holds the
  port). Teensy USB CDC ignores the baud rate; 115200 is nominal.
#>
param(
    [string]$Port = "COM4",
    [int]$Baud = 115200,
    [string]$OutDir = (Join-Path $PSScriptRoot "..\projects\spike_esp32_bt_spdif_mix_kit_f32\assets\songs"),
    [switch]$Overwrite      # re-pull even if a same-named file already exists locally
)

$ErrorActionPreference = "Stop"
$us = [char]0x1f

# Read '\n'-terminated device lines until one starts with a wanted prefix. Status
# spam (non-'@' log lines) is skipped by the prefix filter.
function Wait-Line {
    param([System.IO.Ports.SerialPort]$Sp, [string[]]$Prefixes, [int]$TimeoutMs)
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        try { $l = $Sp.ReadLine() } catch { continue }   # timeout -> retry until deadline
        if (-not $l) { continue }
        $l = $l.TrimEnd("`r", "`n")
        foreach ($p in $Prefixes) { if ($l.StartsWith($p)) { return $l } }
    }
    return $null
}

# @READ one SD path -> raw bytes, or $null if the device reports it missing.
function Read-SdFile {
    param([System.IO.Ports.SerialPort]$Sp, [string]$Path)
    $Sp.DiscardInBuffer()
    $Sp.WriteLine("@READ=$Path")

    # begin frame: @FB=<id>\x1f<path>\x1f<bytes>   (or @FERR=<id>\x1f<reason>)
    $begin = Wait-Line $Sp @("@FB=", "@FERR=") 5000
    if (-not $begin) { throw "no @FB/@FERR for $Path (device silent)" }
    if ($begin.StartsWith("@FERR")) { return $null }               # not found -> skip
    $bf = $begin.Split($us)
    $id = $bf[0].Substring(4)
    $total = [int]$bf[2]

    # data frames until @FE=<id>\x1f<count>. Store by seq so order can't bite us.
    $chunks = @{}
    for (;;) {
        $l = Wait-Line $Sp @("@FD=$id$us", "@FE=$id$us", "@FERR=$id$us") 10000
        if (-not $l) { throw "timeout mid-transfer for $Path" }
        if ($l.StartsWith("@FERR")) { throw "device error mid-transfer: $l" }
        if ($l.StartsWith("@FE=")) { break }
        $df = $l.Split($us)                                        # @FD=<id> <seq> <b64>
        $chunks[[int]$df[1]] = $df[2]
    }

    $sb = New-Object System.Text.StringBuilder
    foreach ($seq in ($chunks.Keys | Sort-Object)) { [void]$sb.Append($chunks[$seq]) }
    $bytes = [Convert]::FromBase64String($sb.ToString())
    if ($bytes.Length -ne $total) { throw "length mismatch for $Path (got $($bytes.Length), expected $total)" }
    return ,$bytes
}

if (-not (Test-Path -LiteralPath $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path
Write-Host ("Out   : {0}" -f $OutDir)

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
$sp.NewLine      = "`n"
$sp.ReadTimeout  = 1500
$sp.WriteTimeout = 5000
$sp.DtrEnable    = $true
$sp.Open()

$saved = 0; $skipped = 0; $notfound = 0
try {
    Start-Sleep -Milliseconds 200
    $sp.DiscardInBuffer()

    # 1) enumerate
    $sp.WriteLine("@GETCAT")
    $songsLine = Wait-Line $sp @("@SONGS=") 8000
    if (-not $songsLine) { throw "no @SONGS reply to @GETCAT (wrong firmware / port?)" }
    $names = $songsLine.Substring(7).Split('|') | Where-Object { $_ }
    Write-Host ("Songs : {0} in catalog" -f $names.Count)

    # 2) pull each SD-resident file
    foreach ($name in $names) {
        $dest = Join-Path $OutDir ("{0}.mid" -f $name)
        if ((Test-Path -LiteralPath $dest) -and -not $Overwrite) {
            Write-Host ("  have {0}.mid (skip; -Overwrite to refresh)" -f $name); $skipped++; continue
        }

        $bytes = $null
        foreach ($p in @("/songs/$name.mid", "/$name.mid")) {
            $bytes = Read-SdFile $sp $p
            if ($bytes) { break }
        }
        if (-not $bytes) { Write-Host ("  --   {0}  (built-in, not on card)" -f $name); $notfound++; continue }

        [System.IO.File]::WriteAllBytes($dest, $bytes)
        Write-Host ("  ok   {0}.mid  ({1} bytes)" -f $name, $bytes.Length) -ForegroundColor Green
        $saved++
    }
}
finally {
    if ($sp.IsOpen) { $sp.Close() }
    $sp.Dispose()
}

Write-Host ""
Write-Host ("Done - saved {0}, skipped {1} (already local), {2} built-in/not-on-card" -f $saved, $skipped, $notfound) -ForegroundColor Cyan
