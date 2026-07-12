// t-dsp_spdif_monitor — optical S/PDIF input monitor + bring-up test modes.
//
// Goal (default): optical S/PDIF IN -> optical S/PDIF OUT + TAC5212 DAC line out.
//
// Build modes (platformio.ini envs):
//   default            : monitor  spdifIn -> spdifOut + tdmOut(DAC)
//   -D DAC_TONE_ONLY   : 1 kHz tone -> TAC5212 DAC ONLY. No S/PDIF compiled in
//                        at all (simplest possible DAC test).
//   -D DIRECT_TONE     : 1 kHz tone -> DAC + optical OUT (input bypassed).
//   -D SELFTEST_TONE   : tone -> optical OUT; DAC plays whatever returns on IN
//                        (full optical loop test, needs OUT->IN cable).
//
// Board reality (see lib/TAC5212/BOARD_NOTES.md): codec DIN/DOUT are swapped and
// the pins are shorted, so the codec DOUT is disabled (DAC/output-only), and the
// codec is behind a TCA9544A I2C mux. F32 update_responsibility requires the TDM
// output to be the FIRST hardware object constructed (see project memory).

#include <Arduino.h>
#include <Wire.h>
#include "tdsp_hw_config.h"

#include <OpenAudio_ArduinoLibrary.h>     // AudioSettings_F32, AudioOutputTDM_F32
#include <TAC5212.h>

// SPDIF OUT is compiled in for every mode except DAC_TONE_ONLY — not just for
// the optical signal, but because AudioOutputSPDIF3_F32::begin() sets up the
// audio clock (PLL4) that the TAC5212's PLL locks to. Strip it and the codec
// PLL won't lock -> DAC silent (learned the hard way 2026-06-18).
#if !defined(DAC_TONE_ONLY) && !defined(SPDIF_IN_ONLY)
  #define USE_SPDIF_OUT 1
  #include <output_spdif3_f32.h>          // AudioOutputSPDIF3_F32      (pin 14)
#endif
// SPDIF IN — async resampling input (alex6679's design). This is the correct
// path: it decouples the SPDIF-in clock from the SAI clock by resampling, so it
// coexists with the TDM codec output (the output drives update_all). Earlier
// hangs are suspected to be (1) a double-begin() in setup re-initing the DMA,
// and (2) our vendored resampler being an old snapshot. Fixing (1) first.
#if !defined(DAC_TONE_ONLY) && !defined(DIRECT_TONE)
  #define USE_SPDIF_IN 1
  #include <async_input_spdif3_F32.h>     // AsyncAudioInputSPDIF3_F32, pin 15
#endif

#if defined(DAC_TONE_ONLY) || defined(DIRECT_TONE) || defined(SELFTEST_TONE)
  #define USE_TONE 1
#endif

constexpr int     TAC5212_EN_PIN      = 35;   // shared SHDNZ, active-low
constexpr uint8_t TAC5212_I2C_ADDRESS = 0x51;

AudioSettings_F32   g_audioSettings(AUDIO_SAMPLE_RATE_EXACT, AUDIO_BLOCK_SAMPLES);

// MUST be the first hardware-output object constructed: it wins
// update_responsibility and its SAI-TX ISR drives the whole F32 graph's
// update_all(). If a SPDIF object is constructed first the graph freezes.
//
// I2S_OUT: drive the codec with plain 2-channel I2S instead of 8-slot TDM.
// config_spdif3's clock divisors are documented "compatible with I2S" — TDM
// uses a different frame, and alex6679's async S/PDIF input is documented to
// coexist with an I2S output. Testing whether S/PDIF-RX + I2S avoids the hang
// that S/PDIF-RX + TDM triggers. Same input ports (0/1), so the graph is
// unchanged; only the SAI framing + codec format differ. `tdmOut` name reused.
#ifdef I2S_OUT
  #include <output_i2s_f32.h>
AudioOutputI2S_F32  tdmOut(g_audioSettings);            // SAI1 TX (I2S) -> TAC5212 DAC
#else
AudioOutputTDM_F32  tdmOut(g_audioSettings);            // SAI1 TX (TDM) -> TAC5212 DAC
#endif

#ifdef USE_TONE
AudioSynthWaveformSine_F32 toneGen;
AudioAnalyzePeak_F32       txPeak;
AudioConnection_F32        c_txmeter(toneGen, 0, txPeak, 0);
#endif

