// Spike: F32 USB stereo loopback through unity mixer to TDM slots 0/1.
//
// M4c state: full F32 audio graph wired:
//
//   USB host  ---24-bit/48k--->  AudioInputUSB_F32  (Q31 ring -> F32)
//                                     |
//                                     +-> AudioMixer4_F32 (unity)  -> AudioOutputTDM_F32 slot 0/1
//                                     |
//                                     +-> AudioOutputUSB_F32  ---24-bit/48k---> USB host  (loopback)
//
// The mixer is a placeholder for future DSP -- DSP blocks drop into
// the slot between usbIn and tdmOut without touching any other wiring.
//
// Loopback to host means the user can confirm round-trip by playing a
// signal from a DAW into the device and recording it back from the
// device input track. Round-trip latency ~ 2 audio blocks plus USB
// jitter (a few ms).

#include <Arduino.h>
#include <Wire.h>
#include <AudioStream_F32.h>
#include <AudioSettings_F32.h>
#include <AudioMixer_F32.h>
#include "AudioOutputTDM_F32.h"
#include "AudioInputUSB_F32.h"
#include "AudioOutputUSB_F32.h"
#include "tac5212_regs.h"

// SHDNZ for the TAC5212 (and the 6140 ADC, which shares this pin on this
// board). Per project_shdnz_pin memory: this pin may be toggled exactly
// ONCE at boot; after that, drivers must use SW_RESET via I2C only.
const int TAC5212_EN_PIN = 35;

AudioInputUSB_F32   usbIn;
AudioOutputUSB_F32  usbOut;
AudioMixer4_F32     mixL;
AudioMixer4_F32     mixR;
AudioOutputTDM_F32  tdmOut;

// USB L -> mixer L -> TDM slot 0 / usbOut L
AudioConnection_F32 patchUsbL_mix   (usbIn,  0, mixL,   0);
AudioConnection_F32 patchUsbL_usbOut(usbIn,  0, usbOut, 0);
AudioConnection_F32 patchMixL_tdm   (mixL,   0, tdmOut, 0);

// USB R -> mixer R -> TDM slot 1 / usbOut R
AudioConnection_F32 patchUsbR_mix   (usbIn,  1, mixR,   0);
AudioConnection_F32 patchUsbR_usbOut(usbIn,  1, usbOut, 1);
AudioConnection_F32 patchMixR_tdm   (mixR,   0, tdmOut, 1);

