// T-DSP WS2812B Status LED Test — Teensy 4.1
//
// Verifies the onboard status WS2812B (SparkFun COM-16347) driven from the
// Teensy through the 74HCT2G17 3.3V->5V buffer.
//
// Signal path (teensy41_digital_audio_board schematic, STATUS LED block):
//   Teensy pin 31 -> TEENSY_LED -> 74HCT2G17 (non-inverting) -> WS2812B DIN
//
// What you should see:
//   1. A one-shot startup sequence: RED, GREEN, BLUE, WHITE (1s each) so you
//      can confirm every channel of the RGB die works.
//   2. Then a continuous smooth rainbow so you can confirm timing is stable.
// Serial (115200) prints the current phase for a sanity cross-check.

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>

// WS2812B data is on Teensy digital pin 31 (schematic net TEENSY_LED).
constexpr uint8_t LED_DATA_PIN = 31;
constexpr uint16_t NUM_PIXELS  = 1;

// GRB is the standard WS2812B byte order; 800 KHz data rate.
Adafruit_NeoPixel strip(NUM_PIXELS, LED_DATA_PIN, NEO_GRB + NEO_KHZ800);

// Keep brightness modest — full-white on a 5V WS2812B is blinding up close.
constexpr uint8_t BRIGHTNESS = 40;  // 0-255

static void showColor(uint8_t r, uint8_t g, uint8_t b, const char* name) {
    Serial.print("  -> ");
    Serial.println(name);
    strip.setPixelColor(0, strip.Color(r, g, b));
    strip.show();
}

void setup() {
    Serial.begin(115200);

    // Onboard Teensy LED as a "code is running" heartbeat, independent of the
    // WS2812 path — helps distinguish "firmware dead" from "LED wiring dead".
    pinMode(LED_BUILTIN, OUTPUT);

    // Wait up to 3s for the serial monitor.
    while (!Serial && millis() < 3000) {
        digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
        delay(100);
    }

    Serial.println("=====================================");
    Serial.println("  T-DSP WS2812B Status LED Test");
    Serial.println("  Data pin: 31  (net TEENSY_LED)");
    Serial.println("=====================================");

    strip.begin();
    strip.setBrightness(BRIGHTNESS);
    strip.clear();
    strip.show();

    // One-shot channel check.
    Serial.println("Startup channel check:");
    showColor(255,   0,   0, "RED");   delay(1000);
    showColor(  0, 255,   0, "GREEN"); delay(1000);
    showColor(  0,   0, 255, "BLUE");  delay(1000);
    showColor(255, 255, 255, "WHITE"); delay(1000);
    Serial.println("Entering rainbow loop...");
}

void loop() {
    // Smooth rainbow via HSV; hue wraps the full 16-bit range.
    static uint16_t hue = 0;

    strip.setPixelColor(0, strip.gamma32(strip.ColorHSV(hue)));
    strip.show();
    hue += 256;  // ~256 steps per full color wheel

    // Heartbeat on the onboard LED so a frozen loop is obvious.
    static unsigned long lastBlink = 0;
    static bool ledState = false;
    if (millis() - lastBlink >= 500) {
        lastBlink = millis();
        ledState = !ledState;
        digitalWrite(LED_BUILTIN, ledState);
    }

    // Live serial heartbeat so the monitor always shows activity, even if you
    // attach after boot (the startup banner only prints once).
    static unsigned long lastPrint = 0;
    if (millis() - lastPrint >= 2000) {
        lastPrint = millis();
        Serial.print("rainbow running  hue=");
        Serial.println(hue);
    }

    delay(10);
}
