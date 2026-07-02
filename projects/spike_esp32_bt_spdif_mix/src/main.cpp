// spike_esp32_bt_spdif_mix — mix Bluetooth + S/PDIF into the TAC5212 DAC.
//
//   (A) phone --A2DP--> ESP32 (I2S master, 44.1k) --> Teensy SAI2 slave (pin 5)
//         --> AsyncAudioInput<AsyncAudioInputI2S2_16bitslave>  (BT, async-resampled)
//   (B) tone --> S/PDIF OUT (pin 14 optical) --[loopback cable]--> S/PDIF IN (pin 15)
//         --> AsyncAudioInputSPDIF3  (S/PDIF, async-resampled)
//   mix (A)+(B) --> AudioOutputTDM (SAI1) --> TAC5212 DAC --> OUT1/OUT2.
//
// Two async resamplers coexist: each locks a foreign clock (the ESP32 crystal,
// the looped-back S/PDIF crystal) to the Teensy audio clock, so neither source
// scratches. Proven pieces: BT path = spike_esp32_i2s_dac (lib/TDspAsyncI2S);
// S/PDIF path = spike_spdif_alex_dac (lib/Audio AsyncAudioInputSPDIF3).
//
// Wiring (teensy41_digital_audio_board):
//   ESP32 BCLK(GPIO26)->pin4  WS(GPIO16)->pin3  DOUT(GPIO25)->pin5 (SAI2 slave in)
//   TAC5212 DAC <- SAI1 TDM (pins 21/20/23 clocks, 7 = OUT1A data)
//   Optical TOSLINK: pin 14 (S/PDIF OUT) -> pin 15 (S/PDIF IN) loopback cable

#include <Arduino.h>
#include <Wire.h>
#include "tdsp_hw_config.h"
#include <Audio.h>
#include <TAC5212.h>
#include <TDspEsp32.h>
// BT async I2S resampler (ESP32 free-running clock -> Teensy rate). See
// lib/TDspAsyncI2S/async_input.h and project memory reference_async_i2s_resampler.
#include "async_input.h"
#include "input_i2s2_16bit.h"

constexpr int     TAC5212_EN_PIN      = 35;     // shared SHDNZ, active-low
constexpr uint8_t TAC5212_I2C_ADDRESS = 0x51;

// --- Audio graph -----------------------------------------------------------
// Declaration order matters: tdmClk (SAI1 TDM input) is constructed FIRST so it
// owns update_responsibility (an input drives update_all(); a lone output does
// not). It ties the graph's update rate to the codec's SAI1 clock domain. The
// two async inputs and the S/PDIF output run their update() when called but
// never claim update-responsibility.
AudioInputTDM          tdmClk;               // SAI1 TDM clock + update driver
AudioOutputTDM         tdmOut;               // SAI1 TDM -> TAC5212 DAC (ch0=L, ch1=R)

// (B) S/PDIF optical self-test loop. spdifIn is async (never drives update);
// spdifOut sends the tone out the optical port. Same params as spike_spdif_alex_dac.
AsyncAudioInputSPDIF3  spdifIn(false, false, 100, 20, 80);  // optical IN, pin 15
AudioOutputSPDIF3      spdifOut;                            // optical OUT, pin 14
AudioSynthWaveformSine spdifTone;                          // tone sent out the optical port

// (A) Bluetooth: SAI2 slave <- ESP32 I2S (pin 5), async-resampled ESP32 -> Teensy rate.
AsyncAudioInput<AsyncAudioInputI2S2_16bitslave> btIn(false, false, 100, 20, 80);

AudioSynthWaveformSine testTone;             // local DAC self-test source
AudioMixer4            outL, outR;           // mix: 0=BT, 1=local tone, 2=S/PDIF-in
AudioAnalyzePeak       peakBt;               // Bluetooth input level
AudioAnalyzePeak       peakSpdif;            // S/PDIF input level
AudioAnalyzePeak       peakOut;              // signal into the DAC

