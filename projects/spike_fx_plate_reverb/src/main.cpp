// spike_fx_plate_reverb — hexefx F32 FX cost/space bench.
//
// Minimal audio path:  testTone (F32 sine) -> [ONE hexefx effect] -> AudioMixer4_F32
// -> AudioOutputTDM_F32 -> TAC5212 (TDM, 32-bit slots, HP out).  The effect is chosen
// at BUILD time by the SPIKE_FX_* flag the env sets (see platformio.ini). Exactly one
// effect is instantiated, so --gc-sections strips the rest and the size report is that
// effect's real FLASH/RAM cost.
//
// Once per second (after a warm-up) it prints a machine-parseable line:
//   [FXCOST] effect=PLATE cpu=12.3 memI16=3 memF32=21
// tools/fx_cost.py builds every env, reads the size report, captures this line, and
// writes the per-effect cost table (planning/plate-reverb-fx/DESIGN.md §6.5).
//
// Bring-up mirrors firmware/mix-kit (codec init, SHDNZ, mux, TDM-first update order);
// see that tree for the full-featured version. HW: Teensy 4.1 + TAC5212 behind a
// TCA9544A mux, SAI1 TDM (COM4 bench board).

#include <Arduino.h>
#include <Wire.h>
#include <malloc.h>                      // mallinfo() — real RAM2 heap used (effects malloc buffers)
#include <Audio.h>                       // stock int16 pool + AudioProcessorUsageMax()
#include <AudioStream_F32.h>
#include <AudioSettings_F32.h>
#include <OpenAudio_ArduinoLibrary.h>    // AudioOutputTDM_F32, AudioMixer4_F32, sine_F32
#include "tdsp_hw_config.h"              // TCA9544A mux helper (reused from mix-kit include/)
#include "TAC5212.h"

// ---- one-effect selection -------------------------------------------------
// Default to the plate reverb if the env forgot to set a flag.
#if !defined(SPIKE_FX_NONE) && !defined(SPIKE_FX_PLATE) && !defined(SPIKE_FX_SPRING) && \
    !defined(SPIKE_FX_REVERBSC) && !defined(SPIKE_FX_DELAY) && !defined(SPIKE_FX_PHASER)
  #define SPIKE_FX_PLATE 1
#endif

#if   defined(SPIKE_FX_NONE)
  #define SPIKE_FX_NAME "NONE"
#elif defined(SPIKE_FX_PLATE)
  #include "effect_platereverb_F32.h"
  #define SPIKE_FX_NAME "PLATE"
#elif defined(SPIKE_FX_SPRING)
  #include "effect_springreverb_F32.h"
  #define SPIKE_FX_NAME "SPRING"
#elif defined(SPIKE_FX_REVERBSC)
  #include "effect_reverbsc_F32.h"
  #define SPIKE_FX_NAME "REVERBSC"
#elif defined(SPIKE_FX_DELAY)
  #include "effect_delaystereo_F32.h"
  #define SPIKE_FX_NAME "DELAY"
#elif defined(SPIKE_FX_PHASER)
  #include "effect_phaserStereo_F32.h"
  #define SPIKE_FX_NAME "PHASER"
#endif

// ---- HW constants (mix-kit parity) ----------------------------------------
constexpr int     TAC5212_EN_PIN      = 35;     // shared SHDNZ, active-low
constexpr uint8_t TAC5212_I2C_ADDRESS = 0x51;
static float      g_dvol              = -10.0f; // codec analog output level (dB), safe HP level

// ---- audio graph ----------------------------------------------------------
// AudioOutputTDM_F32 MUST be the FIRST audio object constructed: it masters SAI1 and
// owns update_responsibility (project_f32_update_order). Do not reorder.
AudioSettings_F32          g_audio(AUDIO_SAMPLE_RATE_EXACT, AUDIO_BLOCK_SAMPLES);
AudioOutputTDM_F32         tdmOut;              // <-- FIRST
AudioSynthWaveformSine_F32 testTone;
AudioMixer4_F32            outL, outR;          // slot 1 carries the tone/FX (mix-kit map)

