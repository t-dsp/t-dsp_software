// TDspProgrammingKit — drop-in, touch-free ESP32 + Teensy programming/boot control.
// ===========================================================================
// One master (the Teensy) owns the on-board ESP32's EN/IO0 straps and its UART0,
// so a whole project can boot, hold, reset, and *reflash* the ESP32 — and reflash
// the Teensy — with NO button presses and NO power-cycle. Wraps the low-level
// TDspEsp32 pin/UART driver with the field-proven higher-level workflow
// (see projects/spike_esp32_bt_spdif_mix + its HANDOFF.md for the war story).
//
// Wiring (Teensy 4.1; TDspEsp32 defaults):
//   Teensy pin37 -> ESP32 EN        (must be a SOLID connection; on the current
//                                    board this needed a jumper — a bad trace only
//                                    pulled EN to ~1.8V, above the reset threshold)
//   Teensy pin36 -> ESP32 GPIO0/BOOT (native/good)
//   Teensy pin28 (RX7) <- ESP32 U0TXD, pin29 (TX7) -> ESP32 U0RXD   (Serial7)
//
// What it does that a naive bridge does not:
//   * bootApp() HOLDS EN+IO0 high (never releases) — boards where EN has no pull-up
//     collapse straight back into reset the instant you stop driving it.
//   * enterFlash() drives the ESP32 into ROM download from the TEENSY (deterministic),
//     VERIFYING the ROM actually responded and RE-PULSING up to dlAttempts times — so a
//     slightly loose jumper or a mid-stream reset still lands. (The esptool DTR/RTS
//     auto-reset emulation was unreliable; this replaces it.)
//   * Calls an onFlashEnter() hook so an audio project can pause its graph — a busy
//     audio ISR load (~200% CPU) otherwise starves the passthrough and drops bytes.
//   * A soft-reboot escape token ("@BOOTAPP@") leaves flash mode WITHOUT a power-cycle.
//   * 'U' drops the Teensy into HalfKay (program mode) for button-free teensy uploads.
//
// Usage:
//   #include <TDspProgrammingKit.h>
//   TDspProgrammingKit kit;                 // defaults: EN37/IO036/Serial7, LED blink
//   void setup() { Serial.begin(115200); kit.begin(); }   // boots+holds the ESP32
//   void loop() {
//     if (kit.service(Serial)) return;      // flash-mode passthrough owns the loop
//     if (Serial.available()) { int c = Serial.read(); if (kit.handleChar(Serial, c)) return; }
//     while (kit.uart().available()) Serial.write(kit.uart().read());   // mirror ESP32
//   }
//
// Flash the ESP32 (host side, no buttons/power-cycle):
//   1) send 'g' to the Teensy
//   2) esptool --chip esp32 --port <TeensyCOM> --baud 115200 \
//              --before no_reset --after no_reset write_flash <offsets> <bins>
//   3) send "@BOOTAPP@"   (Teensy soft-reboots -> ESP32 back in its app)
// Flash the Teensy: send 'U', then teensy_loader_cli -w  (no PROGRAM button).
// ===========================================================================
#pragma once
#include <Arduino.h>
#include <TDspEsp32.h>

extern "C" void _reboot_Teensyduino_(void);   // Teensy core: jump into HalfKay

class TDspProgrammingKit {
 public:
  struct Config {
    TDspEsp32::Config esp;                 // EN/IO0/uart/baud (default 37/36/Serial7/115200)
    uint8_t      ledPin      = LED_BUILTIN; // fast blink while in flash mode
    IRQ_NUMBER_t uartIrq     = IRQ_LPUART7; // raise UART ISR above audio DMA (Serial7)
    uint16_t     resetHoldMs = 1000;        // EN-low hold; board reset caps -> generous
    uint8_t      resetPulses = 2;           // reset-into-app pulses (insurance)
    uint8_t      dlAttempts  = 5;           // download-entry retries (loose-jumper safe)
    uint16_t     dlWindowMs  = 700;         // ROM-response listen window per attempt
    const char*  escape      = "@BOOTAPP@"; // host token -> leave flash mode (soft-reboot)
  };

  TDspProgrammingKit() : esp_(cfg_.esp) {}
  explicit TDspProgrammingKit(const Config &cfg) : cfg_(cfg), esp_(cfg.esp) {}

  // Optional: called right after the ESP32 is placed in download, before passthrough.
  // Audio projects should pass AudioNoInterrupts here so the passthrough isn't starved.
  void onFlashEnter(void (*fn)()) { onEnter_ = fn; }

  void begin() {
    pinMode(cfg_.ledPin, OUTPUT);
    esp_.begin();                 // EN/IO0 -> OUTPUT high, uart.begin(baud)
    bootApp();
  }

