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
    Serial.println("  m              - toggle PDM mic monitor on/off");
    Serial.println("  l              - toggle line input monitor on/off");
    Serial.println("  u / p / i      - select active channel (USB/PDM/line)");
    Serial.println("  u50, p75, etc. - set active channel directly to a value");
    Serial.println("  + / -          - increase/decrease active channel by 5%");
    Serial.println("  Up/Down arrows - same as + / -");
    Serial.println("  s              - status");
}

bool micOn = false;
bool lineOn = false;
float lastUsbVolume = -1.0;

// Active channel for +/- adjustments
enum ActiveCh { CH_NONE, CH_USB, CH_MIC, CH_LINE };
ActiveCh activeCh = CH_NONE;

// Current volume levels (0-100)
int usbVol = 100;
int micVol = 0;
int lineVol = 0;

// Escape sequence state for arrow keys
int escState = 0;  // 0=normal, 1=got ESC, 2=got ESC[

const char* chName(ActiveCh c) {
    switch (c) {
        case CH_USB:  return "USB";
        case CH_MIC:  return "MIC";
        case CH_LINE: return "LINE";
        default:      return "(none)";
    }
}

void applyChannel(ActiveCh ch, int vol) {
    vol = constrain(vol, 0, 100);
    float g;
    switch (ch) {
        case CH_USB:
            usbVol = vol;
            g = vol / 100.0f;
            mixL.gain(0, g);
            mixR.gain(0, g);
            break;
        case CH_MIC:
            micVol = vol;
            g = vol / 50.0f;  // 100% = 2x
            mixL.gain(1, g);
            mixR.gain(1, g);
            micOn = (vol > 0);
            break;
        case CH_LINE:
            lineVol = vol;
            g = vol / 50.0f;
            mixL.gain(2, g);
            mixR.gain(2, g);
            lineOn = (vol > 0);
            break;
        default:
            return;
    }
    Serial.print(chName(ch));
    Serial.print(" = ");
    Serial.print(vol);
    Serial.println("%");
}

int currentVol(ActiveCh ch) {
    switch (ch) {
        case CH_USB:  return usbVol;
        case CH_MIC:  return micVol;
        case CH_LINE: return lineVol;
        default:      return 0;
    }
}

void selectOrSet(ActiveCh ch) {
    activeCh = ch;
    // Peek for a number after the letter
    delay(2);
    if (Serial.available() && isDigit(Serial.peek())) {
        int v = Serial.parseInt();
        applyChannel(ch, v);
    } else {
        Serial.print("Active channel: ");
        Serial.print(chName(ch));
        Serial.print(" (");
        Serial.print(currentVol(ch));
        Serial.println("%)");
    }
}

void bumpActive(int delta) {
    if (activeCh == CH_NONE) {
        Serial.println("No active channel — press u, p, or i first.");
        return;
    }
    applyChannel(activeCh, currentVol(activeCh) + delta);
}

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

    while (Serial.available()) {
        char cmd = Serial.read();

        // Arrow key escape sequence handling: ESC [ A/B/C/D
        if (escState == 1) {
            if (cmd == '[') { escState = 2; continue; }
            escState = 0;
        } else if (escState == 2) {
            escState = 0;
            if (cmd == 'A') { bumpActive(+5); continue; }   // Up
            if (cmd == 'B') { bumpActive(-5); continue; }   // Down
            continue;  // ignore other CSI codes
        }
        if (cmd == 0x1B) { escState = 1; continue; }

        switch (cmd) {
            case 'm': case 'M':
                micOn = !micOn;
                mixL.gain(1, micOn ? (micVol > 0 ? micVol / 50.0f : 1.0f) : 0.0f);
                mixR.gain(1, micOn ? (micVol > 0 ? micVol / 50.0f : 1.0f) : 0.0f);
                if (micOn && micVol == 0) micVol = 50;
                Serial.print("Mic: "); Serial.println(micOn ? "ON" : "OFF");
                break;
            case 'l': case 'L':
                lineOn = !lineOn;
                mixL.gain(2, lineOn ? (lineVol > 0 ? lineVol / 50.0f : 1.0f) : 0.0f);
                mixR.gain(2, lineOn ? (lineVol > 0 ? lineVol / 50.0f : 1.0f) : 0.0f);
                if (lineOn && lineVol == 0) lineVol = 50;
                Serial.print("Line monitor: "); Serial.println(lineOn ? "ON" : "OFF");
                break;
            case 'u': case 'U': selectOrSet(CH_USB); break;
            case 'p': case 'P': selectOrSet(CH_MIC); break;
            case 'i': case 'I': selectOrSet(CH_LINE); break;
            case '+': case '=':
                bumpActive(+5);
                break;
            case '-': case '_':
                bumpActive(-5);
                break;
            case 's': case 'S':
                Serial.println("\n--- Status ---");
                Serial.print("Audio Mem: ");
                Serial.print(AudioMemoryUsage());
                Serial.print("/"); Serial.println(AudioMemoryUsageMax());
                Serial.print("CPU: "); Serial.print(AudioProcessorUsage(), 2); Serial.println("%");
                Serial.print("USB host vol: "); Serial.println(lastUsbVolume, 3);
                Serial.print("Active ch: "); Serial.println(chName(activeCh));
                Serial.print("USB="); Serial.print(usbVol);
                Serial.print("%  MIC="); Serial.print(micVol);
                Serial.print("%  LINE="); Serial.print(lineVol); Serial.println("%");
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