#ifdef USE_SPDIF_OUT
AudioOutputSPDIF3_F32      spdifOut(g_audioSettings);   // optical OUT, pin 14 (+audio clock)
#endif
#ifdef USE_SPDIF_IN
AudioAnalyzePeak_F32       rxPeakL, rxPeakR;
AsyncAudioInputSPDIF3_F32  spdifIn(g_audioSettings);   // async resampling (default 100/20/80)
AudioConnection_F32        c_mL(spdifIn, 0, rxPeakL, 0);
AudioConnection_F32        c_mR(spdifIn, 1, rxPeakR, 0);
#endif

// ---- Audio graph wiring per mode -------------------------------------------
#if defined(DAC_TONE_ONLY)
AudioConnection_F32 c_t_dacL(toneGen, 0, tdmOut, 0);
AudioConnection_F32 c_t_dacR(toneGen, 0, tdmOut, 1);
#elif defined(DIRECT_TONE)
AudioConnection_F32 c_t_dacL (toneGen, 0, tdmOut,   0);
AudioConnection_F32 c_t_dacR (toneGen, 0, tdmOut,   1);
AudioConnection_F32 c_t_optL (toneGen, 0, spdifOut, 0);
AudioConnection_F32 c_t_optR (toneGen, 0, spdifOut, 1);
#elif defined(SELFTEST_TONE)
AudioConnection_F32 c_t_optL (toneGen, 0, spdifOut, 0);
AudioConnection_F32 c_t_optR (toneGen, 0, spdifOut, 1);
AudioConnection_F32 c_in_dacL(spdifIn, 0, tdmOut,   0);
AudioConnection_F32 c_in_dacR(spdifIn, 1, tdmOut,   1);
#elif defined(SPDIF_IN_ONLY)
// Real use case: external optical source -> TAC5212 DAC. No SPDIF output at all
// (tests whether the hang was the SPDIF in+out combo vs SPDIF-in + TDM).
AudioConnection_F32 c_in_dacL(spdifIn, 0, tdmOut,   0);
AudioConnection_F32 c_in_dacR(spdifIn, 1, tdmOut,   1);
#else  // monitor
AudioConnection_F32 c_in_optL(spdifIn, 0, spdifOut, 0);
AudioConnection_F32 c_in_optR(spdifIn, 1, spdifOut, 1);
AudioConnection_F32 c_in_dacL(spdifIn, 0, tdmOut,   0);
AudioConnection_F32 c_in_dacR(spdifIn, 1, tdmOut,   1);
#endif

tac5212::TAC5212 g_codec(Wire);
elapsedMillis statusTimer;

static void hardResetCodecPower() {
    pinMode(TAC5212_EN_PIN, OUTPUT);
    digitalWrite(TAC5212_EN_PIN, LOW);  delay(5);
    digitalWrite(TAC5212_EN_PIN, HIGH); delay(10);
}

FLASHMEM static void setupCodec() {
    Serial.println("Initializing TAC5212 (DAC line-out)...");
    tac5212::Result r = g_codec.begin(TAC5212_I2C_ADDRESS);
    if (r.isError()) {
        Serial.print("  TAC5212 begin failed: ");
        Serial.println(r.message ? r.message : "(unknown)");
        return;
    }

    tac5212::TAC5212::SerialFormat sf;          // default TDM/32/FSYNC norm/BCLK inv
#ifdef I2S_OUT
    sf.format = tac5212::TAC5212::Format::I2s;  // match AudioOutputI2S_F32 framing
#endif
    g_codec.setSerialFormat(sf);                // enables DOUT (0x52) + DIN

    // *** BOARD BODGE: DIN/DOUT swapped + shorted -> disable DOUT or it fights
    // the Teensy TX and the DAC stays silent. DIN stays enabled. (Codec is
    // output-only here; ADC unusable.) See lib/TAC5212/BOARD_NOTES.md.
    g_codec.writeRegister(0, /*INTF_CFG1*/ 0x10, 0x00);  // DOUT disabled / Hi-Z

    g_codec.setRxSlotOffset(1);
    g_codec.setTxSlotOffset(1);
    g_codec.setRxChannelSlot(1, 0);             // DAC CH1 (OUT1) <- slot 0
    g_codec.setRxChannelSlot(2, 1);             // DAC CH2 (OUT2) <- slot 1

    g_codec.out(1).setMode(tac5212::OutMode::HpDriver);   // known-good mode
    g_codec.out(2).setMode(tac5212::OutMode::HpDriver);
    g_codec.out(1).setDvol(-128.0f);            // boot-muted
    g_codec.out(2).setDvol(-128.0f);

    g_codec.setChannelEnable(/*inMask=*/0x0, /*outMask=*/0xC);  // OUT_CH1+CH2
    g_codec.powerDac(true);
    delay(100);
    g_codec.setDspAvddSelect(true);
}