// tone -> S/PDIF OUT (the loopback source)
AudioConnection c_txL    (spdifTone, 0, spdifOut, 0);
AudioConnection c_txR    (spdifTone, 0, spdifOut, 1);
// mixer input 0 = Bluetooth, 1 = local test tone, 2 = S/PDIF-in
AudioConnection c_btL    (btIn,      0, outL, 0);
AudioConnection c_btR    (btIn,      1, outR, 0);
AudioConnection c_toneL  (testTone,  0, outL, 1);
AudioConnection c_toneR  (testTone,  0, outR, 1);
AudioConnection c_spL    (spdifIn,   0, outL, 2);
AudioConnection c_spR    (spdifIn,   1, outR, 2);
// mix -> DAC (TDM ch0->slot0->OUT1/L, ch1->slot1->OUT2/R)
AudioConnection c_outL   (outL,      0, tdmOut, 0);
AudioConnection c_outR   (outR,      0, tdmOut, 1);
// meters
AudioConnection c_pkBt   (btIn,      0, peakBt,    0);
AudioConnection c_pkSp   (spdifIn,   0, peakSpdif, 0);
AudioConnection c_pkOut  (outL,      0, peakOut,   0);

tac5212::TAC5212 g_codec(Wire);

// ESP32 control/monitor over the no-touch link (Serial7 + EN/IO0). Pins 28/29/
// 36/37 don't overlap the audio pins, so it coexists with the audio graph. Reset
// the ESP32 into its A2DP app on boot and mirror its serial log to USB.
TDspEsp32 esp;
elapsedMillis hb;

static void hardResetCodecPower() {
    pinMode(TAC5212_EN_PIN, OUTPUT);
    digitalWrite(TAC5212_EN_PIN, LOW);  delay(5);
    digitalWrite(TAC5212_EN_PIN, HIGH); delay(10);
}

static bool g_codecOk = false;
static const char *g_codecMsg = "not run";

// Bridge mode: hand the USB serial to the ESP32 (esptool passthrough) so the
// ESP32 can be reflashed through the Teensy. Set by 'b'; cleared by Teensy reset.
static bool g_bridge = false;

// Output volume (dB), applied to both DAC channels. -20 dB default = safe level.
static float g_dvol = -20.0f;
static void applyVol() {
    g_codec.out(1).setDvol(g_dvol);
    g_codec.out(2).setDvol(g_dvol);
}

FLASHMEM static void setupCodec() {
    Serial.println("Init TAC5212 (TDM, HP out)...");
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
    g_codec.setRxChannelSlot(1, 0);              // DAC CH1 (OUT1) <- slot 0 (L)
    g_codec.setRxChannelSlot(2, 1);              // DAC CH2 (OUT2) <- slot 1 (R)
    g_codec.out(1).setMode(tac5212::OutMode::HpDriver);
    g_codec.out(2).setMode(tac5212::OutMode::HpDriver);
    g_codec.out(1).setDvol(-128.0f);             // boot-muted
    g_codec.out(2).setDvol(-128.0f);
    g_codec.setChannelEnable(/*inMask=*/0x0, /*outMask=*/0xC);
    g_codec.powerDac(true);
    delay(100);
    g_codec.setDspAvddSelect(true);
}

// mixer helper: 0=BT, 1=local test tone, 2=S/PDIF-in
static void setMix(float bt, float tone, float spdif) {
    outL.gain(0, bt);   outR.gain(0, bt);
    outL.gain(1, tone); outR.gain(1, tone);
    outL.gain(2, spdif); outR.gain(2, spdif);
}

void setup() {
    hardResetCodecPower();
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);

    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 1500) {}

    Serial.println();
    Serial.println("=== spike_esp32_bt_spdif_mix ===");
    Serial.println("MIX: (A) ESP32 A2DP  +  (B) S/PDIF optical loopback tone  -> TAC5212.");
    Serial.println("Connect a TOSLINK cable pin14(OUT)->pin15(IN). Pair 'T-DSP' and play.");

    // Bring up the ESP32: claim EN/IO0 + Serial7, reset it into its A2DP app.
    esp.begin();
    esp.resetToApp();
    Serial.println("ESP32 reset into app (A2DP). Its serial log follows, prefixed [esp]:");

    Wire.begin();
    tdspMuxAutoSelectCodec(TAC5212_I2C_ADDRESS);
    setupCodec();

    AudioMemory(60);                 // two async resamplers + S/PDIF out + TDM + mixers
    setMix(1.0f, 0.0f, 1.0f);        // default: BT + S/PDIF mixed, local tone off
    testTone.frequency(440.0f);
    testTone.amplitude(0.0f);
    spdifTone.frequency(1000.0f);    // the tone we send out the optical port
    spdifTone.amplitude(0.25f);

    applyVol();                       // unmute to the safe default level

    Serial.println("running -- cmds: t=DACtone a=BT+SPDIF mix  s=SPDIF-only  m=BT-only");
    Serial.println("                 x=toggle SPDIF tone  r=reset ESP32  b=bridge  +/-=vol  d=dump");
}