  // Reset the ESP32 into its application, then HOLD EN+IO0 high (do NOT release):
  // on boards whose EN has no pull-up, releasing lets EN sag to 0V -> back into reset.
  void bootApp() {
    esp_.setBootLow(false);                       // IO0 HIGH (app strap)
    delay(400);                                   // let the rail settle
    for (uint8_t i = 0; i < cfg_.resetPulses; ++i) {
      esp_.assertReset(true);  delay(cfg_.resetHoldMs);   // EN LOW  (reset)
      esp_.assertReset(false); delay(250);                // EN HIGH (run)
    }
    esp_.setBootLow(false);                       // IO0 HIGH, held
    esp_.assertReset(false);                      // EN  HIGH, held (no release)
    flash_ = false;
  }

  bool inFlashMode() const { return flash_; }
  HardwareSerial &uart() { return *cfg_.esp.uart; }
  TDspEsp32 &esp() { return esp_; }

  // Process one host command byte (call only when NOT in flash mode).
  // 'g' enter flash, 'U' Teensy program mode, 'r' reset ESP32 -> app.
  // Returns true if the byte was a kit command (else it's yours to handle).
  template <class UsbSerial>
  bool handleChar(UsbSerial &host, int c) {
    switch (c) {
      case 'g': enterFlash(host); return true;
      case 'r': host.println("[kit] reset ESP32 -> app"); bootApp(); return true;
      case 'U': host.println("[kit] -> Teensy PROGRAM MODE (HalfKay). Upload: teensy_loader_cli -w.");
                host.flush(); delay(50); _reboot_Teensyduino_(); return true;
      default:  return false;
    }
  }

  // Drive the ESP32 into ROM download (verify + retry) and enter USB<->ESP32 passthrough.
  template <class UsbSerial>
  void enterFlash(UsbSerial &host) {
    host.println("[kit] FLASH MODE: ESP32 -> download; USB<->ESP32 passthrough. Run esptool "
                 "--before no_reset --after no_reset --baud 115200. Send escape to exit.");
    host.flush();
    NVIC_SET_PRIORITY(cfg_.uartIrq, 64);          // UART ISR above audio DMA -> no FIFO drops
    HardwareSerial &u = *cfg_.esp.uart;
    bool inDl = false;
    for (uint8_t a = 1; a <= cfg_.dlAttempts && !inDl; ++a) {
      esp_.setBootLow(true);                      // IO0 LOW  (select download)
      esp_.assertReset(true);                     // EN  LOW  (reset)
      delay(400);                                 // hold (reset caps)
      while (u.available()) u.read();             // flush pre-reset noise
      esp_.assertReset(false);                    // EN HIGH -> boot; IO0 low = download
      int n = 0; uint32_t t0 = millis();
      while ((uint32_t)(millis() - t0) < cfg_.dlWindowMs)
        while (u.available()) { u.read(); ++n; }  // ROM banner = reset landed
      esp_.setBootLow(false);                     // release IO0 (strap already latched)
      host.printf("[kit] reset attempt %u: %d ROM bytes\n", a, n);
      inDl = (n > 4);
    }
    host.println(inDl ? "[kit] ESP32 in DOWNLOAD -- passthrough ON."
                      : "[kit] WARN: no ROM response (check EN wiring) -- passthrough ON anyway.");
    if (onEnter_) onEnter_();                      // e.g. AudioNoInterrupts()
    esc_ = 0;
    flash_ = true;
  }

  // Call every loop iteration. In flash mode: raw passthrough + escape detect + blink,
  // returns true (the host should skip its own work). Otherwise returns false.
  template <class UsbSerial>
  bool service(UsbSerial &host) {
    if (!flash_) return false;
    blink();
    HardwareSerial &u = *cfg_.esp.uart;
    const char *esc = cfg_.escape;
    while (host.available()) {
      uint8_t b = (uint8_t)host.read();
      u.write(b);                                 // USB -> ESP32 (esptool)
      if (b == (uint8_t)esc[esc_]) { if (esc[++esc_] == '\0') softReboot(); }
      else esc_ = (b == (uint8_t)esc[0]) ? 1 : 0;
    }
    while (u.available()) host.write((uint8_t)u.read());   // ESP32 -> USB
    return true;
  }

 private:
  void softReboot() { SCB_AIRCR = 0x05FA0004; while (1) {} }   // Teensy self-reset -> setup()
  void blink() {
    if ((uint32_t)(millis() - ledT_) >= 70) {
      ledT_ = millis(); ledOn_ = !ledOn_; digitalWrite(cfg_.ledPin, ledOn_);
    }
  }

  Config     cfg_;
  TDspEsp32  esp_;
  bool       flash_ = false;
  uint8_t    esc_   = 0;
  uint32_t   ledT_  = 0;
  bool       ledOn_ = false;
  void     (*onEnter_)() = nullptr;
};
