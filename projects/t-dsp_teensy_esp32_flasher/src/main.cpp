#include <Arduino.h>
#include <TDspEsp32.h>
//
// Dedicated ESP32 flasher/bridge sketch for the T-DSP board (Teensy 4.1 +
// ESP32-DevKitC). Turns the Teensy into a transparent USB<->ESP32 UART bridge
// with auto-reset emulation, so the PC's esptool can flash the ESP32 through
// the Teensy — no button presses, no ESP32 USB. All the real logic lives in the
// reusable lib/TDspEsp32 driver; this is just its dedicated host app.
//
//   python -m esptool --port <Teensy COM> --chip esp32 --baud 115200 \
//     --before default-reset --after hard-reset write-flash \
//     0x1000 bootloader.bin 0x8000 partitions.bin \
//     0xe000 boot_app0.bin 0x10000 firmware.bin
//
// Keep esptool at --baud 115200 (Serial7 is a fixed-rate UART across the bridge).
// Flash this sketch onto the Teensy with:
//   pio run -d projects/t-dsp_teensy_esp32_flasher
//   tool-teensy/teensy_reboot.exe
//   tool-teensy/teensy_loader_cli.exe --mcu=TEENSY41 -w .pio/build/teensy41/firmware.hex

constexpr int LED = 13;

TDspEsp32 esp;  // defaults: EN=37, IO0=36, Serial7 @ 115200

void setup() {
  pinMode(LED, OUTPUT);
  Serial.begin(115200);  // USB CDC (baud value is virtual/ignored)
  esp.begin();           // claim EN/IO0 (run state), open Serial7 to the ESP32
}

void loop() {
  esp.bridgeTask(Serial);                             // USB <-> ESP32 + auto-reset
  digitalWriteFast(LED, (millis() & 0x200) ? HIGH : LOW);  // heartbeat
}
