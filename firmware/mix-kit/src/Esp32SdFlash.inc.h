// Esp32SdFlash.inc.h — program the on-board ESP32 from an image on the Teensy's SD card (opt-in).
//
// Gated behind -D TDSP_ESP32_SDFLASH (platformio.ini), so default builds are unaffected.
//
// Why: the ESP32 is the box's Wi-Fi radio, so "flash the ESP32 over Wi-Fi" can't stream the image
// straight into the ESP32 ROM — the link dies the moment the chip is reset into its bootloader.
// Instead the image is STAGED on the SD card first (over whatever link is up: the ESP32's raw WS
// tunnel + the @WB file-write primitive, or USB), and then the Teensy — which already owns the
// ESP32's EN/IO0 straps and UART0 (lib/TDspProgrammingKit) — plays esptool itself: it resets the
// ESP32 into ROM serial-download mode and speaks the ROM bootloader protocol over Serial7 to write
// the file. Together with FlasherX (@FXUP, Teensy self-update through the ESP32 tunnel) this makes
// BOTH MCUs reflashable with no USB host and no buttons.
//
// Commands (handleControlLine, any link):
//   @ESPUP=<sdpath>[\x1f<hexoffset>]   flash <sdpath> at <hexoffset> (default 0x10000 = the app
//                                     partition; use 1000 / 8000 / e000 for bootloader / partition
//                                     table / boot_app0). Replies @ESPUP_GO first (relayed to the
//                                     WS client before the ESP32 goes away), logs progress on USB,
//                                     then reboots the ESP32 into its app. Blocks (~2.5 min / 1.7 MB
//                                     at 115200; audio is paused, like a FlasherX update).
//   @ESPUP?                            -> @ESPUP_LAST=<result of the last @ESPUP>  (the client
//                                     reconnects after the ESP32 reboots and asks this).
//
// Protocol: Espressif serial bootloader (ROM, no stub) — SLIP frames, SYNC, SPI_ATTACH,
// SPI_SET_PARAMS, FLASH_BEGIN (ROM erases the region up front), FLASH_DATA 0x400-byte blocks with
// the 0xEF XOR checksum, FLASH_END(stay). Constants verified against esptool v4 loader.py. The ROM
// supports CHANGE_BAUDRATE on the ESP32; TDSP_ESPUP_BAUD opts in after SYNC (115200 = don't).
//
// PC side: projects/t-dsp_esp32_bt_receiver/tools/esp32_ota_wifi.py (push via the ESP32 '!tunnel'
// + @WB, verify @CRC, then @ESPUP, wait for the box to come back, confirm the ESP32's "fw" stamp).
#pragma once
#ifdef TDSP_ESP32_SDFLASH
#include <new>

#ifndef TDSP_ESPUP_BAUD
#define TDSP_ESPUP_BAUD 115200      // ROM default. 460800 cuts a 1.7 MB write from ~150 s to ~40 s (HW-tested value: see README)
#endif

static char g_espupLast[128] = "none";   // @ESPUP? reply: result of the last run (RAM only)