tac5212::TAC5212 g_codec(Wire);
static bool      g_codecOk = false;

// ---- effect object + wiring (one block compiled) --------------------------
#if defined(SPIKE_FX_NONE)
AudioConnection_F32 cDryL(testTone, 0, outL, 1);
AudioConnection_F32 cDryR(testTone, 0, outR, 1);

#elif defined(SPIKE_FX_PLATE)
AudioEffectPlateReverb_F32 fx;
AudioConnection_F32 cInL(testTone, 0, fx, 0), cInR(testTone, 0, fx, 1);
AudioConnection_F32 cOutL(fx, 0, outL, 1),    cOutR(fx, 1, outR, 1);
static void fxConfigure() { fx.size(0.7f); fx.diffusion(0.65f); fx.hidamp(0.4f);
                            fx.mix(0.4f); fx.bypass_set(false); }

#elif defined(SPIKE_FX_SPRING)
AudioEffectSpringReverb_F32 fx;
AudioConnection_F32 cInL(testTone, 0, fx, 0), cInR(testTone, 0, fx, 1);
AudioConnection_F32 cOutL(fx, 0, outL, 1),    cOutR(fx, 1, outR, 1);
static void fxConfigure() { fx.time(0.6f); fx.mix(0.4f); fx.bypass_set(false); }

#elif defined(SPIKE_FX_REVERBSC)
AudioEffectReverbSc_F32 fx(/*use_psram=*/true);   // 396 KB delay buf -> PSRAM
AudioConnection_F32 cInL(testTone, 0, fx, 0), cInR(testTone, 0, fx, 1);
AudioConnection_F32 cOutL(fx, 0, outL, 1),    cOutR(fx, 1, outR, 1);
static void fxConfigure() { fx.feedback(0.75f); fx.lowpass(0.5f); fx.mix(0.4f); fx.bypass_set(false); }

#elif defined(SPIKE_FX_DELAY)
AudioEffectDelayStereo_F32 fx(400, /*use_psram=*/false);
AudioConnection_F32 cInL(testTone, 0, fx, 0), cInR(testTone, 0, fx, 1);
AudioConnection_F32 cOutL(fx, 0, outL, 1),    cOutR(fx, 1, outR, 1);
static void fxConfigure() { fx.time(0.5f); fx.feedback(0.45f); fx.mix(0.4f); fx.bypass_set(false); }

#elif defined(SPIKE_FX_PHASER)
AudioEffectPhaserStereo_F32 fx;               // 3 in (2 audio + mod), 2 out
AudioConnection_F32 cInL(testTone, 0, fx, 0), cInR(testTone, 0, fx, 1);
AudioConnection_F32 cOutL(fx, 0, outL, 1),    cOutR(fx, 1, outR, 1);
static void fxConfigure() { fx.stages(6); fx.lfo_rate(0.4f); fx.depth(0.7f);
                            fx.feedback(0.5f); fx.mix(0.6f); fx.bypass_set(false); }
#endif

#if defined(SPIKE_FX_NONE)
static void fxConfigure() {}
#endif

