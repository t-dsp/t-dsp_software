// FlasherXUpdate.inc.h — OTA self-update hook for the mix-kit (opt-in).
//
// Gated entirely behind -D TDSP_FLASHERX (see the *_fx env in platformio.ini), so
// default builds are byte-identical and unaffected. When enabled, the @FXUP
// control command hands the active serial stream to FlasherX (lib/FlasherX):
// read an Intel-hex firmware image, buffer it in unused FLASH, verify the target
// id, move it to the program base, and reboot into the new image.
//
// Cost when linked (measured on this board, projects/spike_flasherx): ~48 KB flash
// + ~28 KB RAM1 — most of it the C-library scanf/float pulled in by the Intel-hex
// parser, NOT the flash-write core. See spike RESUME for the breakdown.
//
// First transport is USB self-update (PC streams the .hex over USB Serial). The
// ESP32->Serial7 relay is a later phase; when added, generalise fxRunUpdate() to
// take the incoming stream instead of hard-wiring Serial at the @FXUP call site.
#pragma once
#ifdef TDSP_FLASHERX

#include "FXUtil.h"  // read_ascii_line(), update_firmware()
extern "C" {
#include "FlashTxx.h"  // FLASH_ID, firmware_buffer_init(), IN_FLASH(), REBOOT
}

// Enter update mode on `io` (used for BOTH the hex input and status/echo, matching
// FlasherX's serial mode). Blocks while the image transfers. Reboots into the new
// firmware on success (never returns); returns only on error/abort.
static void fxRunUpdate(Stream& io) {
  // Over the ESP32 UART relay, bytes stream in continuously at 115200 while
  // FlasherX pauses to write each record to flash (interrupts briefly off). The
  // default 64-byte Serial7 RX ring can overflow during those pauses -> a dropped
  // byte -> "bad hex line". Give the UART a large RX ring so it rides through the
  // write pauses. Only for the UART path (the USB Serial isn't a HardwareSerial).
  // MUST live in DMAMEM (OCRAM): this Dexed-pool build leaves only ~15 KB DTCM
  // stack, so an 8 KB DTCM static overflows the stack when the A2DP graph goes live
  // (crash-loop on boot -- learned the hard way). OCRAM has 500+ KB free.
  static DMAMEM uint8_t s_rxbuf[8192];
  if (&io == (Stream*)&Serial7) Serial7.addMemoryForRead(s_rxbuf, sizeof(s_rxbuf));

  uint32_t buffer_addr, buffer_size;
  io.printf("[fx] FlasherX update — target %s (%dK flash, %dK sectors)\n",
            FLASH_ID, FLASH_SIZE / 1024, FLASH_SECTOR_SIZE / 1024);
  if (firmware_buffer_init(&buffer_addr, &buffer_size) == 0) {
    io.println("[fx] ERROR: cannot create firmware buffer");
    return;
  }
  io.printf("[fx] buffer = %luK %s (%08lX-%08lX) — stream the .hex now\n",
            buffer_size / 1024, IN_FLASH(buffer_addr) ? "FLASH" : "RAM",
            buffer_addr, buffer_addr + buffer_size);
  io.flush();

  // DIAGNOSTIC: clear the LPUART7 hardware RX overrun flag so we can tell after the
  // transfer whether bytes were lost to a FIFO overrun (RX ISR starved) vs. some
  // other cause (e.g. ESP-side drop). LPUART_STAT_OR is bit 19, write-1-to-clear.
  bool uartPath = (&io == (Stream*)&Serial7);
  if (uartPath) IMXRT_LPUART7.STAT |= (1u << 19);

  // Reads hex from &io, writes/verifies/moves to program flash, then REBOOTs on
  // success (does not return). On error/abort it returns here.
  update_firmware(&io, &io, buffer_addr, buffer_size);

  if (uartPath) {
    uint32_t stat = IMXRT_LPUART7.STAT;
    io.printf("[fx] LPUART7 STAT=%08lX rx_overrun=%d (1=FIFO overflowed, bytes lost)\n",
              (unsigned long)stat, (int)((stat >> 19) & 1u));
  }
  io.println("[fx] update aborted — freeing buffer, rebooting");
  firmware_buffer_free(buffer_addr, buffer_size);
  io.flush();
  REBOOT;
}

#endif  // TDSP_FLASHERX