namespace espup {

static const uint8_t  OP_FLASH_BEGIN = 0x02, OP_FLASH_DATA = 0x03, OP_FLASH_END = 0x04, OP_SYNC = 0x08,
                      OP_SPI_SET_PARAMS = 0x0B, OP_SPI_ATTACH = 0x0D, OP_CHANGE_BAUD = 0x0F;
static const uint32_t kBlock      = 0x400;              // ROM FLASH_WRITE_SIZE
static const uint32_t kFlashBytes = 4u * 1024u * 1024u; // the board's ESP32 has 4 MB (esptool flash_id: Device 4016)
static const uint32_t kRomBaud    = 115200;

static HardwareSerial* U = nullptr;
static int lastErr = 0;   // 0 ok; -1 timeout; -2 frame overflow; 0x100|code = ROM status error

static inline void put32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static void slipByte(uint8_t b) {
  if (b == 0xC0)      { U->write((uint8_t)0xDB); U->write((uint8_t)0xDC); }
  else if (b == 0xDB) { U->write((uint8_t)0xDB); U->write((uint8_t)0xDD); }
  else U->write(b);
}

// One request frame: C0 | 00 op len16 chk32 | hdr | data | C0
static void sendCommand(uint8_t op, const uint8_t* hdr, size_t hl, const uint8_t* data, size_t dl, uint32_t chk) {
  uint16_t len = (uint16_t)(hl + dl);
  U->write((uint8_t)0xC0);
  slipByte(0x00); slipByte(op); slipByte(len & 0xFF); slipByte(len >> 8);
  for (int i = 0; i < 4; i++) slipByte((chk >> (8 * i)) & 0xFF);
  for (size_t i = 0; i < hl; i++) slipByte(hdr[i]);
  for (size_t i = 0; i < dl; i++) slipByte(data[i]);
  U->write((uint8_t)0xC0);
}

// Read one SLIP frame (decoded) into buf. Returns its length, -1 on timeout, -2 on overflow.
static int readFrame(uint8_t* buf, size_t cap, uint32_t timeoutMs) {
  uint32_t t0 = millis(); bool in = false, esc = false; size_t len = 0;
  while ((uint32_t)(millis() - t0) < timeoutMs) {
    int c = U->read();
    if (c < 0) continue;
    if (!in) { if (c == 0xC0) { in = true; len = 0; esc = false; } continue; }
    if (c == 0xC0) { if (len == 0) continue; return (int)len; }   // back-to-back C0 = empty, keep going
    if (esc) { buf[len++] = (c == 0xDC) ? 0xC0 : 0xDB; esc = false; }
    else if (c == 0xDB) esc = true;
    else buf[len++] = (uint8_t)c;
    if (len >= cap) return -2;
  }
  return -1;
}

// Send a command and wait for ITS response (dir 0x01, same op). Stray responses (late SYNC echoes)
// are skipped. ESP32 ROM responses carry 4 trailing bytes: status, error, 2 reserved. Returns 0 on
// success and sets lastErr otherwise.
static int command(uint8_t op, const uint8_t* hdr, size_t hl, const uint8_t* data, size_t dl, uint32_t chk,
                   uint32_t timeoutMs, uint32_t* value = nullptr) {
  sendCommand(op, hdr, hl, data, dl, chk);
  uint8_t resp[64];
  uint32_t t0 = millis();
  for (;;) {
    uint32_t spent = millis() - t0;
    if (spent >= timeoutMs) { lastErr = -1; return -1; }
    int n = readFrame(resp, sizeof(resp), timeoutMs - spent);
    if (n == -1) { lastErr = -1; return -1; }
    if (n < 8) continue;                                 // junk / too short
    if (resp[0] != 0x01 || resp[1] != op) continue;      // not ours
    uint16_t len = resp[2] | (resp[3] << 8);
    if (value) *value = resp[4] | (resp[5] << 8) | (resp[6] << 16) | (resp[7] << 24);
    if (len >= 2 && n >= 10) {                           // status bytes follow (no payload for our ops)
      uint8_t st = resp[8], err = resp[9];
      if (st != 0) { lastErr = 0x100 | err; return lastErr; }
    }
    lastErr = 0;
    return 0;
  }
}

static bool sync() {
  uint8_t d[36] = { 0x07, 0x07, 0x12, 0x20 };
  for (int i = 4; i < 36; i++) d[i] = 0x55;
  for (int attempt = 0; attempt < 30; attempt++) {
    if (command(OP_SYNC, nullptr, 0, d, sizeof(d), 0, 150) == 0) {
      // The ROM answers a SYNC with ~8 responses; drain the rest so they don't alias later replies.
      uint32_t t0 = millis();
      while ((uint32_t)(millis() - t0) < 250) while (U->available()) U->read();
      return true;
    }
    delay(50);
  }
  return false;
}

static int spiAttach()      { uint8_t a[8] = { 0 }; return command(OP_SPI_ATTACH, a, sizeof(a), nullptr, 0, 0, 3000); }
static int spiSetParams()   { uint8_t a[24]; put32(a, 0); put32(a + 4, kFlashBytes); put32(a + 8, 64 * 1024); put32(a + 12, 4096); put32(a + 16, 256); put32(a + 20, 0xFFFF);
                              return command(OP_SPI_SET_PARAMS, a, sizeof(a), nullptr, 0, 0, 3000); }
static int flashBegin(uint32_t size, uint32_t offset, uint32_t& nblocks) {
  nblocks = (size + kBlock - 1) / kBlock;
  uint8_t a[16]; put32(a, size); put32(a + 4, nblocks); put32(a + 8, kBlock); put32(a + 12, offset);
  // The ROM erases the whole region inside this command: esptool allows 30 s/MB (min 3 s).
  uint32_t timeout = 3000 + (uint32_t)((uint64_t)size * 30000ull / (1024ull * 1024ull));
  return command(OP_FLASH_BEGIN, a, sizeof(a), nullptr, 0, 0, timeout);
}
static int flashBlock(const uint8_t* blk, uint32_t seq) {
  uint8_t a[16]; put32(a, kBlock); put32(a + 4, seq); put32(a + 8, 0); put32(a + 12, 0);
  uint32_t chk = 0xEF;
  for (uint32_t i = 0; i < kBlock; i++) chk ^= blk[i];
  return command(OP_FLASH_DATA, a, sizeof(a), blk, kBlock, chk, 3000);
}
static int flashEnd()       { uint8_t a[4]; put32(a, 1 /* 1 = stay in the loader; we reboot via EN */); return command(OP_FLASH_END, a, sizeof(a), nullptr, 0, 0, 3000); }
static int changeBaud(uint32_t baud) {
  uint8_t a[8]; put32(a, baud); put32(a + 4, 0 /* ROM: 0; the stub wants the old baud */);
  int r = command(OP_CHANGE_BAUD, a, sizeof(a), nullptr, 0, 0, 3000);
  if (r == 0) { U->flush(); delay(20); U->begin(baud); delay(20); while (U->available()) U->read(); }
  return r;
}

} // namespace espup