// ---- codec bring-up (DAC path only; mirrors mix-kit setupCodec) -----------
static void hardResetCodecPower() {
    pinMode(TAC5212_EN_PIN, OUTPUT);
    digitalWrite(TAC5212_EN_PIN, LOW);  delay(5);
    digitalWrite(TAC5212_EN_PIN, HIGH); delay(10);
}
static void i2cBusRecover(uint8_t sdaPin = 18, uint8_t sclPin = 19) {
    pinMode(sclPin, INPUT_PULLUP); pinMode(sdaPin, INPUT_PULLUP); delayMicroseconds(10);
    if (digitalRead(sdaPin) == HIGH) return;
    for (int i = 0; i < 9 && digitalRead(sdaPin) == LOW; ++i) {
        pinMode(sclPin, OUTPUT); digitalWrite(sclPin, LOW); delayMicroseconds(5);
        pinMode(sclPin, INPUT_PULLUP); delayMicroseconds(5);
    }
    pinMode(sdaPin, OUTPUT); digitalWrite(sdaPin, LOW); delayMicroseconds(5);
    pinMode(sclPin, INPUT_PULLUP); delayMicroseconds(5);
    pinMode(sdaPin, INPUT_PULLUP); delayMicroseconds(5);
}
FLASHMEM static void setupCodec() {
    tdspMuxAutoSelectCodec(TAC5212_I2C_ADDRESS);
    tac5212::Result r = g_codec.begin(TAC5212_I2C_ADDRESS);
    g_codecOk = !r.isError();
    if (r.isError()) { Serial.printf("[codec] begin failed: %s\n", r.message ? r.message : "?"); return; }
    tac5212::TAC5212::SerialFormat sf;
    sf.format  = tac5212::TAC5212::Format::Tdm;
    sf.wordLen = tac5212::TAC5212::WordLen::Bits32;   // 32-bit slots for AudioOutputTDM_F32
    g_codec.setSerialFormat(sf);
#ifdef TDSP_DIGITAL_AUDIO_BOARD
    g_codec.writeRegister(0, 0x10, 0x00);             // board bodge: disable codec DOUT
#endif
    g_codec.setRxSlotOffset(1);
    g_codec.setRxChannelSlot(1, 0);                   // DAC L <- TDM slot 0
    g_codec.setRxChannelSlot(2, 1);                   // DAC R <- TDM slot 1
    g_codec.out(1).setMode(tac5212::OutMode::HpDriver);
    g_codec.out(2).setMode(tac5212::OutMode::HpDriver);
    g_codec.out(1).setDvol(-128.0f);                  // start muted; unmute after graph is up
    g_codec.out(2).setDvol(-128.0f);
    g_codec.setChannelEnable(/*inMask=*/0x0, /*outMask=*/0xC);   // OUT1/OUT2 only (DAC-only spike)
    g_codec.powerDac(true);
    delay(100);
    g_codec.setDspAvddSelect(true);
}

void setup() {
    hardResetCodecPower();
    Serial.begin(115200);
    uint32_t t0 = millis(); while (!Serial && millis() - t0 < 1500) {}
    Serial.printf("\n[spike_fx] effect=%s  building graph...\n", SPIKE_FX_NAME);

    i2cBusRecover();
    Wire.begin();
    Wire.setClock(100000);
    setupCodec();

    AudioMemory(10);            // int16 pool (codec/convert plumbing touches it)
    AudioMemory_F32(100);       // F32 pool — headroom for the effect's blocks

    outL.gain(1, 1.0f);
    outR.gain(1, 1.0f);
    testTone.frequency(440.0f);
    testTone.amplitude(0.25f);  // low level; wet is audible on reverb/delay/mod
    fxConfigure();

    if (g_codecOk) { g_codec.out(1).setDvol(g_dvol); g_codec.out(2).setDvol(g_dvol); }
    Serial.printf("[spike_fx] up. codec=%s. printing cost once/sec...\n",
                  g_codecOk ? "ok" : "FAIL");
    AudioProcessorUsageMaxReset();
    AudioMemoryUsageMaxReset();
    AudioMemoryUsageMaxReset_F32();
}

void loop() {
    static elapsedMillis t;
    static uint32_t warm = 0;
    if (t >= 1000) {
        t = 0;
        // Skip the first 2 s of warm-up peaks, then report the rolling per-second max.
        // heapRAM2 = bytes malloc'd from the RAM2/OCRAM heap — this is where the hexefx
        // effects put their delay/allpass buffers, which teensy_size's STATIC report can't
        // see. It's the number that actually decides whether an effect fits at runtime.
        if (warm++ >= 2) {
            struct mallinfo mi = mallinfo();
            Serial.printf("[FXCOST] effect=%s cpu=%.1f memI16=%u memF32=%u heapRAM2=%d\n",
                          SPIKE_FX_NAME, AudioProcessorUsageMax(),
                          AudioMemoryUsageMax(), AudioMemoryUsageMax_F32(), mi.uordblks);
        }
        AudioProcessorUsageMaxReset();
        AudioMemoryUsageMaxReset();
        AudioMemoryUsageMaxReset_F32();
    }
}
