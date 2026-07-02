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
// Async I2S input: async-resamples the ESP32's free-running 44.1k I2S stream to
// the Teensy's own 44.1k audio clock, killing the steady scratch from the two
// unsynchronized crystals. Ported from JayShoe/esp32_T4_bt_music_receiver
// (alex6679). See lib/TDspAsyncI2S/async_input.h.
#include "async_input.h"
#include "input_i2s2_16bit.h"

constexpr int     TAC5212_EN_PIN      = 35;     // shared SHDNZ, active-low
constexpr uint8_t TAC5212_I2C_ADDRESS = 0x51;

// Declaration order matters: i2sOut (SAI1 master) is constructed first so it wins
// update_responsibility — its stable master clock drives the graph's update_all().
// i2sIn is a SAI2 SLAVE (clocked by the ESP32); it only supplies DMA'd samples.
// PROVEN-STEREO path = TDM (as in t-dsp_f32_audio_shield), NOT I2S. In I2S the
// TAC5212's 2-phase (LRCLK) handling dropped the right channel (slot 1 dead);
// TDM gives clean per-slot stereo (ch0->slot0->OUT1, ch1->slot1->OUT2).
// AudioInputTDM (SAI1 TDM) is declared first as the stable clock/update driver
// (an input drives update_all(); a lone output does not). Stock int16 TDM packs
// 16 ch x 16-bit, so the codec must be set to WordLen::Bits16 (see setupCodec).
AudioInputTDM          tdmClk;               // SAI1 TDM clock + update driver
AudioOutputTDM         tdmOut;               // SAI1 TDM -> TAC5212 DAC (ch0=L, ch1=R)
// SAI2 slave <- ESP32 I2S (pin 5 data), async-resampled ESP32 rate -> Teensy rate.
// Params match the proven spike_spdif_alex_dac AsyncAudioInputSPDIF3 (same Resampler):
// dither=false, noiseshaping=false, attenuation=100dB, half-filter 20..80.
AsyncAudioInput<AsyncAudioInputI2S2_16bitslave> i2sIn(false, false, 100, 20, 80);
AudioSynthWaveformSine testTone;             // internal DAC test source
AudioMixer4            outL, outR;           // switch ESP32 audio vs test tone
AudioAnalyzePeak       peakIn;              // ESP32 input level
AudioAnalyzePeak       peakOut;             // signal into the DAC

// slot 0 = ESP32 audio, slot 1 = test tone (gains set in setup / 't' / 'a')
AudioConnection c_inL_mix (i2sIn,    0, outL, 0);
AudioConnection c_inR_mix (i2sIn,    1, outR, 0);
AudioConnection c_toneL   (testTone, 0, outL, 1);
AudioConnection c_toneR   (testTone, 0, outR, 1);
AudioConnection c_outL    (outL,     0, tdmOut, 0);   // ch0 -> slot0 -> OUT1 (L)
AudioConnection c_outR    (outR,     0, tdmOut, 1);   // ch1 -> slot1 -> OUT2 (R)
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

// Bridge mode: when true, the USB serial is handed to the ESP32 (esptool
// passthrough + auto-reset) so the ESP32 can be reprogrammed through the Teensy.
// Set by the 'b' command; cleared by resetting the Teensy. Audio keeps running
// (the SAI ISR is independent of loop()).
static bool g_bridge = false;

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
    sf.format  = tac5212::TAC5212::Format::Tdm;      // proven stereo path (f32 shield)
    sf.wordLen = tac5212::TAC5212::WordLen::Bits16;  // match stock 16-bit TDM slots
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
    // Bridge mode: USB <-> ESP32 passthrough for esptool (reflash the ESP32).
    // Reset the Teensy to leave bridge mode and resume normal audio/commands.
    if (g_bridge) { esp.bridgeTask(Serial); return; }

    // USB debug commands: 'r' = reset the ESP32 into its app (then watch its
    // [esp] boot log to see whether A2DP inits / crashes / brownout-loops).
    if (Serial.available()) {
        int c = Serial.read();
        if (c == 'r') { Serial.println("[cmd] resetting ESP32 -> app"); esp.resetToApp(); }
        else if (c == 'b') {
            Serial.println("[cmd] BRIDGE MODE: USB now talks to the ESP32 (esptool). "
                           "Reset the Teensy to exit.");
            Serial.flush();
            g_bridge = true;
        }
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
        // --- live codec RX-slot debug (chasing the dead right channel) ------
        else if (c == 'd') {          // dump the actual programmed RX/out regs
            Serial.printf("[reg] PASI_CFG0(1A)=%02X RX_OFF(26)=%02X "
                          "RX_CH1(28)=%02X RX_CH2(29)=%02X CH_EN(76)=%02X PWR(78)=%02X\n",
                          g_codec.readRegister(0, 0x1A), g_codec.readRegister(0, 0x26),
                          g_codec.readRegister(0, 0x28), g_codec.readRegister(0, 0x29),
                          g_codec.readRegister(0, 0x76), g_codec.readRegister(0, 0x78));
        }
        else if (c >= '0' && c <= '3') {   // OUT2 (RX_CH2) <- slot N; tone on BOTH
            uint8_t n = (uint8_t)(c - '0');
            g_codec.setRxChannelSlot(2, n);
            outL.gain(0, 0.0f); outL.gain(1, 1.0f);  // both slots carry the tone
            outR.gain(0, 0.0f); outR.gain(1, 1.0f);  // (OUT1<-slot0 = left-ear reference)
            testTone.amplitude(0.4f);
            Serial.printf("[cmd] OUT2 <- slot %u ; tone on BOTH -> listen RIGHT ear\n", n);
        }
        else if (c == 'o') {          // cycle RX slot offset 0..3, play RIGHT
            static uint8_t off = 1; off = (uint8_t)((off + 1) & 3);
            g_codec.setRxSlotOffset(off);
            outL.gain(0, 0.0f); outL.gain(1, 0.0f);
            outR.gain(0, 0.0f); outR.gain(1, 1.0f);
            testTone.amplitude(0.4f);
            Serial.printf("[cmd] RX slot offset = %u ; RIGHT-only tone\n", off);
        }
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
