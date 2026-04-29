// teensy_usb_audio_tac5212 -- USB-host -> F32 -> 32-bit TDM -> TAC5212,
// with internal-loopback capture path back over USB so a host-side DAW can
// FFT the round-trip and characterize jitter / THD / noise floor.
//
//   USB host  -- 16-bit/44.1k  -->  AudioInputUSB
//                                       |
//                           AudioConvert_I16toF32 x2
//                                       |
//                            AudioOutputTDM_F32              setGain(usbIn.volume())
//                                       |
//                            SAI1_TX (8 slots x 32-bit)
//                                       |
//                                    TAC5212 DAC ---> OUT1/OUT2 jack
//                                                          |
//                                       (3.5mm cable loopback to INxP jack)
//                                                          |
//                                    TAC5212 ADC ---  TX slots 0/1
//                                       |
//                            SAI1_RX (8 slots x 32-bit)
//                                       |
//                            AudioInputTDM_F32
//                                       |
//                           AudioConvert_F32toI16 x2
//                                       |
//                            AudioOutputUSB  -- 16-bit/44.1k -->  USB host
//
// The capture path is the natural test rig for the new AudioOutputTDM_F32:
// host plays a known signal, host captures the round-trip, FFT shows what
// the F32->32-bit-TDM->codec->ADC->TDM->F32->USB chain did to it.

#include <Arduino.h>
#include <Wire.h>
#include <Audio.h>
#include <OpenAudio_ArduinoLibrary.h>
#include "tac5212_regs.h"

// SHDNZ for the TAC5212 (and the 6140 ADC, which shares this pin on this
// board). Per project_shdnz_pin: this pin may be toggled exactly ONCE at
// boot; afterward, drivers must use SW_RESET via I2C only.
constexpr int TAC5212_EN_PIN = 35;

// Per project_6140_buffer_contention: the 6140 ADC's '125 buffer is always
// driven on, and it fights the TAC5212 on TDM slots 0..3. The fix is to
// SW-reset the 6140 (returns it to POR sleep state, where it stops driving
// the bus). The 6140 sits at I2C 0x4C.
constexpr uint8_t ADC6140_I2C_ADDR = 0x4C;

// Playback chain (USB host -> codec).
AudioInputUSB           usbIn;
AudioConvert_I16toF32   cnvPlayL;
AudioConvert_I16toF32   cnvPlayR;
AudioOutputTDM_F32      tdmOut;

AudioConnection      patchPlayInL (usbIn, 0, cnvPlayL, 0);
AudioConnection      patchPlayInR (usbIn, 1, cnvPlayR, 0);
AudioConnection_F32  patchPlayOutL(cnvPlayL, 0, tdmOut, 0);
AudioConnection_F32  patchPlayOutR(cnvPlayR, 0, tdmOut, 1);

// Capture chain (codec -> USB host). Mirror image of the playback chain,
// using the new AudioInputTDM_F32 on the SAI RX side.
AudioInputTDM_F32       tdmIn;
AudioConvert_F32toI16   cnvCapL;
AudioConvert_F32toI16   cnvCapR;
AudioOutputUSB          usbOut;

AudioConnection_F32  patchCapInL (tdmIn, 0, cnvCapL, 0);
AudioConnection_F32  patchCapInR (tdmIn, 1, cnvCapR, 0);
AudioConnection      patchCapOutL(cnvCapL, 0, usbOut, 0);
AudioConnection      patchCapOutR(cnvCapR, 0, usbOut, 1);

