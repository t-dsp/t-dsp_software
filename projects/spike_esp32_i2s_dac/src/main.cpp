// spike_esp32_i2s_dac — hear the ESP32 Bluetooth (A2DP) audio out the TAC5212.
//
//   phone --A2DP--> ESP32 (I2S MASTER, 44.1k) --BCLK/LRCLK/DOUT-->
//       Teensy SAI2 slave (AudioInputI2S2slave, data on pin 5)
//       -> AudioOutputI2S (SAI1 master, 44.1k) -> TAC5212 DAC -> OUT1/OUT2.
//
// Stock int16 Audio @ 44.1k (== A2DP rate) so no resampling is needed here; the
// ESP32 and Teensy 44.1k clocks differ by ~ppm -> slow, harmless drift. This is a
// bring-up spike (prove the path / hear audio); proper async resampling and
// integration into t-dsp_f32_audio_shield come later.
//
// Wiring (teensy41_digital_audio_board, #ESP32_I2S1 header):
//   ESP32 BCLK (GPIO26) -> Teensy pin 4  (BCLK2)   SAI2 bit clock  (slave in)
//   ESP32 WS   (GPIO16) -> Teensy pin 3  (LRCLK2)  SAI2 word clock (slave in)
//   ESP32 DOUT (GPIO25) -> Teensy pin 5  (IN2)     SAI2 RX data
//   TAC5212 DAC          <- Teensy SAI1 (pins 21/20/23 clocks, 7 = OUT1A data)

#include <Arduino.h>
#include <Wire.h>
#include "tdsp_hw_config.h"
#include <Audio.h>
#include <TAC5212.h>
#include <TDspEsp32.h>

constexpr int     TAC5212_EN_PIN      = 35;     // shared SHDNZ, active-low
constexpr uint8_t TAC5212_I2C_ADDRESS = 0x51;

// Declaration order matters: i2sOut (SAI1 master) is constructed first so it wins
// update_responsibility — its stable master clock drives the graph's update_all().
// i2sIn is a SAI2 SLAVE (clocked by the ESP32); it only supplies DMA'd samples.
// SAI1 master-clock + update driver. AudioOutputI2S alone does not drive
// update_all() (only an I2S *input* does). Without a stable driver the graph was
// clocked only by the ESP32's SAI2 (cpuMax=0% until it connected) and the SAI1
// DAC never reliably clocked -> dead silence. Declaring an AudioInputI2S on SAI1
// FIRST makes SAI1 run continuously at 44.1k: it owns update_responsibility (its
// RX DMA ISR drives the graph) and generates MCLK/BCLK/LRCLK for the codec,
// independent of the ESP32. Its RX data (codec ADC on pin 8) is unused.
AudioInputI2S          i2sClk;               // SAI1 master clock + update driver
AudioOutputI2S         i2sOut;               // SAI1 -> TAC5212 DAC
AudioInputI2S2slave    i2sIn;                // SAI2 slave <- ESP32 I2S (pin 5 data)
AudioSynthWaveformSine testTone;             // internal DAC test source
AudioMixer4            outL, outR;           // switch ESP32 audio vs test tone
AudioAnalyzePeak       peakIn;              // ESP32 input level
AudioAnalyzePeak       peakOut;             // signal into the DAC

// slot 0 = ESP32 audio, slot 1 = test tone (gains set in setup / 't' / 'a')
AudioConnection c_inL_mix (i2sIn,    0, outL, 0);
AudioConnection c_inR_mix (i2sIn,    1, outR, 0);
AudioConnection c_toneL   (testTone, 0, outL, 1);
AudioConnection c_toneR   (testTone, 0, outR, 1);
AudioConnection c_outL    (outL,     0, i2sOut, 0);   // -> DAC OUT1
AudioConnection c_outR    (outR,     0, i2sOut, 1);   // -> DAC OUT2
AudioConnection c_pkIn    (i2sIn,    0, peakIn,  0);
AudioConnection c_pkOut   (outL,     0, peakOut, 0);

