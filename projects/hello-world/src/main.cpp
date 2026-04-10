#include <Arduino.h>

// Teensy 4.1 onboard LED
const int LED_PIN = LED_BUILTIN;

void setup() {
    Serial.begin(115200);
    pinMode(LED_PIN, OUTPUT);

    // Wait up to 3 seconds for serial monitor to connect
    while (!Serial && millis() < 3000) {
        // blink while waiting
        digitalWrite(LED_PIN, !digitalRead(LED_PIN));
        delay(100);
    }

    Serial.println("=============================");
    Serial.println("  T-DSP Hello World");
    Serial.println("  Teensy 4.1 is alive!");
    Serial.println("=============================");
}

void loop() {
    static unsigned long lastBlink = 0;
    static bool ledState = false;

    // Blink LED every 500ms
    if (millis() - lastBlink >= 500) {
        lastBlink = millis();
        ledState = !ledState;
        digitalWrite(LED_PIN, ledState);
    }

    // Print heartbeat every 2 seconds
    static unsigned long lastPrint = 0;
    if (millis() - lastPrint >= 2000) {
        lastPrint = millis();
        Serial.print("T-DSP uptime: ");
        Serial.print(millis() / 1000);
        Serial.println(" seconds");
    }
}
