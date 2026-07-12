// TDspEsp32 — Teensy-side driver for the on-board ESP32-DevKitC.
// ---------------------------------------------------------------------------
// Board: Teensy 4.1 + ESP32-DevKitC (t-dsp_dual_preamp_lite / digital audio
// board). The Teensy owns the ESP32's reset/boot straps and its UART0, so it
// can act as the ESP32's USB-serial adapter + auto-reset controller. Drop this
// object into any project to (re)program the ESP32 on demand, with NO button
// presses and without the ESP32's own USB.
//
// Default pin map (verified from schematic — see
// projects/t-dsp_esp32_bt_receiver/docs/teensy_programs_esp32.md):
//   ESP32 EN   <- Teensy pin 37   (active-low reset)
//   ESP32 IO0  <- Teensy pin 36   (GPIO0/BOOT strap; LOW at reset = download)
//   ESP32 U0TXD-> Teensy pin 28   (RX7)  \  Serial7
//   ESP32 U0RXD<- Teensy pin 29   (TX7)  /
// Requires a Teensy 4.1 (pins 28/29/36/37 are only on the 4.1's rear edges).
//
// -------------------- Two ways to program the ESP32 ------------------------
// 1) HOST-DRIVEN (implemented here): the PC's esptool flashes the ESP32
//    *through* the Teensy. In "flash passthrough" mode the Teensy forwards
//    USB<->Serial7 and emulates the auto-reset circuit from the host DTR/RTS
//    lines. Usage:
//
//        #include <TDspEsp32.h>
//        TDspEsp32 esp;              // defaults: pins 37/36, Serial7 @115200
//        void setup() { Serial.begin(115200); esp.begin(); }
//        void loop()  { esp.bridgeTask(Serial); }   // dedicated flasher
//
//    In a real app, gate bridgeTask() behind a "flash mode" you enter on a
//    command (so it doesn't steal the USB from your normal protocol), e.g.:
//        if (flashMode) esp.bridgeTask(Serial); else { ...normal app... }
//    Then on the PC:  python -m esptool --port <Teensy COM> --baud 115200 \
//                       --before default-reset --after hard-reset write-flash ...
//    IMPORTANT: keep esptool at --baud 115200; Serial7 here is a fixed-rate
//    UART and esptool's higher-baud renegotiation cannot cross the bridge.
//
// 2) TEENSY-NATIVE (not yet implemented): the Teensy holds the ESP32 image and
//    speaks the esptool ROM-bootloader protocol itself. enterDownloadMode()
//    below is the entry point that a future flashImage() would build on.
//
// ---------------------------------------------------------------------------
// Why the settle debounce matters: the classic auto-reset truth table is
//   (DTR,RTS) 00->EN1 IO0-1  01->EN0 IO0-1(reset)  10->EN1 IO0-0(boot)  11->EN1 IO0-1
// esptool enters download mode by going (0,1)[reset] -> (1,0)[EN release, IO0
// low], but between those it briefly passes through the (1,1) transient (two
// separate USB DTR/RTS transfers). A naive bridge that reacts to that transient
// releases EN while IO0 is still HIGH -> the chip samples IO0=HIGH at the EN
// rising edge and boots NORMALLY (boot:0x13) instead of into the ROM bootloader.
// That was the bug that forced a manual BOOT-button press.
//
// Fix: debounce. Only apply a (DTR,RTS) combination after it has been STABLE for
// settleUs, which filters out the brief (1,1) transient, and apply IO0 *before*
// EN so IO0 is at its final level before EN releases. The intended esptool
// states are held ~50-100 ms, far longer than settleUs, so they always apply;
// the ~1-3 ms transient never does.
#pragma once
#include <Arduino.h>

class TDspEsp32 {
 public:
  struct Config {
    uint8_t enPin = 37;               // Teensy -> ESP32 EN  (active-low reset)
    uint8_t io0Pin = 36;              // Teensy -> ESP32 GPIO0/BOOT
    HardwareSerial *uart = &Serial7;  // Teensy UART -> ESP32 UART0 (pins 28/29)
    uint32_t baud = 115200;           // ROM bootloader default; keep across bridge
    uint32_t settleUs = 10000;        // DTR/RTS debounce, filters the (1,1) glitch
  };

  TDspEsp32() {}
  explicit TDspEsp32(const Config &cfg) : cfg_(cfg) {}

