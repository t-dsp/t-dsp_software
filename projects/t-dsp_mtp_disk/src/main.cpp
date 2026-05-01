// T-DSP MTP Disk — companion firmware for SD content authoring.
//
// Boots the Teensy 4.1 as a USB MTP storage device exposing the
// BUILTIN_SDCARD over the USB descriptor. Plug into a host, the
// SD card shows up as "T-DSP SD" in Windows Explorer / macOS Finder
// / `lsblk` on Linux.
//
// Use case: Drop sample banks (WAV files) into /samples/<bank>/ on
// the card without pulling it out of the Teensy. When done, flash the
// audio firmware (t-dsp_f32_audio_shield) back to return to normal
// synth operation. tools/file_mode.sh automates the swap.
//
// Notes
// -----
// * USB_MTPDISK_SERIAL is mutually exclusive with USB_MIDI_AUDIO_SERIAL.
//   Audio playback is not available while this firmware is running.
// * BUILTIN_SDCARD on Teensy 4.1 = 4-bit SDIO (the fast slot, on the
//   underside of the board). The audio shield's SD slot won't work
//   here without different SD.begin() arguments.
// * macOS / Linux see this as a generic MTP device. Linux users may
//   want libmtp / Files or `gvfs-mtp` to mount and copy.
// * A 1 Hz LED heartbeat indicates the main loop is alive.

#include <Arduino.h>
#include <SD.h>
#include <MTP_Teensy.h>

constexpr uint8_t LED_PIN = LED_BUILTIN;

void setup() {
    pinMode(LED_PIN, OUTPUT);

    Serial.begin(115200);
    // Brief wait for serial monitor; don't block forever — host-side
    // file copy works whether serial is connected or not.
    const uint32_t bootStart = millis();
    while (!Serial && (millis() - bootStart) < 2000) {
        digitalWrite(LED_PIN, !digitalRead(LED_PIN));
        delay(100);
    }

    Serial.println("================================");
    Serial.println("  T-DSP MTP Disk firmware");
    Serial.println("  Exposes BUILTIN_SDCARD as USB MTP storage");
    Serial.println("================================");

    if (!SD.begin(BUILTIN_SDCARD)) {
        Serial.println("ERROR: SD card init failed (BUILTIN_SDCARD)");
        Serial.println("Check the card is seated in the Teensy 4.1's");
        Serial.println("built-in slot (NOT the audio shield's slot).");
    } else {
        Serial.println("SD card initialized");
    }

    // MTP responder. begin() initializes the USB MTP class layer;
    // addFilesystem() registers a storage device that the host sees.
    MTP.begin();
    MTP.addFilesystem(SD, "T-DSP SD");

    Serial.println("MTP responder running.");
    Serial.println("Open File Explorer / Finder — the card should");
    Serial.println("appear as 'T-DSP SD'. Drop your WAV files into");
    Serial.println("/samples/<bank>/ then re-flash the audio firmware.");
}

void loop() {
    // Drive the MTP state machine. This handles enumeration, file
    // listing, reads/writes from the host, and event delivery.
    MTP.loop();

    // 1 Hz heartbeat so the main loop's liveness is visible.
    static uint32_t lastBlinkMs = 0;
    const uint32_t now = millis();
    if (now - lastBlinkMs >= 500) {
        lastBlinkMs = now;
        digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    }
}
