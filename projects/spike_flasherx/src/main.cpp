// spike_flasherx — measure the RAM/flash footprint of FlasherX on a Teensy 4.1.
//
// Purpose: answer "how much does FlasherX cost when installed?" by building the
// SAME firmware twice — once without FlasherX (env teensy41_base) and once with
// the receiver linked + reachable (env teensy41_flasherx, -DWITH_FLASHERX). The
// PlatformIO size report (RAM1/RAM2/FLASH) diff between the two envs is the
// answer. The receiver also actually works: send 'u' then stream an Intel-hex
// over USB Serial to self-flash. This mirrors the eventual ESP32->Serial7 path;
// here the transport is USB Serial so we can drive it from the PC directly.
//
// FlasherX buffers the incoming image in unused FLASH (RAM_BUFFER_SIZE == 0 in
// FlashTxx.h), so the RAM cost is only the .fastrun (ITCM) flash routines plus a
// small hex-parse line buffer — NOT proportional to the image size.

#include <Arduino.h>

#ifdef WITH_FLASHERX
#include "FXUtil.h"  // read_ascii_line(), update_firmware()
extern "C" {
#include "FlashTxx.h"  // FLASH_ID, firmware_buffer_init(), REBOOT, etc.
}
#endif

static const int kLed = LED_BUILTIN;

// Rough free-RAM probe (Teensy 4: gap between heap break and current stack).
extern "C" char* sbrk(int incr);
static uint32_t freeRamBytes() {
  char stackVar;
  return (uint32_t)(&stackVar) - (uint32_t)sbrk(0);
}

static void printBanner() {
  Serial.printf("spike_flasherx boot — free RAM ~= %lu bytes\n",
                (unsigned long)freeRamBytes());
#ifdef WITH_FLASHERX
  Serial.printf("FlasherX LINKED — target %s (%dK flash, %dK sectors)\n",
                FLASH_ID, FLASH_SIZE / 1024, FLASH_SECTOR_SIZE / 1024);
  Serial.println("send 'u' to enter update mode (stream Intel-hex over USB)");
#else
  Serial.println("baseline build — FlasherX NOT linked");
#endif
}

#ifdef WITH_FLASHERX
static void doUpdate() {
  uint32_t buffer_addr, buffer_size;
  if (firmware_buffer_init(&buffer_addr, &buffer_size) == 0) {
    Serial.println("unable to create firmware buffer");
    return;
  }
  Serial.printf("buffer = %luK %s (%08lX - %08lX)\n", buffer_size / 1024,
                IN_FLASH(buffer_addr) ? "FLASH" : "RAM", buffer_addr,
                buffer_addr + buffer_size);
  Serial.println("stream the .hex now...");
  update_firmware(&Serial, &Serial, buffer_addr, buffer_size);
  // Only returns on error/abort — clean up and reboot to a known state.
  Serial.println("update aborted — freeing buffer, rebooting");
  firmware_buffer_free(buffer_addr, buffer_size);
  Serial.flush();
  REBOOT;
}
#endif

void setup() {
  pinMode(kLed, OUTPUT);
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 2000) {
  }
  if (CrashReport) Serial.print(CrashReport);
  printBanner();
}

void loop() {
  // Heartbeat so we can confirm the board is alive after flashing.
  digitalWrite(kLed, (millis() / 500) & 1);

  if (Serial.available()) {
    int c = Serial.read();
#ifdef WITH_FLASHERX
    if (c == 'u' || c == 'U') doUpdate();
#endif
    if (c == 'r' || c == 'R') {
      Serial.printf("free RAM ~= %lu bytes\n", (unsigned long)freeRamBytes());
    }
  }
}
