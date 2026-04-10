#include <Arduino.h>
#include <Audio.h>
#include <Wire.h>

#define TAC5212_ADDR  0x51
const int TAC5212_EN_PIN = 35;

// --- Audio Objects ---
AudioInputUSB        usbIn;
AudioOutputUSB       usbOut;
AudioInputTDM        tdmIn;
AudioOutputTDM       tdmOut;

// Mixers to combine USB + PDM mic + line in for DAC output
AudioMixer4          mixL;
AudioMixer4          mixR;

// PDM mic combiners (32-bit PDM split across two 16-bit TDM channels)
AudioMixer4          pdmMixL;
AudioMixer4          pdmMixR;

// USB audio → mixer input 0
AudioConnection      pc1(usbIn, 0, mixL, 0);
AudioConnection      pc2(usbIn, 1, mixR, 0);

// PDM mic (TDM slots 2+3 = Teensy ch 4,5,6,7) → PDM mixers → mixer input 1
AudioConnection      pc3(tdmIn, 4, pdmMixL, 0);
AudioConnection      pc4(tdmIn, 5, pdmMixL, 1);
AudioConnection      pc5(tdmIn, 6, pdmMixR, 0);
AudioConnection      pc6(tdmIn, 7, pdmMixR, 1);
AudioConnection      pc7(pdmMixL, 0, mixL, 1);
AudioConnection      pc8(pdmMixR, 0, mixR, 1);

// Line input (ADC) → mixer input 2
AudioConnection      pc11(tdmIn, 0, mixL, 2);      // ADC IN1
AudioConnection      pc12(tdmIn, 2, mixR, 2);      // ADC IN2

// Mixer → TDM out (slot 0 = left, slot 1 = right)
AudioConnection      pc9(mixL, 0, tdmOut, 0);
AudioConnection      pc10(mixR, 0, tdmOut, 2);

// Line input → USB out for recording on the computer
AudioConnection      pc13(tdmIn, 0, usbOut, 0);
AudioConnection      pc14(tdmIn, 2, usbOut, 1);