// 8 KB UART RX ring for Serial7, allocated once. The ROM's replies are tiny, but the SAME UART carries
// the @WB payload while the image is being staged to the SD card over the ESP32 tunnel, and SD writes
// stall for tens of ms (FAT allocation) — the default 64-byte ring would drop bytes. 8 KB rides out
// ~700 ms at 115200. FlasherX adds its own ring for @FXUP (same idea).
//
// PSRAM, NOT the OCRAM heap: on this build OCRAM is nearly all DMAMEM statics (.bss.dma ~437 KB of
// 512 KB) and the audio block pools come out of what's left at AudioMemory_F32() time. Taking 8 KB of
// heap here first starved that pool -> `new audio_block_f32_t[]` returned null -> DACCVIOL hard fault
// in allocate_f32_memory() on every boot (crash-loop, hardware-verified 2026-10-01). extmem_malloc()
// returns null on a board without PSRAM, in which case we simply run on the stock ring (slower link
// is still correct, SD stalls may cost a retry).
FLASHMEM static void espSdFlashBegin() {
  static uint8_t* ring = nullptr;
  // Serial7 by name: addMemoryForRead() is on the Teensy 4 class (HardwareSerialIMXRT), not the HardwareSerial
  // base the kit hands out. The kit's UART IS Serial7 (TDspEsp32 default, pins 28/29).
  if (!ring) {
    ring = (uint8_t*)extmem_malloc(8192);
    if (ring) Serial7.addMemoryForRead(ring, 8192);
    Serial.printf("[espup] Serial7 RX ring: %s\n", ring ? "8 KB in PSRAM" : "no PSRAM, stock ring");
  }
}

