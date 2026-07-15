# Fast SD Transfer — Speeding Up the "Author → Load" Loop

**Status:** Proposed · **Author:** planning session 2026-07-15 · **Target firmware:** `projects/spike_esp32_bt_spdif_mix_kit_f32`

## 1. The problem

The dev loop today is: **flash `t-dsp_mtp_disk` → drag files over MTP → flash the audio firmware back → load the instrument.** Two things make it slow:

1. **MTP is slow for many small files** — ~2 files/sec, per-file protocol + Windows-shell overhead (not an SDIO limit). A 15 MB library ≈ 30 min. See `tools/push_to_teensy.ps1:84-224` (the `Shell.Application` `CopyHere` fallback path).
2. **It requires two reflashes per iteration.** `t-dsp_mtp_disk`'s `USB_MTPDISK_SERIAL` descriptor is *mutually exclusive* with the audio build (`projects/t-dsp_mtp_disk/platformio.ini:11-13`), so authoring and playing can't happen in one session — you round-trip through the bootloader every time you tweak a patch. `tools/file_mode.sh` automates the round-trip but the round-trip itself is the tax.

Goal: **write instruments/files to the card fast, then load them — ideally without reflashing.**

## 2. What's actually possible (state of the art, mid-2026)

| Approach | Speed | Works *with* audio firmware? | Reflash per iter? | Maturity / risk | Effort |
|---|---|---|---|---|---|
| **A. USB MSC device (TinyUSB / vjmuzik)** | ~9 MB/s W, 7.5 MB/s R | ❌ replaces whole USB stack | Yes (separate mode) | POC, ~3★, unmaintained, no USB-audio class | High |
| **B. Optimize MTP** | marginal | ✅ (already does) | No | Core already latest | Low, low payoff |
| **C. Custom `@WRITE` over USB CDC** ⭐ | ~1–3 MB/s (raw) | ✅ **live, no reflash** | **No** | We own it; reuses `@READ` design | Medium |
| **D. Card reader (Robocopy)** | Full SDIO / USB3 | n/a (card removed) | No (physical) | Already works | Zero |
| **E. ESP32 Wi-Fi upload** | — | — | — | Wi-Fi not enabled; link is 115200 Serial7 | Not viable now |

### Notes per option