// LED breadcrumb: pulse the LED n times (visible groups), survives a USB wedge.
// NOTE: the async input's update() runs from static-init (the output ctors start
// their DMA before setup()), so if update() hangs we'd see NO blips at all
// (dark from power-on). Blips up to N => reached setup step N. All 6 + steady
// blink in loop => fully alive.
static void ledBlip(int n) {
    for (int i = 0; i < n; i++) {
        digitalWriteFast(LED_BUILTIN, HIGH); delay(130);
        digitalWriteFast(LED_BUILTIN, LOW);  delay(170);
    }
    delay(600);
}

void setup() {
    hardResetCodecPower();

    pinMode(LED_BUILTIN, OUTPUT);
    ledBlip(1);                 // reached setup() => static init (ctors+first update) OK

    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 800) { /* brief, no blink */ }

    Serial.println();
    Serial.println("=== t-dsp_spdif_monitor ===");
#if defined(DAC_TONE_ONLY)
    Serial.println("MODE: DAC_TONE_ONLY  1 kHz tone -> TAC5212 DAC only (no S/PDIF)");
#elif defined(DIRECT_TONE)
    Serial.println("MODE: DIRECT_TONE  tone -> DAC + optical OUT");
#elif defined(SELFTEST_TONE)
    Serial.println("MODE: SELFTEST_TONE  tone -> optical OUT; DAC plays optical IN");
#else
    Serial.println("MODE: monitor  optical IN -> optical OUT + TAC5212 DAC");
#endif

    ledBlip(2);                 // before codec/mux
    Wire.begin();
    tdspMuxAutoSelectCodec(TAC5212_I2C_ADDRESS);
    setupCodec();
    ledBlip(3);                 // codec configured

    AudioMemory_F32(60);
    AudioMemory(20);
    ledBlip(4);                 // audio memory set

    // NOTE: do NOT call spdifOut.begin()/spdifIn.begin() here — their
    // constructors already call begin() at static-init. Re-calling it re-runs
    // dma.begin()/config_spdif3() on live peripherals (double-init) — a hang
    // cause and a deviation from alex6679's canonical example (begin in ctor only).
#ifdef USE_TONE
    toneGen.setSampleRate_Hz(AUDIO_SAMPLE_RATE_EXACT);
    toneGen.frequency(1000.0f);
    toneGen.amplitude(0.5f);
    toneGen.begin();
#endif

    g_codec.out(1).setDvol(0.0f);               // unmute (0 dB)
    g_codec.out(2).setDvol(0.0f);

    ledBlip(5);                 // graph/tone configured, about to report
    Serial.println("---- TAC5212 status ----");
    g_codec.dumpStatus(Serial);
    Serial.println("------------------------");
    Serial.println("running -- heartbeat below:");
    ledBlip(6);                 // setup() fully complete -> loop should blink steadily
}

void loop() {
    if (statusTimer >= 1000) {
        statusTimer = 0;
        digitalToggle(LED_BUILTIN);

#ifdef USE_TONE
        float tx = txPeak.available() ? txPeak.read() : -1.0f;
#else
        float tx = -2.0f;
#endif
#ifdef USE_SPDIF_IN
        bool  locked = AsyncAudioInputSPDIF3_F32::isLocked();
        float pl = rxPeakL.available() ? rxPeakL.read() : -1.0f;
        float pr = rxPeakR.available() ? rxPeakR.read() : -1.0f;
        Serial.printf("alive up=%lus  TXtone=%.3f  spdif=%-9s fs=%u RXpeak L=%.3f R=%.3f  cpuMax=%.1f%%\n",
                      (unsigned long)(millis()/1000), tx,
                      locked ? "LOCKED" : "no signal",
                      (unsigned)spdifIn.getInputFrequency(), pl, pr,
                      AudioProcessorUsageMax());
#else
        Serial.printf("alive up=%lus  TXtone=%.3f  cpuMax=%.1f%%\n",
                      (unsigned long)(millis()/1000), tx, AudioProcessorUsageMax());
#endif
    }
}
