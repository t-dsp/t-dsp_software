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

// --- OUTx_CFG0 / OUTx_CFG1 / OUTx_CFG2 bitfield constants ---
//
// Values verified against TAC5212 datasheet SLASF23A §8.1.1.86 (OUTx_CFG0)
// and §8.1.1.87 (OUTx_CFG1). Re-verify if extending the bit layout.
//
// OUTx_CFG0 layout:
//   bits[7:5] OUTx_SRC   — 001b = DAC chain (POR default)
//   bits[4:2] OUTx_CFG   — routing mode (diff / stereo-SE / mono-SE-P / ...)
//   bit [1]   OUTx_VCOM  — 0 = 0.6×VREF, 1 = AVDD/2
//   bit [0]   reserved
//
// OUTx_CFG1/CFG2 layout (per-driver for OUTxP and OUTxM respectively):
//   bits[7:6] OUTxP_DRIVE     — 00 = line 300Ω, 01 = HP 16Ω, 10 = 4Ω, 11 = FD recv
//   bits[5:3] OUTxP_LVL_CTRL  — 100b = 0 dB; MUST stay at 100b outside analog
//                               bypass mode, other values are "Reserved; Don't use"
//   bits[2:0] misc (bypass impedance / bypass cfg / DAC BW mode)

// Source select (bits[7:5] of OUTx_CFG0)
constexpr uint8_t OUT_SRC_DAC         = 0x20;  // 001 << 5 — DAC chain

// Routing mode (bits[4:2] of OUTx_CFG0)
constexpr uint8_t OUT_ROUTE_DIFF      = 0x00;  // 000 << 2
constexpr uint8_t OUT_ROUTE_STEREO_SE = 0x04;  // 001 << 2
constexpr uint8_t OUT_ROUTE_MONO_SE_P = 0x08;  // 010 << 2 — DAC1A+DAC1B summed to OUTxP
constexpr uint8_t OUT_ROUTE_MONO_SE_M = 0x0C;  // 011 << 2

// Driver type (bits[7:6] of OUTx_CFG1/CFG2)
constexpr uint8_t OUT_DRV_LINE        = 0x00;  // 00 — 300Ω line driver
constexpr uint8_t OUT_DRV_HP          = 0x40;  // 01 — 16Ω headphone driver
constexpr uint8_t OUT_DRV_4OHM        = 0x80;  // 10 — 4Ω speaker driver
constexpr uint8_t OUT_DRV_RECEIVER    = 0xC0;  // 11 — FD receiver driver

// Level control (bits[5:3] of OUTx_CFG1/CFG2) — MUST preserve for DAC playback.
constexpr uint8_t OUT_LVL_CTRL_0DB    = 0x20;  // 100 << 3 — POR default, safe value

// Convenience: full byte value for "headphone driver, 0 dB level, other bits = POR".
constexpr uint8_t OUT_CFG1_HP_0DB     = OUT_DRV_HP | OUT_LVL_CTRL_0DB;  // = 0x60

// DAC digital volume register encoding (per datasheet):
//   0   = mute
//   201 = 0 dB (default unity)
//   255 = +27 dB
// Step is 0.5 dB per LSB. Formula: reg = 201 + dB*2.
constexpr uint8_t DAC_VOL_0DB        = 201;
constexpr uint8_t DAC_VOL_MINUS_20DB = 161;  // safe cold-boot level into 16Ω HP

// CH_EN: enable IN1..4 + OUT1..4 (top 4 bits = inputs, low 4 bits = outputs)
constexpr uint8_t CH_EN_ALL = 0xFF;

// PWR_CFG: power up ADC + DAC + MICBIAS (bits 7,6,5)
constexpr uint8_t PWR_CFG_ADC_DAC_MICBIAS = 0xE0;