// Flash <path> (an SD file) into the ESP32 at <offset>. Blocks; pauses audio; reboots the ESP32
// into its app afterwards whether or not the write succeeded. Logs to `log` (USB Serial — the
// ESP32 link is the thing being reprogrammed). Result also lands in g_espupLast for @ESPUP?.
FLASHMEM static bool espFlashFromSd(const char* path, uint32_t offset, Print& log) {
  using namespace espup;
  File f = SD.open(path);
  if (!f || f.isDirectory()) { snprintf(g_espupLast, sizeof(g_espupLast), "ERR open %s", path); log.printf("[espup] %s\n", g_espupLast); return false; }
  uint32_t size = f.size();
  if (!size) { f.close(); snprintf(g_espupLast, sizeof(g_espupLast), "ERR empty %s", path); log.printf("[espup] %s\n", g_espupLast); return false; }
  log.printf("[espup] %s (%lu bytes) -> ESP32 flash @0x%lx\n", path, (unsigned long)size, (unsigned long)offset); log.flush();

  // Block buffer from PSRAM when present (the OCRAM heap is tight on this build, see espSdFlashBegin).
  bool blkExt = true;
  uint8_t* blk = (uint8_t*)extmem_malloc(kBlock);
  if (!blk) { blkExt = false; blk = (uint8_t*)malloc(kBlock); }
  if (!blk) { f.close(); snprintf(g_espupLast, sizeof(g_espupLast), "ERR no RAM"); return false; }

  AudioNoInterrupts();            // the audio ISR load would starve the byte pump (same rule as the kit's passthrough)
  U = &kit.uart();
  uint32_t t0 = millis();
  bool ok = false; const char* stage = "download"; bool baudChanged = false;
  do {
    if (!kit.resetIntoDownload(log)) break;
    delay(50); while (U->available()) U->read();
    stage = "sync";        if (!sync()) break;
    log.println("[espup] ROM sync OK"); log.flush();
#if TDSP_ESPUP_BAUD != 115200
    stage = "baud";
    if (changeBaud(TDSP_ESPUP_BAUD) == 0) { baudChanged = true; log.printf("[espup] baud -> %lu\n", (unsigned long)TDSP_ESPUP_BAUD); }
    else log.printf("[espup] baud change refused (err %d), staying at %lu\n", lastErr, (unsigned long)kRomBaud);
#endif
    stage = "spi_attach";  if (spiAttach()) break;
    stage = "spi_params";  if (spiSetParams()) break;
    uint32_t nblocks = 0;
    stage = "flash_begin"; log.println("[espup] erasing..."); log.flush(); if (flashBegin(size, offset, nblocks)) break;
    stage = "flash_data";
    bool dataOk = true;
    for (uint32_t seq = 0; seq < nblocks; seq++) {
      int n = f.read(blk, kBlock);
      if (n < 0) { dataOk = false; break; }
      if ((uint32_t)n < kBlock) memset(blk + n, 0xFF, kBlock - n);   // pad the last block like esptool
      int r = -1;
      for (int tries = 0; tries < 3 && r != 0; tries++) r = flashBlock(blk, seq);   // esptool retries each block 3x
      if (r != 0) { dataOk = false; break; }
      if ((seq & 63) == 63 || seq + 1 == nblocks)
        log.printf("[espup] %lu/%lu KB  %lu s\n", (unsigned long)((seq + 1) * kBlock / 1024), (unsigned long)(size / 1024), (unsigned long)((millis() - t0) / 1000));
    }
    if (!dataOk) break;
    stage = "flash_end";   if (flashEnd()) { log.println("[espup] FLASH_END refused (image is written; continuing)"); }
    ok = true;
  } while (0);
  f.close();
  if (blkExt) extmem_free(blk); else free(blk);
  if (baudChanged) { U->begin(kRomBaud); delay(20); }     // back to the app's UART rate
  kit.bootApp();                                           // IO0 high, EN pulse, hold: ESP32 runs whatever is in flash now
  AudioInterrupts();
  if (ok) snprintf(g_espupLast, sizeof(g_espupLast), "OK %s %lu bytes @0x%lx in %lu s", path, (unsigned long)size, (unsigned long)offset, (unsigned long)((millis() - t0) / 1000));
  else    snprintf(g_espupLast, sizeof(g_espupLast), "ERR at %s (code %d) after %lu s", stage, lastErr, (unsigned long)((millis() - t0) / 1000));
  log.printf("[espup] %s\n", g_espupLast); log.flush();
  return ok;
}

// @ESPUP=<path>[\x1f<hexoffset>]  — parse + run. `reply` = the link the command came in on.
FLASHMEM static void espupCommand(const char* args, Stream& reply) {
  char path[128]; uint32_t offset = 0x10000;
  const char* sep = strchr(args, '\x1f');
  size_t pl = sep ? (size_t)(sep - args) : strlen(args);
  if (pl == 0 || pl >= sizeof(path)) { reply.println("@ESPUP_ERR=bad path"); return; }
  memcpy(path, args, pl); path[pl] = 0;
  if (sep) offset = (uint32_t)strtoul(sep + 1, nullptr, 16);
  if (!SD.exists(path)) { reply.printf("@ESPUP_ERR=no such file %s\n", path); return; }
  reply.printf("@ESPUP_GO=%s\x1f%lx\n", path, (unsigned long)offset);   // reaches the WS client before the ESP32 is reset
  reply.flush();
  delay(150);                                                            // let the ESP32 forward that line
  espFlashFromSd(path, offset, Serial);
}

#endif // TDSP_ESP32_SDFLASH
