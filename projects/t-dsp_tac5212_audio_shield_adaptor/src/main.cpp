#include <Arduino.h>
#include <Audio.h>
#include <Wire.h>
#include "tac5212_regs.h"

// Teensy pin that drives EN_HELD_HIGH on the TAC5212 module → enables LDO
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

// Capture mixers — combine line in + PDM mic for USB recording stream
AudioMixer4          captureL;
AudioMixer4          captureR;

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

// USB capture stream: line input + PDM mic mixed into the host recording input
AudioConnection      pc13(tdmIn, 0, captureL, 0);    // line L
AudioConnection      pc14(tdmIn, 2, captureR, 0);    // line R
AudioConnection      pc15(pdmMixL, 0, captureL, 1);  // PDM L
AudioConnection      pc16(pdmMixR, 0, captureR, 1);  // PDM R
AudioConnection      pc17(captureL, 0, usbOut, 0);
AudioConnection      pc18(captureR, 0, usbOut, 1);

// --- Codec I2C helpers ---

static void writeReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(TAC5212_I2C_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

static uint8_t readReg(uint8_t reg) {
    Wire.beginTransmission(TAC5212_I2C_ADDR);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom(TAC5212_I2C_ADDR, (uint8_t)1);
    return Wire.available() ? Wire.read() : 0xFF;
}

// --- Codec setup, split into discrete configuration steps ---

// 1. Software reset, then wake the device.
static void codecResetAndWake() {
    writeReg(REG_SW_RESET, 0x01);
    delay(100);
    writeReg(REG_SLEEP_CFG, SLEEP_CFG_WAKE);
    delay(100);
}

// 2. Configure the audio serial interface: TDM, 32-bit, BCLK inverted, target mode.
static void codecConfigureSerialInterface() {
    writeReg(REG_PASI_CFG0, PASI_CFG0_TDM_32_BCLK_INV);
    writeReg(REG_INTF_CFG1, INTF_CFG1_DOUT_PASI);
    writeReg(REG_INTF_CFG2, INTF_CFG2_DIN_ENABLE);
    writeReg(REG_PASI_RX_CFG0, PASI_OFFSET_1);
    writeReg(REG_PASI_TX_CFG2, PASI_OFFSET_1);
}

// 3. Map TDM slots to DAC inputs (RX, host → codec) and ADC outputs (TX, codec → host).
//    DAC: slot 0 → OUT1 (left ear), slot 1 → OUT2 (right ear)
//    ADC: slot 0 → ADC ch1 (left), slot 1 → ADC ch2 (right)
static void codecConfigureSlotMappings() {
    writeReg(REG_RX_CH1_SLOT, slot(0));  // DAC L1 ← slot 0
    writeReg(REG_RX_CH2_SLOT, slot(1));  // DAC L2 ← slot 1

    writeReg(REG_TX_CH1_SLOT, slot(0));  // ADC ch1 → slot 0
    writeReg(REG_TX_CH2_SLOT, slot(1));  // ADC ch2 → slot 1
}

// 4. Configure the onboard PDM microphone:
//    - GPIO1 generates the PDM clock
//    - GPI1 reads the PDM data line, routed to virtual PDM channels 3+4
//    - TX channels 3+4 transmit those to TDM slots 2+3
static void codecConfigurePdmMic() {
    writeReg(REG_GPIO1_CFG, GPIO1_PDM_CLK);
    writeReg(REG_GPI1_CFG,  GPI1_INPUT);
    writeReg(REG_INTF_CFG4, INTF_CFG4_GPI1_PDM_3_4);
    writeReg(REG_TX_CH3_SLOT, slot(2));  // PDM ch3 → slot 2
    writeReg(REG_TX_CH4_SLOT, slot(3));  // PDM ch4 → slot 3
}

// 5. Set both ADC inputs to single-ended mode reading INxP only.
//    This board ties IN1- to IN2+ to support balanced-mic mode, so we must
//    ignore INxM for line input — otherwise differential mode would cancel
//    a mono signal.
static void codecConfigureAdcInputs() {
    writeReg(REG_ADC_CH1_CFG0, ADC_CFG0_SE_INP_ONLY);
    writeReg(REG_ADC_CH2_CFG0, ADC_CFG0_SE_INP_ONLY);
}

// 6. Configure DAC outputs for stereo headphone use on this board:
//    OUT1P = TRS tip (left channel), OUT2P = TRS ring (right channel),
//    sleeve = GND, OUTxM pins unused. Each output runs in "mono SE at OUTxP"
//    routing mode and uses the 16Ω headphone driver.
//
//    NOTE: writing 0x60 (HP drive + 0 dB level_ctrl) is correct ONLY because
//    the other bits of CFG1/CFG2 are at their POR reset values at this point
//    in setup. A runtime setter that changes driver mode later must use
//    read-modify-write to preserve those bits.
static void codecConfigureDacOutputs() {
    writeReg(REG_OUT1_CFG0, OUT_SRC_DAC | OUT_ROUTE_MONO_SE_P);  // 0x28
    writeReg(REG_OUT1_CFG1, OUT_CFG1_HP_0DB);                    // 0x60
    writeReg(REG_OUT1_CFG2, OUT_CFG1_HP_0DB);                    // OUT1M unused, safe
    writeReg(REG_OUT2_CFG0, OUT_SRC_DAC | OUT_ROUTE_MONO_SE_P);  // 0x28
    writeReg(REG_OUT2_CFG1, OUT_CFG1_HP_0DB);                    // 0x60
    writeReg(REG_OUT2_CFG2, OUT_CFG1_HP_0DB);                    // OUT2M unused, safe
}

// 7. Set all four DAC digital volumes to 0 dB (unity).
//    Windows does its own software attenuation before streaming audio to the
//    USB endpoint, so the user's host volume slider already affects what
//    arrives at the codec. The DAC just passes it through at unity. The HP
//    driver (16 Ω mode) handles the final voltage / current delivery.
//
//    Note: 0 dB (register 201 = 0xC9) is the TAC5212 POR default for all
//    four DAC channels, so this is explicit documentation more than a
//    functional write.
static void codecSetDefaultDacVolume() {
    writeReg(REG_DAC_L1_VOL, DAC_VOL_0DB);
    writeReg(REG_DAC_R1_VOL, DAC_VOL_0DB);
    writeReg(REG_DAC_L2_VOL, DAC_VOL_0DB);
    writeReg(REG_DAC_R2_VOL, DAC_VOL_0DB);
}

// 8. Enable all input/output channels and power up ADC + DAC + MICBIAS.
static void codecPowerUp() {
    writeReg(REG_CH_EN,   CH_EN_ALL);
    writeReg(REG_PWR_CFG, PWR_CFG_ADC_DAC_MICBIAS);
    delay(100);
}

void setupCodec() {
    Serial.println("Initializing TAC5212...");

    codecResetAndWake();
    codecConfigureSerialInterface();
    codecConfigureSlotMappings();
    codecConfigurePdmMic();
    codecConfigureAdcInputs();
    codecConfigureDacOutputs();
    codecSetDefaultDacVolume();
    codecPowerUp();

    Serial.print("DEV_STS0: 0x"); Serial.println(readReg(REG_DEV_STS0), HEX);
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

    Wire.beginTransmission(TAC5212_I2C_ADDR);
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

    // Capture mixers — both line and PDM go to USB recording at unity by default
    captureL.gain(0, 1.0);   // line L
    captureR.gain(0, 1.0);   // line R
    captureL.gain(1, 1.0);   // PDM L
    captureR.gain(1, 1.0);   // PDM R

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

// Host volume scaling.
//
// Teensy's USB Audio Class descriptor declares the playback volume feature
// unit as unsigned 0..0xFFF, while UAC 1.0 expects signed Q8.8 dB. Windows
// interprets this mismatch by squeezing its linear-amplitude slider into a
// tiny corner of the 0..1 float range that `usbIn.volume()` returns — on
// this machine, slider 100% reads as ~0.062 and slider 50% as ~0.028.
//
// USB_VOL_SCALE un-squeezes that by multiplying the observed value by
// 1/0.062 ≈ 16.13 and clamping to [0, 1], giving us a usable linear-
// amplitude gain 0..1 that tracks the host slider.
//
// The scale factor is empirical; if you see `USB host vol (raw)` in the
// `s` status dump max out at a different value on a different machine,
// update this constant.
constexpr float USB_VOL_SCALE = 16.13f;  // 1 / 0.062
float hostVolGain  = 1.0f;  // current scaled host gain, updated from loop()
float lastUsbVolRaw = -1.0f;  // last raw usbIn.volume() value we processed

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
            // USB channel is the product of CLI usbVol and the Windows host
            // slider (hostVolGain). Either one can attenuate independently.
            g = (vol / 100.0f) * hostVolGain;
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
    // Track the Windows host volume slider via usbIn.volume() and apply it
    // to the USB channel's mixer gain. DAC stays at unity; all host-side
    // volume happens in the software mixer.
    float raw = usbIn.volume();
    if (raw != lastUsbVolRaw) {
        lastUsbVolRaw = raw;
        float scaled = raw * USB_VOL_SCALE;
        if (scaled > 1.0f) scaled = 1.0f;
        hostVolGain = scaled;
        float g = (usbVol / 100.0f) * hostVolGain;
        mixL.gain(0, g);
        mixR.gain(0, g);
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
                Serial.print("USB host vol raw: "); Serial.print(usbIn.volume(), 4);
                Serial.print("  scaled: "); Serial.println(hostVolGain, 3);
                Serial.print("Active ch: "); Serial.println(chName(activeCh));
                Serial.print("USB="); Serial.print(usbVol);
                Serial.print("%  MIC="); Serial.print(micVol);
                Serial.print("%  LINE="); Serial.print(lineVol); Serial.println("%");
                Serial.print("DEV_STS0: 0x"); Serial.println(readReg(REG_DEV_STS0), HEX);
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