tac5212::TAC5212 g_codec(Wire);

// ESP32 control/monitor over the no-touch link (Serial7 + EN/IO0). Uses pins
// 28/29/36/37, which do NOT overlap the audio pins (SAI1/SAI2/I2C), so it
// coexists with the audio graph. We reset the ESP32 into its A2DP app on boot
// (in case it came up stuck in download mode) and mirror its serial log to USB.
TDspEsp32 esp;
elapsedMillis hb;

static void hardResetCodecPower() {
    pinMode(TAC5212_EN_PIN, OUTPUT);
    digitalWrite(TAC5212_EN_PIN, LOW);  delay(5);
    digitalWrite(TAC5212_EN_PIN, HIGH); delay(10);
}

// Codec init status, reported in the heartbeat + on the 'i' command.
static bool g_codecOk = false;
static const char *g_codecMsg = "not run";

// Output volume (dB), applied to both DAC channels. -20 dB default = safe level.
static float g_dvol = -20.0f;
static void applyVol() {
    g_codec.out(1).setDvol(g_dvol);
    g_codec.out(2).setDvol(g_dvol);
}

FLASHMEM static void setupCodec() {
    Serial.println("Init TAC5212 (I2S, HP out)...");
    tdspMuxAutoSelectCodec(TAC5212_I2C_ADDRESS);
    tac5212::Result r = g_codec.begin(TAC5212_I2C_ADDRESS);
    g_codecOk = !r.isError();
    g_codecMsg = r.isError() ? (r.message ? r.message : "unknown") : "ok";
    if (r.isError()) { Serial.print("  begin failed: ");
        Serial.println(r.message ? r.message : "(unknown)"); return; }

    tac5212::TAC5212::SerialFormat sf;
    sf.format = tac5212::TAC5212::Format::I2s;   // match AudioOutputI2S framing
    g_codec.setSerialFormat(sf);

    // Board bodge: DIN/DOUT swapped+shorted -> disable codec DOUT so it can't
    // fight the Teensy TX; DIN stays enabled (output-only).
    g_codec.writeRegister(0, /*INTF_CFG1*/ 0x10, 0x00);

    g_codec.setRxSlotOffset(1);
    g_codec.setRxChannelSlot(1, 0);              // DAC CH1 (OUT1) <- I2S left
    g_codec.setRxChannelSlot(2, 1);              // DAC CH2 (OUT2) <- I2S right
    g_codec.out(1).setMode(tac5212::OutMode::HpDriver);
    g_codec.out(2).setMode(tac5212::OutMode::HpDriver);
    g_codec.out(1).setDvol(-128.0f);             // boot-muted
    g_codec.out(2).setDvol(-128.0f);
    g_codec.setChannelEnable(/*inMask=*/0x0, /*outMask=*/0xC);
    g_codec.powerDac(true);
    delay(100);
    g_codec.setDspAvddSelect(true);
}

void setup() {
    hardResetCodecPower();
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);

    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 1500) {}

    Serial.println();
    Serial.println("=== spike_esp32_i2s_dac ===");
    Serial.println("ESP32 A2DP -> SAI2 slave (pin 5) -> TAC5212 DAC. Pair 'T-DSP' and play.");

    // Bring up the ESP32: claim EN/IO0 + Serial7, then reset it into its A2DP
    // app so it advertises "T-DSP" (fixes it being stuck in download mode).
    esp.begin();
    esp.resetToApp();
    Serial.println("ESP32 reset into app (A2DP). Its serial log follows, prefixed [esp]:");

    Wire.begin();
    tdspMuxAutoSelectCodec(TAC5212_I2C_ADDRESS);
    setupCodec();

    AudioMemory(30);
    outL.gain(0, 1.0f); outL.gain(1, 0.0f);   // default: ESP32 audio, tone off
    outR.gain(0, 1.0f); outR.gain(1, 0.0f);
    testTone.frequency(440.0f);
    testTone.amplitude(0.0f);

    applyVol();                       // unmute to the safe default level

    Serial.println("running -- cmds: t=tone l=Ltone p=Rtone a=audio +/-=vol  heartbeat:");
}

