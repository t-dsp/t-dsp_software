// teensy_usb_audio_tac5212 -- TRUE 24-bit, 48 kHz, F32 end-to-end loopback rig.
//
// Audio graph (F32 throughout, no int16 conversion at any stage):
//
//   USB host  -- 24-bit/48k -->  AudioInputUSB_F32
//                                       |
//                              AudioOutputTDM_F32       (OpenAudio: 8 slots x 32-bit)
//                                       |
//                              SAI1_TX  -->  TAC5212 DAC --> OUT1/OUT2 jack
//                                                                  |
//                                            (3.5mm cable loopback to INxP jack)
//                                                                  |
//                              SAI1_RX  <--  TAC5212 ADC
//                                       |
//                              AudioInputTDM_F32        (OpenAudio: 8 slots x 32-bit)
//                                       |
//                              AudioOutputUSB_F32 -- 24-bit/48k --> USB host
//
// Compared to the 16-bit version this replaces:
//   - AudioInputUSB / AudioOutputUSB (alex6679, int16 over USB) replaced by
//     AudioInputUSB_F32 / AudioOutputUSB_F32 from lib/USB_Audio_F32_24, which
//     keep F32 across the host boundary (int24 on the wire, Q31 in the ring
//     buffer, F32 once it reaches the AudioStream_F32 graph).
//   - AudioConvert_I16toF32 / AudioConvert_F32toI16 are gone -- F32 is the
//     native data type at every stage now.
//   - AudioOutputTDM_F32.setGain() is left at unity; the host volume slider
//     no longer affects output (the F32 USB classes don't expose a feature
//     unit volume control). Test signals come at fixed levels via Python.
//
// What this proves when the loopback measurements stay clean:
//   - The TAC5212 PLL is filtering Teensy clock jitter well enough for
//     24-bit content (jitter would surface above the now-much-lower noise
//     floor that 24-bit affords).
//   - The full F32 chain is bit-correct (no scaling errors / sign flips
//     would survive a null test against the original signal).

#include <Arduino.h>
#include <Wire.h>
#include <OpenAudio_ArduinoLibrary.h>
#include <AudioInputUSB_F32.h>
#include <AudioOutputUSB_F32.h>
#include "tac5212_regs.h"

// SHDNZ for the TAC5212 (and the 6140 ADC, which shares this pin on this
// board). Per project_shdnz_pin: this pin may be toggled exactly ONCE at
// boot; afterward, drivers must use SW_RESET via I2C only.
constexpr int TAC5212_EN_PIN = 35;

// Per project_6140_buffer_contention: the 6140 ADC's '125 buffer is always
// driven on, fights the TAC5212 on TDM slots 0..3. SW-reset returns it to
// POR sleep state.
constexpr uint8_t ADC6140_I2C_ADDR = 0x4C;

// Playback chain: USB host -> codec.
AudioInputUSB_F32       usbIn;
AudioOutputTDM_F32      tdmOut;

// Capture chain: codec -> USB host.
AudioInputTDM_F32       tdmIn;
AudioOutputUSB_F32      usbOut;

// Playback patch: USB chans 0/1 (F32) -> TDM slots 0/1.
AudioConnection_F32  patchPlayL(usbIn, 0, tdmOut, 0);
AudioConnection_F32  patchPlayR(usbIn, 1, tdmOut, 1);

// Capture patch: TDM slots 0/1 -> USB chans 0/1 (F32).
AudioConnection_F32  patchCapL (tdmIn, 0, usbOut, 0);
AudioConnection_F32  patchCapR (tdmIn, 1, usbOut, 1);

static void writeReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(TAC5212_I2C_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

static void shutdownAdc6140() {
    Wire.beginTransmission(ADC6140_I2C_ADDR);
    if (Wire.endTransmission() != 0) return;
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

    writeReg(REG_PASI_CFG0,    PASI_CFG0_TDM_32_BCLK_INV);
    writeReg(REG_INTF_CFG1,    INTF_CFG1_DOUT_PASI);
    writeReg(REG_INTF_CFG2,    INTF_CFG2_DIN_ENABLE);
    writeReg(REG_PASI_RX_CFG0, PASI_OFFSET_1);
    writeReg(REG_PASI_TX_CFG1, PASI_OFFSET_1);

    // Slot 0 -> RX CH1 (DAC L1 -> OUT1); slot 1 -> RX CH2 (DAC L2 -> OUT2).
    writeReg(REG_RX_CH1_SLOT, slot(0));
    writeReg(REG_RX_CH2_SLOT, slot(1));

    // ADC CH1 -> TX slot 0; ADC CH2 -> TX slot 1.
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

    AudioMemory_F32(40);   // bumped: F32 USB rings need a bit more

    Wire.begin();
    shutdownAdc6140();
    setupCodec();

    // No host volume control on the F32 USB classes; leave at unity.
    tdmOut.setGain(1.0f);
}

void loop() {
    // 1 Hz LED heartbeat.
    static uint32_t last_blink = 0;
    const uint32_t now = millis();
    if (now - last_blink >= 500) {
        last_blink = now;
        digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
    }
}
