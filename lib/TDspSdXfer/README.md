# TDspSdXfer

Host → device SD file transfer for Teensy 4.x over any byte `Stream` (USB CDC in
practice). `SdWriteReceiver` receives a file **pushed from the host** straight
onto the SD card at full USB speed — no base64 overhead, no MTP, and **no
reflash**: the running firmware keeps serving the card while a host tool drops a
file and you load it in the same session.

It is the write-direction complement of this project's existing device → host
`@READ`/`streamFile` catalog primitive (see the mix-kit's
`CATALOG_TRANSPORT.md`). Together they give a symmetric, transport-agnostic file
channel that works over USB CDC *or* the ESP32/BLE relay.

## Wire protocol

```
Host -> Dev:  @WB=<id>\x1f<path>\x1f<bytes>[\x1f<crc32hex>]\n   ; begin
Dev  -> Host: @WOK=<id>\x1f<bytes>            ; file opened, host may stream now
              @WERR=<id>\x1f<reason>          ; parse / mkdir / open failure
Host -> Dev:  <bytes> raw octets, back-to-back ; NOT line-framed
Dev  -> Host: @WE=<id>\x1f<written>[\x1f<crc32hex>]   ; success
              @WERR=<id>\x1f<reason>                  ; short write / crc / timeout
```

- Fields split on `0x1f` (US), same as `@READ`.
- `<bytes>` is the exact payload length. The receiver reads precisely that many
  raw bytes, then returns to normal line mode — anything the host pipelines after
  the payload (e.g. a following `@` command) is left for your line assembler.
- The parent directories of `<path>` are created (`mkdir -p`).
- An existing file is **overwritten** (removed, then recreated) — not appended.
- CRC32 is standard IEEE-802.3 / zlib (reflected, init `0xFFFFFFFF`, xorout
  `0xFFFFFFFF`, poly `0xEDB88320`). Omit the crc field to skip integrity checking.
- A stalled transfer (no bytes for `setTimeout()` ms, default 5 s) is aborted and
  the partial file removed.

## Integration

```cpp
#include <SD.h>
#include <TDspSdXfer.h>

tdsp::SdWriteReceiver g_rx(SD);

// in your '@'-command dispatch:
if (!strncmp(line, "@WB=", 4)) { g_rx.begin(line + 4, reply); return true; }

void loop() {
    // While a write is in flight, route incoming bytes to the card BEFORE the
    // line assembler sees them. pump() never over-reads past the payload.
    if (g_rx.receiving()) g_rx.pump(Serial, Serial);

    while (Serial.available() && !g_rx.receiving()) {
        /* ...normal '@'-line assembly, dispatching @WB= to g_rx.begin()... */
    }

    g_rx.tick(Serial, millis());   // stall watchdog
}
```

`pump()` is the easy path; if you already buffer bytes yourself, call
`feed(data, n, reply)` directly and honour its return value (bytes consumed).

## Notes / caveats

- **Single-threaded blocking.** A transfer runs on the main loop; a multi-MB file
  holds the loop for the transfer's duration, which will briefly glitch audio in
  a synth build. Fine for authoring. Chunk across loop iterations (feed a bounded
  slice per `loop()`) if you need glitch-free concurrent playback.
- **USB CDC only for speed.** Over the ESP32/BLE relay the link is a 115200-baud
  UART; push over direct USB CDC for real throughput.
- Verify a round trip with `SdWriteReceiver::fileCrc32(SD, path, crc, bytes)` and
  compare against the host's CRC of the source file.
