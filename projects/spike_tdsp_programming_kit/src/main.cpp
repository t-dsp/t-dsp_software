// spike_tdsp_programming_kit — proves lib/TDspProgrammingKit end to end.
// ---------------------------------------------------------------------------
// Boots the on-board ESP32 into its app (holding EN+IO0 high) and exposes the
// touch-free flash loop. No audio graph -> the USB<->ESP32 passthrough is
// unencumbered, so this is the simplest possible integration of the kit.
//
// Commands over the Teensy USB serial (COM @115200):
//   g          -> ESP32 into ROM download + passthrough  (then: esptool --before no_reset)
//   @BOOTAPP@  -> leave flash mode via soft-reboot; ESP32 back into its app
//   U          -> Teensy into HalfKay (program mode) for teensy_loader_cli -w
//   r          -> reset the ESP32 into its app
// Any serial the ESP32 emits (boot log, A2DP/BLE status, ...) is mirrored to USB.
#include <Arduino.h>
#include <TDspProgrammingKit.h>

TDspProgrammingKit kit;   // defaults: EN=pin37, IO0=pin36, Serial7 @115200, LED blink

void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 1500) {}
    Serial.println("=== spike_tdsp_programming_kit ===");
    Serial.println("cmds: g=flash ESP32   @BOOTAPP@=exit flash   U=Teensy program mode   r=reset ESP32");
    kit.begin();   // boots the ESP32 into its app, holds EN+IO0 high
    Serial.println("[spike] ESP32 booted + held. If it runs the A2DP firmware, pair 'T-DSP'.");
}

void loop() {
    // Flash-mode passthrough owns the loop while active (also handles @BOOTAPP@ + blink).
    if (kit.service(Serial)) return;

    // Kit commands: g / r / U. Returns true if it consumed the byte.
    if (Serial.available()) {
        int c = Serial.read();
        if (kit.handleChar(Serial, c)) return;
        // (no app-specific commands in this spike)
    }

    // Mirror whatever the ESP32 prints on its UART0.
    while (kit.uart().available()) Serial.write((uint8_t)kit.uart().read());
}
