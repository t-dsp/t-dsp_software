# spike_sd_xfer

Test harness proving **Option C** of [`planning/fast-sd-transfer/PLAN.md`](../../planning/fast-sd-transfer/PLAN.md):
push files onto the Teensy 4.1's `BUILTIN_SDCARD` from a host over USB CDC —
fast, no MTP, no reflash — via the [`TDspSdXfer`](../../lib/TDspSdXfer) library.

## What it is

A minimal USB-Serial firmware: SD init + the `@`-command ingest loop wired to
`tdsp::SdWriteReceiver`, plus a few verify/poke commands. No audio, no ESP32 —
just the transport under test.

Commands (`@`-prefixed, `\n`-terminated, fields split by `0x1f`):

| Command | Effect |
|---|---|
| `@WB=<id>\x1f<path>\x1f<bytes>[\x1f<crc32hex>]` | begin a write; raw payload follows |
| `@CRC=<path>` | → `@CRCR=<path>\x1f<crc32hex>\x1f<bytes>` |
| `@LS=<dir>` | list a directory (`@LSE=` per entry, `@LSEND`) |
| `@RM=<path>` | delete a file |
| `@PING` | → `@PONG` |

## Build & flash

```sh
# pio path per memory (not on PATH):
pio run -d projects/spike_sd_xfer -t upload
```

Close any serial monitor first; if the COM port is busy, press the **PROGRAM**
button while uploading.

## Test it

From the repo root, with the board on (default `COM4`):

```powershell
# write a file and verify the device-side CRC matches the source
./tools/push_file_serial.ps1 -File .\some.syx -SdPath /dexed/user/some.syx -Port COM4 -Verify
```

Expected:

```
Local : .\some.syx  (4096 bytes, crc32=abcd1234)
Wrote : 4096 in 0.02s (200 KB/s)  crc OK (abcd1234)
Verify: OK  stored crc=abcd1234  bytes=4096
Done.
```

The `-Verify` step is an *independent* check: the device re-reads the stored file
and re-checksums it, so a green result proves the bytes on the card are correct,
not just that the transfer framing agreed.

## Manual poking

Any serial terminal works (115200). Note `@WB` payloads are raw binary, so
`@WB` is best driven by the script; but `@PING`, `@LS=/`, `@CRC=/file`, `@RM=`
are all typeable by hand.

## Next

Once throughput/verify look good here, fold `@WB` + the receiver into the
mix-kit's `handleControlLine` (see the PLAN's §4.4) so instruments can be dropped
and loaded live. Keep this spike as the isolated regression harness.