void loop() {
    // Bridge mode: USB <-> ESP32 passthrough for esptool. Reset the Teensy to exit.
    if (g_bridge) { esp.bridgeTask(Serial); return; }

    if (Serial.available()) {
        int c = Serial.read();
        if (c == 'r') { Serial.println("[cmd] resetting ESP32 -> app"); esp.resetToApp(); }
        else if (c == 'b') {
            Serial.println("[cmd] BRIDGE MODE: USB now talks to the ESP32 (esptool). "
                           "Reset the Teensy to exit.");
            Serial.flush();
            g_bridge = true;
        }
        else if (c == 't') {          // local DAC self-test tone only
            testTone.amplitude(0.4f);
            setMix(0.0f, 1.0f, 0.0f);
            Serial.println("[cmd] local DAC tone 440Hz -> BOTH (BT+SPDIF muted)");
        }
        else if (c == 'a') {          // BT + S/PDIF mix (default)
            testTone.amplitude(0.0f);
            setMix(1.0f, 0.0f, 1.0f);
            Serial.println("[cmd] MIX: BT + S/PDIF");
        }
        else if (c == 's') {          // S/PDIF-in only
            testTone.amplitude(0.0f);
            setMix(0.0f, 0.0f, 1.0f);
            Serial.println("[cmd] S/PDIF-in only");
        }
        else if (c == 'm') {          // Bluetooth only
            testTone.amplitude(0.0f);
            setMix(1.0f, 0.0f, 0.0f);
            Serial.println("[cmd] Bluetooth only");
        }
        else if (c == 'x') {          // toggle the outgoing S/PDIF tone
            static bool on = true; on = !on;
            spdifTone.amplitude(on ? 0.25f : 0.0f);
            Serial.printf("[cmd] S/PDIF out tone %s\n", on ? "ON (0.25)" : "OFF");
        }
        else if (c == '+') { g_dvol += 3.0f; if (g_dvol > 0) g_dvol = 0;
            applyVol(); Serial.printf("[cmd] vol %.0f dB\n", g_dvol); }
        else if (c == '-') { g_dvol -= 3.0f; if (g_dvol < -60) g_dvol = -60;
            applyVol(); Serial.printf("[cmd] vol %.0f dB\n", g_dvol); }
        else if (c == 'd') {          // dump codec RX/out regs
            Serial.printf("[reg] RX_OFF(26)=%02X RX_CH1(28)=%02X RX_CH2(29)=%02X "
                          "CH_EN(76)=%02X PWR(78)=%02X\n",
                          g_codec.readRegister(0, 0x26), g_codec.readRegister(0, 0x28),
                          g_codec.readRegister(0, 0x29), g_codec.readRegister(0, 0x76),
                          g_codec.readRegister(0, 0x78));
        }
        else if (c == 'i') {          // re-init codec
            Serial.println("[cmd] re-init codec");
            setupCodec(); applyVol();
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
        float pbt = peakBt.available()    ? peakBt.read()    : 0.0f;
        float psp = peakSpdif.available() ? peakSpdif.read() : 0.0f;
        float po  = peakOut.available()   ? peakOut.read()   : 0.0f;
        Serial.printf("alive up=%lus  codec=%s(%s)  spdif=%s inFreq=%.0f  "
                      "btPeak=%.3f spdifPeak=%.3f outPeak=%.3f  cpuMax=%.1f%% memMax=%u\n",
                      (unsigned long)(millis() / 1000),
                      g_codecOk ? "OK" : "FAIL", g_codecMsg,
                      AsyncAudioInputSPDIF3::isLocked() ? "LOCKED" : "no-signal",
                      spdifIn.getInputFrequency(),
                      pbt, psp, po,
                      AudioProcessorUsageMax(), AudioMemoryUsageMax());
    }
}
