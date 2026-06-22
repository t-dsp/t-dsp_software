// spike_spdif_alex_dac — full optical loop into the TAC5212 DAC.
//
//   tone -> S/PDIF OUT (pin 14, optical) --[loopback cable]--> S/PDIF IN (pin 15)
//        -> AsyncAudioInputSPDIF3 (alex6679 current) -> AudioOutputI2S -> SAI1
//        -> TAC5212 DAC (I2S, HpDriver) -> OUT1/OUT2 (headphones / amp)
//
// Stock int16 Audio only. The S/PDIF receiver + SAI coexist here because the
// async input is alex's current code (the old vendored snapshot was the hang).
// Codec runs I2S 2-ch (stereo monitor doesn't need the 8-slot TDM). The board's
// DIN/DOUT are swapped+shorted, so the codec DOUT is disabled (output-only).

#include <Arduino.h>
#include <Wire.h>
#include "tdsp_hw_config.h"
#include <Audio.h>
#include <TAC5212.h>

constexpr int     TAC5212_EN_PIN      = 35;     // shared SHDNZ, active-low
constexpr uint8_t TAC5212_I2C_ADDRESS = 0x51;

// Declaration order: spdifIn (async, never drives update) first; spdifOut next
// becomes the update driver + provides the S/PDIF clock; then i2sOut + tone.
AsyncAudioInputSPDIF3   spdifIn(false, false, 100, 20, 80);  // optical IN, pin 15
AudioOutputSPDIF3       spdifOut;                            // optical OUT, pin 14 (+clock+update)
AudioOutputI2S          i2sOut;                              // SAI1 -> TAC5212 DAC
AudioSynthWaveformSine  tone1;                               // self-test source

AudioConnection c_tone_l(tone1,   0, spdifOut, 0);   // tone out optical (loop source)
AudioConnection c_tone_r(tone1,   0, spdifOut, 1);
AudioConnection c_in_l  (spdifIn, 0, i2sOut,   0);   // received audio -> codec DAC
AudioConnection c_in_r  (spdifIn, 1, i2sOut,   1);

tac5212::TAC5212 g_codec(Wire);
elapsedMillis hb;

static void hardResetCodecPower() {
    pinMode(TAC5212_EN_PIN, OUTPUT);
    digitalWrite(TAC5212_EN_PIN, LOW);  delay(5);
    digitalWrite(TAC5212_EN_PIN, HIGH); delay(10);
}

FLASHMEM static void setupCodec() {
    Serial.println("Init TAC5212 (I2S, line/HP out)...");
    tac5212::Result r = g_codec.begin(TAC5212_I2C_ADDRESS);
    if (r.isError()) { Serial.print("  begin failed: ");
        Serial.println(r.message ? r.message : "(unknown)"); return; }

    tac5212::TAC5212::SerialFormat sf;
    sf.format = tac5212::TAC5212::Format::I2s;   // match AudioOutputI2S framing
    g_codec.setSerialFormat(sf);

    // Board bodge: DIN/DOUT swapped+shorted -> disable codec DOUT so it can't
    // fight the Teensy TX; DIN stays enabled (output-only). See BOARD_NOTES.md.
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
    Serial.println("=== spike_spdif_alex_dac ===");
    Serial.println("tone -> opt OUT -> [loop] -> opt IN -> TAC5212 DAC");

    Wire.begin();
    tdspMuxAutoSelectCodec(TAC5212_I2C_ADDRESS);
    setupCodec();

    AudioMemory(40);                 // generous for alex's resampler + I2S/SPDIF
    tone1.frequency(1000.0f);
    tone1.amplitude(0.4f);       // AudioSynthWaveformSine is always a sine; no begin()

    g_codec.out(1).setDvol(0.0f);    // unmute (0 dB)
    g_codec.out(2).setDvol(0.0f);

    Serial.println("running -- heartbeat below:");
}

void loop() {
    if (hb >= 1000) {
        hb = 0;
        digitalToggle(LED_BUILTIN);
        Serial.printf("alive up=%lus  spdif=%s  inFreq=%.0f  cpuMax=%.1f%%  memMax=%u\n",
                      (unsigned long)(millis() / 1000),
                      AsyncAudioInputSPDIF3::isLocked() ? "LOCKED" : "no-signal",
                      spdifIn.getInputFrequency(),
                      AudioProcessorUsageMax(),
                      AudioMemoryUsageMax());
    }
}
