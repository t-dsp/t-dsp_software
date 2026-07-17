<#
.SYNOPSIS
  Push a file onto the Teensy SD card over USB CDC using the TDspSdXfer '@WB'
  protocol (Option C of planning/fast-sd-transfer/PLAN.md). Fast, no MTP, no
  reflash. Pairs with projects/spike_sd_xfer (and, later, the mix-kit firmware).

.DESCRIPTION
  Sends '@WB=<id>\x1f<path>\x1f<bytes>\x1f<crc32>', waits for '@WOK', streams the
  raw file bytes, then waits for '@WE' and checks the returned CRC32. With
  -Verify it also asks the device to re-checksum the stored file (@CRC).

.EXAMPLE
  ./push_file_serial.ps1 -File .\voice.syx -SdPath /dexed/user/voice.syx -Port COM4 -Verify

.NOTES
  Windows PowerShell 5.1 compatible. The Teensy does NOT reset when the port is
  opened, so this is safe to run against a live board.
#>
param(
    [Parameter(Mandatory = $true)][string]$File,
    [Parameter(Mandatory = $true)][string]$SdPath,
    [string]$Port = "COM4",
    [int]$Baud = 115200,
    [switch]$Verify
)

$ErrorActionPreference = "Stop"

# CRC32 (IEEE-802.3 / zlib) via a tiny compiled helper — PowerShell's own
# bitwise operators lack clean unsigned-32-bit semantics (sign-bit overflow).
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

function Get-Crc32 {
    param([byte[]]$Bytes)
    return [TDspCrc32]::Compute($Bytes)
}

# Read '\n'-terminated device lines until one starts with a wanted prefix.
function Wait-Line {
    param([System.IO.Ports.SerialPort]$Sp, [string[]]$Prefixes, [int]$TimeoutMs)
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        try { $l = $Sp.ReadLine() } catch { continue }   # timeout -> retry until deadline
        if (-not $l) { continue }
        $l = $l.TrimEnd("`r", "`n")
        Write-Verbose "<< $l"
        foreach ($p in $Prefixes) { if ($l.StartsWith($p)) { return $l } }
    }
    return $null
}

if (-not (Test-Path -LiteralPath $File)) { throw "File not found: $File" }
$bytes  = [System.IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $File))
$len    = $bytes.Length
$crc    = Get-Crc32 $bytes
$crcHex = ("{0:x8}" -f $crc)
$us     = [char]0x1f
$id     = 1

Write-Host ("Local : {0}  ({1} bytes, crc32={2})" -f $File, $len, $crcHex)

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
$sp.NewLine      = "`n"
$sp.ReadTimeout  = 1000
$sp.WriteTimeout = 5000
$sp.DtrEnable    = $true
$sp.Open()
try {
    Start-Sleep -Milliseconds 200
    $sp.DiscardInBuffer()

    # 1) begin frame
    $begin = "@WB=$id$us$SdPath$us$len$us$crcHex"
    $sp.WriteLine($begin)
    $ok = Wait-Line $sp @("@WOK=$id", "@WERR=$id") 5000
    if (-not $ok)                { throw "no response to @WB (device silent)" }
    if ($ok.StartsWith("@WERR")) { throw "device rejected write: $ok" }

    # 2) raw payload, chunked
    $t0 = Get-Date
    $chunk = 4096; $off = 0
    while ($off -lt $len) {
        $c = [Math]::Min($chunk, $len - $off)
        $sp.Write($bytes, $off, $c)
        $off += $c
    }

    # 3) completion
    $res = Wait-Line $sp @("@WE=$id", "@WERR=$id") 30000
    $t1  = Get-Date
    if (-not $res)                { throw "no completion frame (@WE/@WERR) from device" }
    if ($res.StartsWith("@WERR")) { throw "device write failed: $res" }

    $secs = ($t1 - $t0).TotalSeconds
    $kbps = if ($secs -gt 0) { [Math]::Round($len / 1024 / $secs, 1) } else { 0 }
    $fields = $res.Split($us)
    $devCrc = if ($fields.Count -ge 3) { $fields[2] } else { "" }

    if ($devCrc -and ($devCrc -ne $crcHex)) {
        throw "CRC MISMATCH: local=$crcHex device=$devCrc"
    }
    Write-Host ("Wrote : {0} in {1}s ({2} KB/s)  crc {3}" -f $len, [Math]::Round($secs, 2), $kbps, `
        ($(if ($devCrc) { "OK ($devCrc)" } else { "not checked" })))

    # 4) optional independent round-trip verify (device re-reads the stored file)
    if ($Verify) {
        $sp.WriteLine("@CRC=$SdPath")
        $vr = Wait-Line $sp @("@CRCR=$SdPath", "@CRCERR=$SdPath") 10000
        if (-not $vr)                    { throw "no @CRC response" }
        if ($vr.StartsWith("@CRCERR"))   { throw "device could not checksum $SdPath" }
        $vf = $vr.Split($us)
        if ($vf.Count -lt 3)             { throw "malformed @CRCR: $vr" }
        if ($vf[1] -ne $crcHex)          { throw "VERIFY MISMATCH: local=$crcHex stored=$($vf[1])" }
        Write-Host ("Verify: OK  stored crc={0}  bytes={1}" -f $vf[1], $vf[2])
    }

    Write-Host "Done." -ForegroundColor Green
}
finally {
    if ($sp.IsOpen) { $sp.Close() }
    $sp.Dispose()
}
