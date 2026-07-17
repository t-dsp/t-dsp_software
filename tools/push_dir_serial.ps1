<#
.SYNOPSIS
  Bulk-push a whole folder onto the Teensy SD card over USB CDC using the
  TDspSdXfer '@WB' protocol - opens the COM port ONCE and streams every file
  back-to-back. The batch companion to push_file_serial.ps1 (per PLAN §4.5).

.DESCRIPTION
  Walks -SrcDir recursively; each local file lands at
  <SdRoot>/<path-relative-to-SrcDir> on the card. Per file it sends
  '@WB=<id>\x1f<path>\x1f<bytes>\x1f<crc32>', waits for '@WOK', streams the raw
  bytes, waits for '@WE', and compares the device-computed CRC32 to the local
  one (integrity without an extra @CRC round-trip).

  RESUMABLE: every successful push is appended to a log (default
  <SrcDir>\.pushed.log). Re-running skips files already logged, so a dropped
  COM port or Ctrl-C just resumes. Delete the log to force a full re-push.

  Failures are retried once, then recorded and skipped so one bad file doesn't
  abort a 10k-file run. A final summary lists pushed / skipped / failed.

.EXAMPLE
  ./push_dir_serial.ps1 -SrcDir c:/tmp/t-dsp-dexed/dexed -SdRoot /dexed -Port COM4

.EXAMPLE
  ./push_dir_serial.ps1 -SrcDir c:/tmp/t-dsp-dexed/dexed -SdRoot /dexed -Port COM4 -Limit 50

.NOTES
  Windows PowerShell 5.1. The Teensy does NOT reset on port open. Each write
  briefly blocks the firmware's single-threaded loop, so audio will stutter
  during a large run - expected for a bulk authoring load. Close any serial
  monitor first (it holds the port).
#>
param(
    [Parameter(Mandatory = $true)][string]$SrcDir,
    [Parameter(Mandatory = $true)][string]$SdRoot,      # e.g. /dexed
    [string]$Port = "COM4",
    [int]$Baud = 115200,
    [string]$Filter = "*.syx",
    [int]$Limit = 0,                                     # 0 = all
    [string]$Log,                                        # default <SrcDir>\.pushed.log
    [switch]$Fresh                                       # ignore + rewrite the log
)

$ErrorActionPreference = "Stop"

# CRC32 (IEEE-802.3 / zlib) - same helper as push_file_serial.ps1.
if (-not ([System.Management.Automation.PSTypeName]'TDspCrc32').Type) {
    Add-Type -TypeDefinition @"
public static class TDspCrc32 {
    static readonly uint[] T = new uint[256];
    static TDspCrc32() {
        for (uint i = 0; i < 256; i++) {
            uint c = i;
            for (int k = 0; k < 8; k++) c = ((c & 1) != 0) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            T[i] = c;
        }
    }
    public static uint Compute(byte[] data) {
        uint crc = 0xFFFFFFFFu;
        for (int i = 0; i < data.Length; i++) crc = T[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
        return crc ^ 0xFFFFFFFFu;
    }
}
"@
}

function Wait-Line {
    param([System.IO.Ports.SerialPort]$Sp, [string[]]$Prefixes, [int]$TimeoutMs)
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        try { $l = $Sp.ReadLine() } catch { continue }
        if (-not $l) { continue }
        $l = $l.TrimEnd("`r", "`n")
        foreach ($p in $Prefixes) { if ($l.StartsWith($p)) { return $l } }
    }
    return $null
}

# Push one file over an already-open port. Returns $true on success.
function Push-One {
    param([System.IO.Ports.SerialPort]$Sp, [string]$LocalPath, [string]$DevPath, [int]$Id)
    $bytes  = [System.IO.File]::ReadAllBytes($LocalPath)
    $len    = $bytes.Length
    $crcHex = ("{0:x8}" -f [TDspCrc32]::Compute($bytes))
    $us     = [char]0x1f

    $Sp.DiscardInBuffer()                                   # drop accumulated status spam
    $Sp.WriteLine("@WB=$Id$us$DevPath$us$len$us$crcHex")
    $ok = Wait-Line $Sp @("@WOK=$Id", "@WERR=$Id") 5000
    if (-not $ok -or $ok.StartsWith("@WERR")) { return $false }

    $off = 0
    while ($off -lt $len) {
        $c = [Math]::Min(4096, $len - $off)
        $Sp.Write($bytes, $off, $c)
        $off += $c
    }

    $res = Wait-Line $Sp @("@WE=$Id", "@WERR=$Id") 30000
    if (-not $res -or $res.StartsWith("@WERR")) { return $false }
    $devCrc = $res.Split($us)[2]
    if ($devCrc -and ($devCrc -ne $crcHex)) { return $false }
    return $true
}

