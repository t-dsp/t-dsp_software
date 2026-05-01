// Minimal TAC5212 register/value subset for this project. Trimmed copy of the
// fuller table in projects/spike_f32_usb_loopback/src/tac5212_regs.h.
//
// Only the registers needed to wake the codec, route TDM slots 0/1 to OUT1/OUT2,
// power up DAC + analog, and bring DAC volume to 0 dB.

#pragma once
#include <stdint.h>

constexpr uint8_t TAC5212_I2C_ADDR = 0x51;

constexpr uint8_t REG_SW_RESET     = 0x01;
constexpr uint8_t REG_SLEEP_CFG    = 0x02;
constexpr uint8_t REG_INTF_CFG1    = 0x10;
constexpr uint8_t REG_INTF_CFG2    = 0x11;
constexpr uint8_t REG_PASI_CFG0    = 0x1A;
constexpr uint8_t REG_PASI_TX_CFG1 = 0x1C;
constexpr uint8_t REG_TX_CH1_SLOT  = 0x1E;
constexpr uint8_t REG_TX_CH2_SLOT  = 0x1F;
constexpr uint8_t REG_PASI_RX_CFG0 = 0x26;
constexpr uint8_t REG_RX_CH1_SLOT  = 0x28;
constexpr uint8_t REG_RX_CH2_SLOT  = 0x29;
constexpr uint8_t REG_ADC_CH1_CFG0 = 0x50;
constexpr uint8_t REG_ADC_CH2_CFG0 = 0x55;
constexpr uint8_t REG_OUT1_CFG0    = 0x64;
constexpr uint8_t REG_OUT1_CFG1    = 0x65;
constexpr uint8_t REG_OUT1_CFG2    = 0x66;
constexpr uint8_t REG_DAC_L1_VOL   = 0x67;
constexpr uint8_t REG_DAC_R1_VOL   = 0x69;
constexpr uint8_t REG_OUT2_CFG0    = 0x6B;
constexpr uint8_t REG_OUT2_CFG1    = 0x6C;
constexpr uint8_t REG_OUT2_CFG2    = 0x6D;
constexpr uint8_t REG_DAC_L2_VOL   = 0x6E;
constexpr uint8_t REG_DAC_R2_VOL   = 0x70;
constexpr uint8_t REG_CH_EN        = 0x76;
constexpr uint8_t REG_PWR_CFG      = 0x78;

// Wake AVDD>2V, all VDDIO levels OK.
constexpr uint8_t SLEEP_CFG_WAKE = 0x09;

// PASI_CFG0: TDM, 32-bit slot, FSYNC normal, BCLK inverted, recovery on.
constexpr uint8_t PASI_CFG0_TDM_32_BCLK_INV = 0x35;

// INTF_CFG1: DOUT = PASI DOUT, INTF_CFG2: PASI DIN enabled.
constexpr uint8_t INTF_CFG1_DOUT_PASI  = 0x52;
constexpr uint8_t INTF_CFG2_DIN_ENABLE = 0x80;

// 1 BCLK offset (standard for TDM with 1-bit FSYNC width).
constexpr uint8_t PASI_OFFSET_1 = 0x01;

// Slot register encoding: bit 5 = enable, bits 4:0 = slot number.
inline constexpr uint8_t slot(uint8_t n) { return 0x20 | (n & 0x1F); }

// ADC_CHx_CFG0: single-ended INP-only mode (INSRC[7:6] = 10b). INxM is
// "don't care" per the datasheet (Figure 7-19) -- this avoids cross-bleed
// when INxM is tied to the other channel's INP through a TRS ring.
constexpr uint8_t ADC_CFG0_SE_INP_ONLY = 0x80;

// OUTx_CFG0: DAC source, mono single-ended-positive routing.
constexpr uint8_t OUT_SRC_DAC         = 0x20;  // 001 << 5
constexpr uint8_t OUT_ROUTE_MONO_SE_P = 0x08;  // 010 << 2

// OUTx_CFG1/2: 16Ω headphone driver, 0 dB level (safe POR-compatible value).
constexpr uint8_t OUT_CFG1_HP_0DB = 0x60;

// DAC volume: 0 = mute, 201 = 0 dB (0.5 dB steps; 255 = +27 dB).
constexpr uint8_t DAC_VOL_0DB = 201;

// CH_EN: enable IN1..4 + OUT1..4. PWR_CFG: power up ADC + DAC + MICBIAS.
constexpr uint8_t CH_EN_ALL               = 0xFF;
constexpr uint8_t PWR_CFG_ADC_DAC_MICBIAS = 0xE0;
