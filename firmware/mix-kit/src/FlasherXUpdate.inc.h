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
  // Reads hex from &io, writes/verifies/moves to program flash, then REBOOTs on
  // success (does not return). On error/abort it returns here.
  update_firmware(&io, &io, buffer_addr, buffer_size);
  io.println("[fx] update aborted — freeing buffer, rebooting");
  firmware_buffer_free(buffer_addr, buffer_size);
  io.flush();
  REBOOT;
}

#endif  // TDSP_FLASHERX
