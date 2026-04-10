// TAC5212 register map and bit values used by this project.
//
// References:
//   - TI TAC5212 datasheet (TLV320 family)
//   - Houston's control_TAC5112 driver header (compatible register map)
//
// Only the registers we actually touch are defined here.

#pragma once
#include <stdint.h>

// I2C address — 0x51 because the address jumper on this board pulls
// ADDR with a 4.7k to GND. The other valid options are 0x50, 0x52, 0x53.
constexpr uint8_t TAC5212_I2C_ADDR = 0x51;

// --- Register addresses (page 0) ---

constexpr uint8_t REG_SW_RESET     = 0x01;  // Software reset (write 0x01)
constexpr uint8_t REG_SLEEP_CFG    = 0x02;  // Wake/sleep config

constexpr uint8_t REG_GPIO1_CFG    = 0x0A;  // GPIO1 function (we use it as PDM clock)
constexpr uint8_t REG_GPI1_CFG     = 0x0D;  // GPI1 function (we use it as PDM data)

constexpr uint8_t REG_INTF_CFG1    = 0x10;  // DOUT function/drive
constexpr uint8_t REG_INTF_CFG2    = 0x11;  // DIN enable
constexpr uint8_t REG_INTF_CFG4    = 0x13;  // GPIO/GPI → channel routing

constexpr uint8_t REG_PASI_CFG0    = 0x1A;  // ASI format / word length / polarity
constexpr uint8_t REG_PASI_TX_CFG2 = 0x1D;  // TX BCLK offset

constexpr uint8_t REG_TX_CH1_SLOT  = 0x1E;  // TX channel 1 slot assignment
constexpr uint8_t REG_TX_CH2_SLOT  = 0x1F;
constexpr uint8_t REG_TX_CH3_SLOT  = 0x20;
constexpr uint8_t REG_TX_CH4_SLOT  = 0x21;

constexpr uint8_t REG_PASI_RX_CFG0 = 0x26;  // RX BCLK offset
constexpr uint8_t REG_RX_CH1_SLOT  = 0x28;  // RX channel 1 slot → DAC L1 → OUT1
constexpr uint8_t REG_RX_CH2_SLOT  = 0x29;  // RX channel 2 slot → DAC L2 → OUT2

constexpr uint8_t REG_ADC_CH1_CFG0 = 0x50;  // ADC channel 1 input source/coupling
constexpr uint8_t REG_ADC_CH2_CFG0 = 0x55;  // ADC channel 2 input source/coupling

constexpr uint8_t REG_OUT1_CFG0    = 0x64;  // OUT1 source / routing
constexpr uint8_t REG_OUT1_CFG1    = 0x65;  // OUT1P driver
constexpr uint8_t REG_OUT1_CFG2    = 0x66;  // OUT1M driver
constexpr uint8_t REG_DAC_L1_VOL   = 0x67;  // DAC channel L1 digital volume
constexpr uint8_t REG_DAC_R1_VOL   = 0x69;  // DAC channel R1 digital volume

constexpr uint8_t REG_OUT2_CFG0    = 0x6B;  // OUT2 source / routing
constexpr uint8_t REG_OUT2_CFG1    = 0x6C;  // OUT2P driver
constexpr uint8_t REG_OUT2_CFG2    = 0x6D;  // OUT2M driver
constexpr uint8_t REG_DAC_L2_VOL   = 0x6E;  // DAC channel L2 digital volume
constexpr uint8_t REG_DAC_R2_VOL   = 0x70;  // DAC channel R2 digital volume

constexpr uint8_t REG_CH_EN        = 0x76;  // Per-channel input/output enable bits
constexpr uint8_t REG_PWR_CFG      = 0x78;  // Power up ADC / DAC / MICBIAS

constexpr uint8_t REG_DEV_STS0     = 0x79;  // Channel power status (read only)
constexpr uint8_t REG_DEV_STS1     = 0x7A;  // PLL / mode status      (read only)

// --- Bit/value constants ---

// SLEEP_CFG: wake the device with AVDD>2V and all VDDIO levels OK
constexpr uint8_t SLEEP_CFG_WAKE = 0x09;

// PASI_CFG0: TDM format, 32-bit word, BCLK inverted, bus-error recovery
//   bits 7:6 = 00 (TDM), 5:4 = 11 (32-bit), 3 = 0 (FSYNC normal),
//   bit 2 = 1 (BCLK inverted), bit 1 = 0, bit 0 = 1 (recovery)
constexpr uint8_t PASI_CFG0_TDM_32_BCLK_INV = 0x35;

// INTF_CFG1: DOUT = PASI DOUT with active-low/weak-high drive
constexpr uint8_t INTF_CFG1_DOUT_PASI = 0x52;
// INTF_CFG2: PASI DIN enabled
constexpr uint8_t INTF_CFG2_DIN_ENABLE = 0x80;

// PASI_RX_CFG0 / PASI_TX_CFG2: 1 BCLK offset (standard for TDM with 1-bit FSYNC width)
constexpr uint8_t PASI_OFFSET_1 = 0x01;

// TX/RX slot assignment: bit 5 = enable, bits 4:0 = slot number 0..31
constexpr uint8_t SLOT_ENABLE = 0x20;
inline constexpr uint8_t slot(uint8_t n) { return SLOT_ENABLE | (n & 0x1F); }

// GPIO1 = PDM clock output (active high/low drive)
constexpr uint8_t GPIO1_PDM_CLK = 0x41;
// GPI1 = digital input
constexpr uint8_t GPI1_INPUT    = 0x02;
// INTF_CFG4: route GPI1 → PDM channels 3+4
constexpr uint8_t INTF_CFG4_GPI1_PDM_3_4 = 0x03;

// ADC_CHx_CFG0: single-ended on INxP only.
// On this board, IN1- is tied to IN2+ for balanced-mic mode, so we MUST
// ignore INxM in differential mode would cancel a mono line input.
constexpr uint8_t ADC_CFG0_SE_INP_ONLY = 0x80;

// OUTx_CFG0: DAC source, differential output
constexpr uint8_t OUT_CFG0_DAC_DIFF = 0x20;
// OUTxP/OUTxM driver: line output, 0 dB level
constexpr uint8_t OUT_DRV_LINE_0DB  = 0x20;

// DAC digital volume register encoding (per datasheet):
//   0   = mute
//   201 = 0 dB (default unity)
//   255 = +27 dB
// Step is 0.5 dB per LSB. Formula: reg = 201 + dB*2.
constexpr uint8_t DAC_VOL_0DB = 201;

// CH_EN: enable IN1..4 + OUT1..4 (top 4 bits = inputs, low 4 bits = outputs)
constexpr uint8_t CH_EN_ALL = 0xFF;

// PWR_CFG: power up ADC + DAC + MICBIAS (bits 7,6,5)
constexpr uint8_t PWR_CFG_ADC_DAC_MICBIAS = 0xE0;