  // Claim EN/IO0 (drive to run state) and open the UART to the ESP32.
  void begin() {
    pinMode(cfg_.enPin, OUTPUT);
    pinMode(cfg_.io0Pin, OUTPUT);
    digitalWrite(cfg_.enPin, HIGH);   // not in reset
    digitalWrite(cfg_.io0Pin, HIGH);  // normal boot strap
    resetDebounce();
    cfg_.uart->begin(cfg_.baud);
    started_ = true;
  }

  // Release EN/IO0 to hi-Z so this driver won't hold the straps or fight the
  // ESP32's own USB-serial auto-reset. Call when handing the ESP32 back to
  // direct-USB flashing, or in a project that doesn't want to touch the straps.
  void release(bool alsoUart = false) {
    pinMode(cfg_.enPin, INPUT);
    pinMode(cfg_.io0Pin, INPUT);
    if (alsoUart && started_) {
      cfg_.uart->end();
      started_ = false;
    }
  }

  HardwareSerial &uart() { return *cfg_.uart; }

  // --- Explicit reset / boot control ------------------------------------
  void assertReset(bool on) { digitalWrite(cfg_.enPin, on ? LOW : HIGH); }
  void setBootLow(bool low) { digitalWrite(cfg_.io0Pin, low ? LOW : HIGH); }

  // Reboot the ESP32 into its application (IO0 high across an EN pulse).
  void resetToApp() {
    digitalWrite(cfg_.io0Pin, HIGH);
    digitalWrite(cfg_.enPin, LOW);
    delayMicroseconds(2000);
    digitalWrite(cfg_.enPin, HIGH);
    resetDebounce();
  }

  // Reset the ESP32 into the ROM serial-download (bootloader) mode: hold IO0
  // low across an EN pulse, then release IO0.
  void enterDownloadMode() {
    digitalWrite(cfg_.io0Pin, LOW);   // select download
    digitalWrite(cfg_.enPin, LOW);
    delayMicroseconds(2000);
    digitalWrite(cfg_.enPin, HIGH);   // release reset with IO0 still low
    delayMicroseconds(2000);
    digitalWrite(cfg_.io0Pin, HIGH);  // release boot strap
    resetDebounce();
  }

  // --- Host-driven flash passthrough ------------------------------------
  // Call every loop iteration while in flash mode. Forwards bytes both ways and
  // emulates the auto-reset circuit from the host's DTR/RTS. `host` is the
  // Teensy USB serial (usb_serial_class); templated so this header needs no USB
  // type. Returns the number of bytes forwarded (either direction) this call.
  template <class UsbSerial>
  unsigned bridgeTask(UsbSerial &host) {
    applyResetControl(host.dtr(), host.rts());
    unsigned moved = 0;
    int n = host.available();
    while (n-- > 0) {
      int c = host.read();
      if (c >= 0) { cfg_.uart->write((uint8_t)c); ++moved; }
    }
    n = cfg_.uart->available();
    while (n-- > 0) {
      int c = cfg_.uart->read();
      if (c >= 0) { host.write((uint8_t)c); ++moved; }
    }
    return moved;
  }

  // Blocking dedicated-flasher loop (for a bridge-only sketch); never returns.
  template <class UsbSerial>
  [[noreturn]] void runBridge(UsbSerial &host) {
    for (;;) bridgeTask(host);
  }

 private:
  // Debounced auto-reset: only apply a (DTR,RTS) state after it is stable for
  // settleUs (filters esptool's brief (1,1) glitch); apply IO0 before EN.
  void applyResetControl(bool dtr, bool rts) {
    if (dtr != lastDtr_ || rts != lastRts_) {  // state changed: restart timer
      lastDtr_ = dtr;
      lastRts_ = rts;
      stableStart_ = micros();
      applied_ = false;
      return;
    }
    if (applied_) return;
    if ((uint32_t)(micros() - stableStart_) < cfg_.settleUs) return;
    applied_ = true;
    digitalWrite(cfg_.io0Pin, (dtr && !rts) ? LOW : HIGH);  // IO0 first
    digitalWrite(cfg_.enPin, (rts && !dtr) ? LOW : HIGH);   // then EN
  }

  // Force the next bridge sample to re-evaluate after a manual reset call.
  void resetDebounce() { applied_ = false; stableStart_ = micros(); }

  Config cfg_;
  bool started_ = false;
  bool lastDtr_ = false;
  bool lastRts_ = false;
  bool applied_ = true;
  uint32_t stableStart_ = 0;
};