// --- Codec helpers ---
static void writeReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(TAC5212_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

static uint8_t readReg(uint8_t reg) {
    Wire.beginTransmission(TAC5212_ADDR);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom(TAC5212_ADDR, (uint8_t)1);
    return Wire.available() ? Wire.read() : 0xFF;
}

void setupCodec() {
    Serial.println("Initializing TAC5212...");

    // Reset + wake
    writeReg(0x01, 0x01); delay(100);
    writeReg(0x02, 0x09); delay(100);

    // TDM format, 32-bit, BCLK inverted, bus error recovery
    writeReg(0x1A, 0x35);

    // Target/slave mode
    writeReg(0x55, 0x00);

    // DOUT config + DIN enabled
    writeReg(0x10, 0x52);
    writeReg(0x11, 0x80);

    // RX config: offset=1
    writeReg(0x26, 0x01);

    // DAC slot assignments (verified working stereo)
    writeReg(0x28, 0x20);  // RX CH1 ← slot 0 → OUT1 (left)
    writeReg(0x29, 0x21);  // RX CH2 ← slot 1 → OUT2 (right)

    // TX slot assignments (ADC → Teensy)
    writeReg(0x1E, 0x20);  // TX CH1 → slot 0
    writeReg(0x1F, 0x21);  // TX CH2 → slot 1
    writeReg(0x1D, 0x01);  // TX offset

    // PDM mic
    writeReg(0x0A, 0x41);  // GPIO1 → PDM clock
    writeReg(0x0D, 0x02);  // GPI1 → input
    writeReg(0x13, 0x03);  // GPI1 → PDM channels 3+4
    writeReg(0x20, 0x22);  // TX CH3 → PDM ch3, slot 2
    writeReg(0x21, 0x23);  // TX CH4 → PDM ch4, slot 3

    // ADC input config: SE on INxP only (IN1- is tied to IN2+ on this board)
    writeReg(0x50, 0x80);
    writeReg(0x55, 0x80);

    // Output config: DAC source, differential (verified working stereo)
    writeReg(0x64, 0x20);
    writeReg(0x65, 0x20);
    writeReg(0x66, 0x20);
    writeReg(0x6B, 0x20);
    writeReg(0x6C, 0x20);
    writeReg(0x6D, 0x20);

    // DAC volumes (overridden by USB volume tracking)
    writeReg(0x67, 200);
    writeReg(0x69, 200);
    writeReg(0x6E, 200);
    writeReg(0x70, 200);

    // Enable all channels
    writeReg(0x76, 0xFF);

    // Power up ADC + DAC + MICBIAS
    writeReg(0x78, 0xE0);

    delay(100);
    Serial.print("DEV_STS0: 0x"); Serial.println(readReg(0x79), HEX);
    Serial.println("Codec ready: TDM + PDM + ADC");
}

void setup() {
    pinMode(TAC5212_EN_PIN, OUTPUT);
    digitalWrite(TAC5212_EN_PIN, HIGH);

    Serial.begin(115200);
    pinMode(LED_BUILTIN, OUTPUT);

    while (!Serial && millis() < 3000) {
        digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
        delay(100);
    }

    Serial.println("================================");
    Serial.println("  T-DSP TAC5212 Audio Shield");
    Serial.println("================================");

    delay(100);
    Wire.begin();

    Wire.beginTransmission(TAC5212_ADDR);
    if (Wire.endTransmission() != 0) {
        Serial.println("TAC5212 not found!");
        while (1) { delay(100); }
    }

    setupCodec();
    AudioMemory(64);

    // Mixer gains
    mixL.gain(0, 1.0);   // USB
    mixR.gain(0, 1.0);
    mixL.gain(1, 0.0);   // PDM mic off initially
    mixR.gain(1, 0.0);
    mixL.gain(2, 0.0);   // Line in monitoring off initially
    mixR.gain(2, 0.0);

    // PDM mic boost
    float pdmGain = 16.0;
    pdmMixL.gain(0, pdmGain);
    pdmMixL.gain(1, pdmGain / 65536);
    pdmMixR.gain(0, pdmGain);
    pdmMixR.gain(1, pdmGain / 65536);

    Serial.println("\nReady!");
    Serial.println("  USB audio → DAC (stereo)");
    Serial.println("  PDM mic / line in → USB capture");
    Serial.println("  USB host volume → DAC volume");
    Serial.println("\nCommands:");
    Serial.println("  m        - toggle PDM mic monitor on/off");
    Serial.println("  l        - toggle line input monitor on/off");
    Serial.println("  u<0-100> - USB playback volume");
    Serial.println("  p<0-100> - PDM mic volume (also enables monitor)");
    Serial.println("  i<0-100> - line input volume (also enables monitor)");
    Serial.println("  s        - status");
}

bool micOn = false;
bool lineOn = false;
float lastUsbVolume = -1.0;

void loop() {
    // Track USB host volume → DAC volume
    float vol = usbIn.volume();
    if (vol != lastUsbVolume) {
        lastUsbVolume = vol;
        uint8_t regVal;
        if (vol < 0.01f) {
            regVal = 0;
        } else {
            // Map 0.0-1.0 → -40dB to 0dB (register 121 to 201)
            regVal = (uint8_t)(121 + vol * 80);
        }
        writeReg(0x67, regVal);
        writeReg(0x69, regVal);
        writeReg(0x6E, regVal);
        writeReg(0x70, regVal);
    }

    if (Serial.available()) {
        char cmd = Serial.read();
        switch (cmd) {
            case 'm': case 'M':
                micOn = !micOn;
                mixL.gain(1, micOn ? 1.0 : 0.0);
                mixR.gain(1, micOn ? 1.0 : 0.0);
                Serial.print("Mic: "); Serial.println(micOn ? "ON" : "OFF");
                break;
            case 'l': case 'L':
                lineOn = !lineOn;
                mixL.gain(2, lineOn ? 1.0 : 0.0);
                mixR.gain(2, lineOn ? 1.0 : 0.0);
                Serial.print("Line monitor: "); Serial.println(lineOn ? "ON" : "OFF");
                break;
            case 'u': case 'U': {
                int v = Serial.parseInt();
                float g = constrain(v, 0, 100) / 100.0f;
                mixL.gain(0, g);
                mixR.gain(0, g);
                Serial.print("USB: "); Serial.print(v); Serial.println("%");
                break;
            }
            case 'p': case 'P': {
                int v = Serial.parseInt();
                float g = constrain(v, 0, 100) / 50.0f;  // 100% = 2x gain
                mixL.gain(1, g);
                mixR.gain(1, g);
                micOn = (v > 0);
                Serial.print("Mic vol: "); Serial.print(v); Serial.println("%");
                break;
            }
            case 'i': case 'I': {
                int v = Serial.parseInt();
                float g = constrain(v, 0, 100) / 50.0f;  // 100% = 2x gain
                mixL.gain(2, g);
                mixR.gain(2, g);
                lineOn = (v > 0);
                Serial.print("Line vol: "); Serial.print(v); Serial.println("%");
                break;
            }
            case 's': case 'S':
                Serial.println("\n--- Status ---");
                Serial.print("Audio Mem: ");
                Serial.print(AudioMemoryUsage());
                Serial.print("/"); Serial.println(AudioMemoryUsageMax());
                Serial.print("CPU: "); Serial.print(AudioProcessorUsage(), 2); Serial.println("%");
                Serial.print("USB host vol: "); Serial.println(lastUsbVolume, 3);
                Serial.print("DEV_STS0: 0x"); Serial.println(readReg(0x79), HEX);
                Serial.println("--------------\n");
                break;
        }
    }

    static unsigned long lastBlink = 0;
    if (millis() - lastBlink >= 500) {
        lastBlink = millis();
        digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
    }
}