static void writeReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(TAC5212_I2C_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

static void shutdownAdc6140() {
    Wire.beginTransmission(ADC6140_I2C_ADDR);
    if (Wire.endTransmission() != 0) return;  // not present, nothing to do
    Wire.beginTransmission(ADC6140_I2C_ADDR);
    Wire.write(0x01);  // SW_RESET
    Wire.write(0x01);
    Wire.endTransmission();
    delay(20);
}

static void setupCodec() {
    writeReg(REG_SW_RESET, 0x01);
    delay(100);
    writeReg(REG_SLEEP_CFG, SLEEP_CFG_WAKE);
    delay(100);

    // TDM 32-bit, BCLK inverted, 1 BCLK offset.
    writeReg(REG_PASI_CFG0,    PASI_CFG0_TDM_32_BCLK_INV);
    writeReg(REG_INTF_CFG1,    INTF_CFG1_DOUT_PASI);
    writeReg(REG_INTF_CFG2,    INTF_CFG2_DIN_ENABLE);
    writeReg(REG_PASI_RX_CFG0, PASI_OFFSET_1);
    writeReg(REG_PASI_TX_CFG1, PASI_OFFSET_1);

    // Slot 0 -> RX CH1 (DAC L1 -> OUT1); slot 1 -> RX CH2 (DAC L2 -> OUT2).
    writeReg(REG_RX_CH1_SLOT, slot(0));
    writeReg(REG_RX_CH2_SLOT, slot(1));

    // ADC CH1 -> TX slot 0; ADC CH2 -> TX slot 1. Single-ended INP-only mode
    // so INxM doesn't bleed between channels through the TRS ring.
    writeReg(REG_TX_CH1_SLOT, slot(0));
    writeReg(REG_TX_CH2_SLOT, slot(1));
    writeReg(REG_ADC_CH1_CFG0, ADC_CFG0_SE_INP_ONLY);
    writeReg(REG_ADC_CH2_CFG0, ADC_CFG0_SE_INP_ONLY);

    // OUT1/OUT2: source = DAC, mono SE-positive, headphone driver at 0 dB.
    writeReg(REG_OUT1_CFG0, OUT_SRC_DAC | OUT_ROUTE_MONO_SE_P);
    writeReg(REG_OUT1_CFG1, OUT_CFG1_HP_0DB);
    writeReg(REG_OUT1_CFG2, OUT_CFG1_HP_0DB);
    writeReg(REG_OUT2_CFG0, OUT_SRC_DAC | OUT_ROUTE_MONO_SE_P);
    writeReg(REG_OUT2_CFG1, OUT_CFG1_HP_0DB);
    writeReg(REG_OUT2_CFG2, OUT_CFG1_HP_0DB);

    // Mute boot-gate -- power up silent so PWR_CFG doesn't pop.
    writeReg(REG_DAC_L1_VOL, 0);
    writeReg(REG_DAC_R1_VOL, 0);
    writeReg(REG_DAC_L2_VOL, 0);
    writeReg(REG_DAC_R2_VOL, 0);

    writeReg(REG_CH_EN,   CH_EN_ALL);
    writeReg(REG_PWR_CFG, PWR_CFG_ADC_DAC_MICBIAS);
    delay(100);

    writeReg(REG_DAC_L1_VOL, DAC_VOL_0DB);
    writeReg(REG_DAC_R1_VOL, DAC_VOL_0DB);
    writeReg(REG_DAC_L2_VOL, DAC_VOL_0DB);
    writeReg(REG_DAC_R2_VOL, DAC_VOL_0DB);
}

void setup() {
    pinMode(TAC5212_EN_PIN, OUTPUT);
    digitalWrite(TAC5212_EN_PIN, LOW);
    delay(5);
    digitalWrite(TAC5212_EN_PIN, HIGH);
    delay(10);

    pinMode(LED_BUILTIN, OUTPUT);
    Serial.begin(115200);

    AudioMemory(20);
    AudioMemory_F32(20);

    Wire.begin();
    shutdownAdc6140();
    setupCodec();

    tdmOut.setGain(usbIn.volume());
}

void loop() {
    const uint32_t now = millis();

    // Poll the Windows volume / mute state every 20 ms and apply to the
    // F32 TDM output's outputScale.
    static uint32_t last_vol_check = 0;
    static float    last_vol       = -1.0f;
    if (now - last_vol_check >= 20) {
        last_vol_check = now;
        const float v = usbIn.volume();
        if (v != last_vol) {
            tdmOut.setGain(v);
            last_vol = v;
        }
    }

    // 1 Hz LED heartbeat.
    static uint32_t last_blink = 0;
    if (now - last_blink >= 500) {
        last_blink = now;
        digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
    }
}