if (-not (Test-Path -LiteralPath $SrcDir)) { throw "SrcDir not found: $SrcDir" }
$SrcDir = (Resolve-Path -LiteralPath $SrcDir).Path
$SdRoot = "/" + $SdRoot.Trim("/")
if (-not $Log) { $Log = Join-Path $SrcDir ".pushed.log" }

# Resume set.
$done = @{}
if ($Fresh) { Remove-Item -LiteralPath $Log -ErrorAction SilentlyContinue }
elseif (Test-Path -LiteralPath $Log) {
    Get-Content -LiteralPath $Log | ForEach-Object { if ($_) { $done[$_] = $true } }
    Write-Host ("Resume: {0} file(s) already pushed (from {1})" -f $done.Count, $Log)
}

$files = Get-ChildItem -LiteralPath $SrcDir -Recurse -File -Filter $Filter
$total = $files.Count
Write-Host ("Found : {0} '{1}' files under {2}" -f $total, $Filter, $SrcDir)

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
$sp.NewLine      = "`n"
$sp.ReadTimeout  = 1000
$sp.WriteTimeout = 5000
$sp.DtrEnable    = $true
$sp.Open()

$pushed = 0; $skipped = 0; $failed = 0; $id = 0; $n = 0
$failures = New-Object System.Collections.Generic.List[string]
$logw = [System.IO.StreamWriter]::new($Log, $true)   # append
$t0 = Get-Date
try {
    Start-Sleep -Milliseconds 200
    $sp.DiscardInBuffer()
    foreach ($f in $files) {
        if ($Limit -gt 0 -and $n -ge $Limit) { break }
        $n++
        $rel = $f.FullName.Substring($SrcDir.Length).TrimStart('\','/') -replace '\\','/'
        $dev = "$SdRoot/$rel"
        if ($done.ContainsKey($dev)) { $skipped++; continue }

        $id = ($id % 250) + 1
        $ok = Push-One $sp $f.FullName $dev $id
        if (-not $ok) { Start-Sleep -Milliseconds 150; $ok = Push-One $sp $f.FullName $dev $id }  # one retry

        if ($ok) {
            $pushed++; $logw.WriteLine($dev); $logw.Flush()
        } else {
            $failed++; $failures.Add($dev)
            Write-Host ("  FAIL {0}" -f $dev) -ForegroundColor Yellow
        }

        if (($pushed + $skipped) % 200 -eq 0 -or $n -eq $total) {
            $secs = ((Get-Date) - $t0).TotalSeconds
            $rate = if ($secs -gt 0) { [Math]::Round($pushed / $secs, 1) } else { 0 }
            $remain = $total - $n
            $eta = if ($rate -gt 0) { [TimeSpan]::FromSeconds([Math]::Round($remain / $rate)) } else { [TimeSpan]::Zero }
            Write-Host ("  {0}/{1}  pushed={2} skip={3} fail={4}  {5} f/s  ETA {6:hh\:mm\:ss}" -f `
                $n, $total, $pushed, $skipped, $failed, $rate, $eta)
        }
    }
}
finally {
    $logw.Close()
    if ($sp.IsOpen) { $sp.Close() }
    $sp.Dispose()
}

$secs = [Math]::Round(((Get-Date) - $t0).TotalSeconds, 1)
Write-Host ""
Write-Host ("Done in {0}s - pushed {1}, skipped {2}, failed {3}" -f $secs, $pushed, $skipped, $failed) -ForegroundColor Green
if ($failures.Count -gt 0) {
    $ff = Join-Path $SrcDir ".failed.log"
    $failures | Set-Content -LiteralPath $ff
    Write-Host ("Failures written to {0} - re-run the same command to retry them." -f $ff) -ForegroundColor Yellow
}