void loop() {
    // USB debug commands: 'r' = reset the ESP32 into its app (then watch its
    // [esp] boot log to see whether A2DP inits / crashes / brownout-loops).
    if (Serial.available()) {
        int c = Serial.read();
        if (c == 'r') { Serial.println("[cmd] resetting ESP32 -> app"); esp.resetToApp(); }
        else if (c == 't') {          // test tone -> BOTH channels
            outL.gain(0, 0.0f); outL.gain(1, 1.0f);
            outR.gain(0, 0.0f); outR.gain(1, 1.0f);
            testTone.amplitude(0.4f);
            Serial.println("[cmd] TONE 440Hz -> BOTH (L+R)");
        }
        else if (c == 'l') {          // tone -> LEFT only (i2sOut ch0 / OUT1)
            outL.gain(0, 0.0f); outL.gain(1, 1.0f);
            outR.gain(0, 0.0f); outR.gain(1, 0.0f);
            testTone.amplitude(0.4f);
            Serial.println("[cmd] TONE -> LEFT only (i2sOut ch0 -> OUT1)");
        }
        else if (c == 'p') {          // tone -> RIGHT only (i2sOut ch1 / OUT2)
            outL.gain(0, 0.0f); outL.gain(1, 0.0f);
            outR.gain(0, 0.0f); outR.gain(1, 1.0f);
            testTone.amplitude(0.4f);
            Serial.println("[cmd] TONE -> RIGHT only (i2sOut ch1 -> OUT2)");
        }
        else if (c == '+') { g_dvol += 3.0f; if (g_dvol > 0) g_dvol = 0;
            applyVol(); Serial.printf("[cmd] vol %.0f dB\n", g_dvol); }
        else if (c == '-') { g_dvol -= 3.0f; if (g_dvol < -60) g_dvol = -60;
            applyVol(); Serial.printf("[cmd] vol %.0f dB\n", g_dvol); }
        else if (c == 'a') {          // back to ESP32 audio
            testTone.amplitude(0.0f);
            outL.gain(0, 1.0f); outL.gain(1, 0.0f);
            outR.gain(0, 1.0f); outR.gain(1, 0.0f);
            Serial.println("[cmd] ESP32 audio -> DAC (tone off)");
        }
        else if (c == 'i') {          // re-init codec, report result, unmute
            Serial.println("[cmd] re-init codec");
            setupCodec();
            applyVol();
            Serial.printf("[cmd] codec=%s (%s), vol %.0f dB\n",
                          g_codecOk ? "OK" : "FAIL", g_codecMsg, g_dvol);
        }
    }

    // Mirror the ESP32's UART log to USB, line-buffered with an [esp] prefix.
    static char line[160];
    static size_t n = 0;
    while (esp.uart().available()) {
        char c = (char)esp.uart().read();
        if (c == '\n' || n >= sizeof(line) - 1) {
            line[n] = 0;
            if (n) Serial.printf("[esp] %s\n", line);
            n = 0;
        } else if (c != '\r') {
            line[n++] = c;
        }
    }

    if (hb >= 1000) {
        hb = 0;
        digitalToggle(LED_BUILTIN);
        float pin  = peakIn.available()  ? peakIn.read()  : 0.0f;
        float pout = peakOut.available() ? peakOut.read() : 0.0f;
        Serial.printf("alive up=%lus  codec=%s(%s)  inPeak=%.3f  outPeak=%.3f  cpuMax=%.1f%%  memMax=%u\n",
                      (unsigned long)(millis() / 1000),
                      g_codecOk ? "OK" : "FAIL", g_codecMsg,
                      pin, pout,
                      AudioProcessorUsageMax(),
                      AudioMemoryUsageMax());
    }
}