- **A — MSC device mode.** vjmuzik proved Adafruit-TinyUSB on Teensy 4.1 can expose `BUILTIN_SDCARD` as a real removable drive at **~9 MB/s write** ([PJRC #64627](https://forum.pjrc.com/index.php?threads/teensy-4-1-adafruit-tinyusb-support.64627/)), and the fork now supports **composite CDC + MSC** ([vjmuzik/Adafruit_TinyUSB_TeensyCore](https://github.com/vjmuzik/Adafruit_TinyUSB_TeensyCore)). But it needs USB Type = **"No USB"** and re-implements every USB class on TinyUSB — and **there is no TinyUSB USB-Audio class for Teensy**, which this firmware depends on. MSC is also inherently a *mode*: while the PC owns the FAT block device, firmware must relinquish the card (raw-block access → mutual corruption otherwise), exactly like the MTP-disk firmware but faster. **Only viable as a drop-in replacement for `t-dsp_mtp_disk` (an authoring firmware with no audio), not in the audio build.**
- **B — MTP.** The vendored core is already the recent PaulStoffregen/KurtE MTP with `USE_DISK_BUFFER` (`lib/teensy_cores/teensy4/MTP_Teensy.cpp:30`, pinned `7659e41…`, 2026-04-11). The slowness is protocol/shell overhead; little headroom. Skip.
- **C — Custom `@WRITE`.** Mirror the existing device→host `@READ` file primitive in the host→device direction over **USB CDC, which streams at full speed** (`streamFile` skips its pacing `delay` on the USB path — `main.cpp:935` `if (&out != &Serial) delay(6);`). This is the only option that lets you **drop a file and load it in the same running session, no reflash.** Full analysis below.
- **D — Card reader.** Already the fast bulk path: a volume labeled `T-DSP*` triggers Robocopy `/MIR` in `push_to_teensy.ps1:59-80`. Keep it as the answer for whole-library loads (the 3,874-cart DX7 tree copied in 63 s this way).
- **E — ESP32.** Wi-Fi is not compiled in (`t-dsp_esp32_bt_receiver` is BTDM-only), and the ESP32↔Teensy link is a hard **115200-baud Serial7** UART with per-frame pacing — ~11.5 KB/s, the slowest transport in the system. Not a basis for uploads.

## 3. Recommendation

**Primary: build Option C (`@WRITE` over USB CDC).** Best ROI — it directly kills the reflash round-trip, reuses the framing/dispatch/web plumbing we already have, and needs no USB-stack surgery.

**Keep Option D (card reader)** as the documented answer for bulk library loads; nothing to build.

**Optional, gated spike: Option A** as a *faster replacement for `t-dsp_mtp_disk`* (authoring-only firmware), pursued **only if** `@WRITE` throughput proves too slow for large assets (e.g. multi-MB SF2 fonts) in Phase 1 verification. Go/no-go below.

Explicitly **not doing:** B (MTP tuning), E (ESP32/Wi-Fi).

---

## 4. Option C — detailed implementation plan

### 4.1 What already exists to build on

- **Command dispatcher** `handleControlLine(const char* line, Print& reply)` — `mix_kit_f32/src/main.cpp:996-1153`. `@`-prefixed lines, `\n`-terminated, fields split by `\x1f` (US).
- **The read primitive** `@READ=<path>` → `streamFile()` (`main.cpp:904-940`) framing `@FB`/`@FD`/`@FE`/`@FERR`, 360 raw bytes → 480 b64 per chunk. Documented in `projects/spike_esp32_bt_spdif_mix_kit_f32/CATALOG_TRANSPORT.md`.
- **Browser plumbing** `web/control.html:437-500` (frame routing + reassembly), `readFile()` at `:474-480`.
- **SD layer** Arduino `SD.h` (wraps SdFat) on `BUILTIN_SDCARD` (SDIO); `SD.open(path, FILE_WRITE)` + `File::write()` available. **No write helper exists yet** — `streamFile` is the structural template to mirror.

### 4.2 Key constraint discovered

The inbound USB-CDC line buffer is **only 160 bytes** (`usbLine[160]`, `main.cpp:1894`). A naive mirror of the 480-char `@READ` chunk **will not fit inbound.** Therefore `@WD` must **not** be a base64 text line. Instead, after a begin frame, put the CDC ingress into a **raw binary receive state** that reads exactly *N* bytes straight to SD, bypassing the line buffer entirely. This is both correct *and* faster (no base64 +33% overhead).

### 4.3 Protocol (`@WRITE`, binary over USB CDC)

```
Host → Dev:  @WB=<id>\x1f<path>\x1f<bytes>[\x1f<crc32hex>]\n   ; begin write
Dev  → Host: @WOK=<id>\x1f<bytes>                              ; opened, ready for raw stream
             @WERR=<id>\x1f<reason>                            ; open/mkdir/space/busy failure
Host → Dev:  <bytes> raw octets, streamed back-to-back         ; NOT line-buffered
Dev  → Host: @WE=<id>\x1f<written>[\x1f<crc32hex>]             ; success (+ crc if requested)
             @WERR=<id>\x1f<reason>                            ; short write / crc mismatch / timeout
```

- Between `@WOK` and completion the firmware is in **raw-receive mode**: `f = SD.open(path, FILE_WRITE)` (truncate), loop `Serial.readBytes(buf, min(chunk, remaining))` into a 4 KB buffer → `f.write(buf, n)`, accumulate CRC32, until `written == bytes`. USB CDC is host-flow-controlled, so no ACK-per-chunk needed.
- **Watchdog:** if no bytes for ~5 s, abort → `@WERR=<id>\x1ftimeout`, close/`f.remove()`, flush RX, return to line mode. Prevents a dropped upload from wedging the console.
- **`mkdir -p`:** split `path` on `/` and `SD.mkdir()` each missing component before opening (helper `sdMkdirp(path)`).
- **Integrity:** optional CRC32 in the begin frame; firmware compares and reports. Cheap insurance for patch files.
- **Reuse `@READ`** unchanged for read-back verification and for loading the instrument afterward.

### 4.4 Firmware work (mix-kit)

1. Add `sdMkdirp()` + `handleWriteBegin()` state machine near `streamFile` (`main.cpp:~904`). Keep it `FLASHMEM` — the pool build is chronically ~1 ITCM block over RAM1 (per prior notes, same fix applied to `handleControlLine`/`sendCatalog`).
2. Add a `g_rxMode` flag; in the USB ingress loop (`main.cpp:1897-1909`) branch to the raw reader while a write is in flight instead of the line assembler.
3. Dispatch `@WB=` in `handleControlLine` (`main.cpp:~1043`, beside `@READ=`).
4. Optional `@RESCAN` to drop cached SD catalogs (Dexed bank cache etc.) so a just-written instrument is visible without reflashing.
5. **Concurrency:** the main loop is single-threaded and already interleaves `MTP.loop()` with audio; a write blocks the loop for the transfer's duration. For multi-MB files that will glitch audio briefly — acceptable for authoring; document it. (If not acceptable, chunk the raw read across loop iterations.)

### 4.5 Host tooling

- **`tools/push_file_serial.ps1 <COMx> <localFile> <sdPath>`** — open COM, send `@WB`, await `@WOK`, write raw bytes, await `@WE`, verify count + CRC. A `push_dir_serial.ps1` wrapper walks a folder. This becomes a third, fast, *no-reflash* path alongside the Robocopy/MTP paths in `push_to_teensy.ps1`.
- **`web/control.html` drag-drop zone** — `writeFile(path, arrayBuffer)`: send `@WB`, then `writer.write(new Uint8Array(buf))` raw over Web Serial; show progress from byte count; resolve on `@WE`. Reuses the existing serial reader/`pendingRead`-style guard (one upload in flight).
- **`serial-bridge.mjs` / cloud relay:** these throttle host→device writes (`TX_GAP_MS=20`, `serial-bridge.mjs:42`) to protect USB-audio isoch. For an upload, either (a) raise/bypass the throttle in a dedicated "upload" mode and accept brief audio contention, or (b) require direct COM (Web Serial / PowerShell) rather than going through the bridge. Recommend (b) for v1 to keep it simple.

### 4.6 "Load after" — already covered

Once written, load with existing commands: `@DXPICK=<relCart>\x1f<voice>`, `@READ=` for catalogs, `@MANIFESTS`. Add `@RESCAN` (4.4 step 4) if a cache needs busting. No new load path required.

### 4.7 Verification (Phase-1 gate)

- Round-trip a known file: `push_file_serial.ps1` up → `@READ=` down → byte-compare + CRC match.
- Measure throughput on a ~6 MB payload (SF2-sized). **Gate:** if effective rate ≥ ~1 MB/s, Option C is sufficient and Option A is shelved. If well below, open the Option-A spike.
- Write while audio plays; confirm the card write and subsequent `@DXPICK` load work and audio recovers.

---

## 5. Option A spike (only if 4.7 gate fails)

Build an **MSC authoring firmware** (`projects/t-dsp_msc_disk`, sibling of `t-dsp_mtp_disk`) using vjmuzik's `Adafruit_TinyUSB_TeensyCore` + `Adafruit_TinyUSB_Arduino`, USB Type "No USB", composite CDC+MSC, exposing `BUILTIN_SDCARD`. **No audio in this build** — it's a swap-in replacement for the MTP-disk mode, just ~4–5× faster. `file_mode.sh` would flash *it* instead of `t-dsp_mtp_disk`.

**Go/no-go before investing:** confirm on a scratch sketch that (1) it enumerates as a drive on Windows, (2) composite CDC still gives us a COM port for `@`-commands, (3) the Teensy Loader upload/reboot still works (or PROGRAM-button flashing is acceptable). If any fail, stay on MTP-disk for the "removed-card-not-available" bulk case and rely on Option C + card reader.

## 6. Risks & open questions

- **Audio glitch during large writes** (single-threaded loop) — mitigate by chunking across loop iterations if needed (4.4 step 5).
- **RAM1 fit** — new `FLASHMEM` code on the already-tight pool build; watch the ITCM line.
- **CRC cost** — CRC32 over multi-MB at write time is cheap on the M7; keep it optional per-file.
- **Bridge vs direct COM** — v1 targets direct COM to sidestep the isoch-protection throttle (4.5).
- **Which builds get `@WRITE`** — start with `teensy41_dexed_pool`/mix-kit; the primitive is build-agnostic and can move into the shared `handleControlLine` for all synth builds later.

## 7. Milestones

1. **M1 — firmware `@WRITE`** (raw-receive state machine, `@WB`/`@WOK`/`@WE`/`@WERR`, `sdMkdirp`, watchdog). Flash + smoke-test via a serial terminal.
2. **M2 — `push_file_serial.ps1`** + round-trip/CRC verification (Phase-1 gate, §4.7). **Decision point on Option A.**
3. **M3 — `control.html` drag-drop** upload UI + progress.
4. **M4 — `@RESCAN` + author→load demo**: drop a `.syx`/instrument, load it live, hear it — zero reflash.
5. **M5 (optional)** — Option A MSC-disk spike if M2 gate failed.
6. **Docs/commit** — update `CATALOG_TRANSPORT.md` with the write direction; note the three transports (serial `@WRITE`, MTP, card reader) and when to use each.