// Raw-register I2C write to the TAC5212. Mirrors the production project's
// writeReg() helper (projects/t-dsp_tac5212_audio_shield_adaptor/src/main.cpp)
// rather than going through the typed lib/TAC5212 API for boot config.
//
// Why raw writes: production calls this the "Phase 1 hand-rolled path,
// preserved as the working audio config" (production main.cpp:2000) and
// explicitly notes "Never call g_codec.begin() — that would SW-reset the
// chip and wipe the hand-rolled init" (production main.cpp:4503-4506).
// The typed API is mature for runtime register access (used by Tac5212Panel)
// but the hand-rolled register sequence is the validated boot path. The
// spike mirrors it exactly to take the codec config out of the variable set.
static void writeReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(TAC5212_I2C_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

// DAC-only subset of production's setupCodecHandRolled(). Skips:
//   codecConfigurePdmMic       — no PDM mic on the spike
//   codecConfigureAdcInputs    — no analog input capture
//   codecEnableDspAvddSel      — only matters with the DSP limiter on
//   codecApplyEarFatigueDefaults — biquad EQ polish, optional
static void setupCodec() {
    // SW reset + wake from sleep.
    writeReg(REG_SW_RESET, 0x01);
    delay(100);
    writeReg(REG_SLEEP_CFG, SLEEP_CFG_WAKE);
    delay(100);

    // Serial interface: TDM, 32-bit slot, 24-bit data left-justified,
    // BCLK inverted (matches the SAI1 config in AudioOutputTDM_F32).
    // PASI offset = 1 BCLK = standard 1-bit FSYNC width / FSE.
    writeReg(REG_PASI_CFG0,    PASI_CFG0_TDM_32_BCLK_INV);
    writeReg(REG_INTF_CFG1,    INTF_CFG1_DOUT_PASI);
    writeReg(REG_INTF_CFG2,    INTF_CFG2_DIN_ENABLE);
    writeReg(REG_PASI_RX_CFG0, PASI_OFFSET_1);
    writeReg(REG_PASI_TX_CFG1, PASI_OFFSET_1);

    // Slot mappings: TDM slot 0 -> RX CH1 (DAC L), slot 1 -> RX CH2 (DAC R).
    // TX paths kept symmetric in case the user ever wants ADC capture later.
    writeReg(REG_RX_CH1_SLOT, slot(0));
    writeReg(REG_RX_CH2_SLOT, slot(1));
    writeReg(REG_TX_CH1_SLOT, slot(0));
    writeReg(REG_TX_CH2_SLOT, slot(1));

    // OUT1 / OUT2: source = DAC, mono SE-positive routing, headphone driver
    // at 0 dB level. Same config the production project ships.
    writeReg(REG_OUT1_CFG0, OUT_SRC_DAC | OUT_ROUTE_MONO_SE_P);
    writeReg(REG_OUT1_CFG1, OUT_CFG1_HP_0DB);
    writeReg(REG_OUT1_CFG2, OUT_CFG1_HP_0DB);
    writeReg(REG_OUT2_CFG0, OUT_SRC_DAC | OUT_ROUTE_MONO_SE_P);
    writeReg(REG_OUT2_CFG1, OUT_CFG1_HP_0DB);
    writeReg(REG_OUT2_CFG2, OUT_CFG1_HP_0DB);

    // Boot-gate: power up DAC at -inf so we don't pop on PWR_CFG enable.
    writeReg(REG_DAC_L1_VOL, 0);
    writeReg(REG_DAC_R1_VOL, 0);
    writeReg(REG_DAC_L2_VOL, 0);
    writeReg(REG_DAC_R2_VOL, 0);

    // Power up: enable all channel slots, then PWR_CFG = ADC+DAC+MICBIAS.
    writeReg(REG_CH_EN,   CH_EN_ALL);
    writeReg(REG_PWR_CFG, PWR_CFG_ADC_DAC_MICBIAS);
    delay(100);

    // Release the boot-gate to 0 dB. (Production defers this until the synth
    // engines are ready; the spike has no synth engines, so unmute now.)
    writeReg(REG_DAC_L1_VOL, DAC_VOL_0DB);
    writeReg(REG_DAC_R1_VOL, DAC_VOL_0DB);
    writeReg(REG_DAC_L2_VOL, DAC_VOL_0DB);
    writeReg(REG_DAC_R2_VOL, DAC_VOL_0DB);
}

void setup() {
    // SHDNZ one-shot. Powers the codec LDO and brings it out of HW reset.
    // Per project_shdnz_pin memory, this is the ONLY allowed toggle of
    // pin 35 across the whole firmware lifetime.
    pinMode(TAC5212_EN_PIN, OUTPUT);
    digitalWrite(TAC5212_EN_PIN, LOW);
    delay(5);                            // > 100 us SHDNZ low spec, generous
    digitalWrite(TAC5212_EN_PIN, HIGH);
    delay(10);                           // settle internal supplies before I2C

    pinMode(LED_BUILTIN, OUTPUT);
    Serial.begin(115200);
    AudioMemory_F32(32);

    // Production doesn't call Wire.setClock(); leave the bus at the Arduino
    // default (~100 kHz on Teensy 4.x). Verified by grep on the production
    // project — the only setClock matches are unrelated (MIDI clock plumbing).
    Wire.begin();

    Wire.beginTransmission(TAC5212_I2C_ADDR);
    if (Wire.endTransmission() != 0) {
        Serial.println("TAC5212 not found on I2C!");
    } else {
        setupCodec();
        Serial.println("TAC5212 init done.");
    }

    mixL.gain(0, 1.0f);
    mixR.gain(0, 1.0f);
}

void loop() {
    // 1 Hz heartbeat so absence-of-blink = firmware not running.
    static uint32_t last_toggle = 0;
    const uint32_t now = millis();
    if (now - last_toggle >= 500) {
        last_toggle = now;
        digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
    }
}
