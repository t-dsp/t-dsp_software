// TLV320ADC6140 register map (page 0, unless otherwise noted).
//
// Source: TI datasheet SBAS992A (Oct 2019), §8.6.
//
// The datasheet uses P{page}_R{reg} notation. This header exposes only the
// registers and fields the driver touches; add more as needed.

#pragma once

#include <stdint.h>

namespace tlv320adc6140 {

// Default I2C address: MSBs 10011, LSBs from ADDR1/ADDR0 pins. With both
// pulled low (typical strap), 7-bit slave = 0x4C.
constexpr uint8_t kDefaultI2cAddr = 0x4C;

// --- Page control -----------------------------------------------------------
// Register 0 on every page is PAGE_SELECT. Writing here switches the current
// page for all subsequent non-page accesses.
constexpr uint8_t REG_PAGE_SELECT = 0x00;

// --- Page 0: device configuration ------------------------------------------

constexpr uint8_t REG_SW_RESET        = 0x01;  // write 0x01 to reset
constexpr uint8_t REG_SLEEP_CFG       = 0x02;  // sleep/wake + vref qcharge + broadcast
constexpr uint8_t REG_SHDN_CFG        = 0x05;  // shutdown configuration
constexpr uint8_t REG_ASI_CFG0        = 0x07;  // format + word length + polarity + TX fill
constexpr uint8_t REG_ASI_CFG1        = 0x08;  // TX_LSB + TX_KEEPER + TX_OFFSET
constexpr uint8_t REG_ASI_CFG2        = 0x09;  // daisy + ASI_ERR recovery
constexpr uint8_t REG_ASI_CH1         = 0x0B;  // channel 1 slot assignment
constexpr uint8_t REG_ASI_CH2         = 0x0C;
constexpr uint8_t REG_ASI_CH3         = 0x0D;
constexpr uint8_t REG_ASI_CH4         = 0x0E;
constexpr uint8_t REG_ASI_CH5         = 0x0F;
constexpr uint8_t REG_ASI_CH6         = 0x10;
constexpr uint8_t REG_ASI_CH7         = 0x11;
constexpr uint8_t REG_ASI_CH8         = 0x12;
constexpr uint8_t REG_MST_CFG0        = 0x13;  // master/slave
constexpr uint8_t REG_MST_CFG1        = 0x14;
constexpr uint8_t REG_ASI_STS         = 0x15;  // detected FSYNC/BCLK ratio (read only)
constexpr uint8_t REG_CLK_SRC         = 0x16;
constexpr uint8_t REG_PDMCLK_CFG      = 0x1F;
constexpr uint8_t REG_PDMIN_CFG       = 0x20;
constexpr uint8_t REG_GPIO_CFG0       = 0x21;
constexpr uint8_t REG_GPO_CFG0        = 0x22;
constexpr uint8_t REG_GPO_CFG1        = 0x23;
constexpr uint8_t REG_GPO_CFG2        = 0x24;
constexpr uint8_t REG_GPO_CFG3        = 0x25;
constexpr uint8_t REG_GPO_VAL         = 0x29;
constexpr uint8_t REG_GPIO_MON        = 0x2A;
constexpr uint8_t REG_GPI_CFG0        = 0x2B;
constexpr uint8_t REG_GPI_CFG1        = 0x2C;
constexpr uint8_t REG_GPI_MON         = 0x2F;
constexpr uint8_t REG_INT_CFG         = 0x32;
constexpr uint8_t REG_INT_MASK0       = 0x33;
constexpr uint8_t REG_INT_LTCH0       = 0x36;
constexpr uint8_t REG_BIAS_CFG        = 0x3B;  // MICBIAS level + ADC full-scale

// Per-channel config: 5 registers per channel, 5 channels apart.
//   CHx_CFG0 = input type/source, coupling, impedance, DRE enable
//   CHx_CFG1 = channel analog gain (PGA) 0..42 dB
//   CHx_CFG2 = digital volume (DVOL) -100..+27 dB, 0.5 dB
//   CHx_CFG3 = gain calibration (GCAL) ±0.7 dB, 0.1 dB
//   CHx_CFG4 = phase calibration (PCAL) 0..255 modulator cycles
constexpr uint8_t REG_CH1_CFG0        = 0x3C;
constexpr uint8_t REG_CH1_CFG1        = 0x3D;
constexpr uint8_t REG_CH1_CFG2        = 0x3E;
constexpr uint8_t REG_CH1_CFG3        = 0x3F;
constexpr uint8_t REG_CH1_CFG4        = 0x40;
constexpr uint8_t REG_CH2_CFG0        = 0x41;
constexpr uint8_t REG_CH2_CFG1        = 0x42;
constexpr uint8_t REG_CH2_CFG2        = 0x43;
constexpr uint8_t REG_CH2_CFG3        = 0x44;
constexpr uint8_t REG_CH2_CFG4        = 0x45;
constexpr uint8_t REG_CH3_CFG0        = 0x46;
constexpr uint8_t REG_CH3_CFG1        = 0x47;
constexpr uint8_t REG_CH3_CFG2        = 0x48;
constexpr uint8_t REG_CH3_CFG3        = 0x49;
constexpr uint8_t REG_CH3_CFG4        = 0x4A;
constexpr uint8_t REG_CH4_CFG0        = 0x4B;
constexpr uint8_t REG_CH4_CFG1        = 0x4C;
constexpr uint8_t REG_CH4_CFG2        = 0x4D;
constexpr uint8_t REG_CH4_CFG3        = 0x4E;
constexpr uint8_t REG_CH4_CFG4        = 0x4F;

constexpr uint8_t REG_DSP_CFG0        = 0x6B;  // decimation filter + ch summing + HPF
constexpr uint8_t REG_DSP_CFG1        = 0x6C;  // DVOL gang + biquads per ch + DRE/AGC sel
constexpr uint8_t REG_DRE_CFG0        = 0x6D;  // DRE_LVL + DRE_MAXGAIN
constexpr uint8_t REG_AGC_CFG0        = 0x70;  // AGC_LVL + AGC_MAXGAIN

constexpr uint8_t REG_IN_CH_EN        = 0x73;  // input channel enable (bit 7 = ch1)
constexpr uint8_t REG_ASI_OUT_CH_EN   = 0x74;  // ASI output slot enable (bit 7 = ch1)
constexpr uint8_t REG_PWR_CFG         = 0x75;  // MICBIAS/ADC/PLL power + dyn pupd
constexpr uint8_t REG_DEV_STS0        = 0x76;  // per-channel power status (read only)
constexpr uint8_t REG_DEV_STS1        = 0x77;  // device mode status (read only)

// --- Reset / sleep values --------------------------------------------------

constexpr uint8_t SW_RESET_TRIGGER    = 0x01;  // write to REG_SW_RESET
constexpr uint8_t SLEEP_CFG_WAKE_INTERNAL_AREG = 0x81;  // AREG_SELECT=1 + SLEEP_ENZ=1
constexpr uint8_t SLEEP_CFG_WAKE_EXTERNAL_AREG = 0x01;  // AREG_SELECT=0 + SLEEP_ENZ=1

// --- ASI_CFG0 field masks / value constants --------------------------------

// bit[7:6] ASI_FORMAT: 00 TDM, 01 I2S, 10 LJ
constexpr uint8_t ASI_FORMAT_TDM      = 0x00 << 6;
constexpr uint8_t ASI_FORMAT_I2S      = 0x01 << 6;
constexpr uint8_t ASI_FORMAT_LJ       = 0x02 << 6;
// bit[5:4] ASI_WLEN: 00 16b, 01 20b, 10 24b, 11 32b
constexpr uint8_t ASI_WLEN_16         = 0x00 << 4;
constexpr uint8_t ASI_WLEN_20         = 0x01 << 4;
constexpr uint8_t ASI_WLEN_24         = 0x02 << 4;
constexpr uint8_t ASI_WLEN_32         = 0x03 << 4;
// bit[3] FSYNC_POL: 0 default, 1 inverted
constexpr uint8_t ASI_FSYNC_POL_INV   = 0x08;
// bit[2] BCLK_POL: 0 default, 1 inverted
constexpr uint8_t ASI_BCLK_POL_INV    = 0x04;
// bit[1] TX_EDGE: 0 default, 1 half-cycle delayed
constexpr uint8_t ASI_TX_EDGE_DELAY   = 0x02;
// bit[0] TX_FILL: 0 transmit 0 for unused, 1 Hi-Z for unused (shared bus!)
constexpr uint8_t ASI_TX_FILL_HIZ     = 0x01;

// --- ASI_CH slot field -----------------------------------------------------
// bit[6] output-line select (0 = primary SDOUT, 1 = secondary). For our
// shared-bus single-data-pin setup, always 0.
// bits[5:0] slot number 0..63.
constexpr uint8_t ASI_CH_OUTPUT_PRIMARY = 0x00;

// --- CHx_CFG0 fields -------------------------------------------------------
// bit[7]   CHx_INTYP:  0 microphone, 1 line
// bit[6:5] CHx_INSRC:  00 analog diff, 01 analog SE, 10 PDM
// bit[4]   CHx_DC:     0 AC-coupled, 1 DC-coupled
// bit[3:2] CHx_IMP:    00 2.5k, 01 10k, 10 20k
// bit[0]   CHx_DREEN:  0 disabled, 1 enabled (DRE or AGC depending on DSP_CFG1)
constexpr uint8_t CH_CFG0_INTYP_MIC       = 0x00;
constexpr uint8_t CH_CFG0_INTYP_LINE      = 0x80;
constexpr uint8_t CH_CFG0_INSRC_DIFF      = 0x00;
constexpr uint8_t CH_CFG0_INSRC_SE        = 0x20;
constexpr uint8_t CH_CFG0_INSRC_PDM       = 0x40;
constexpr uint8_t CH_CFG0_COUPLING_AC     = 0x00;
constexpr uint8_t CH_CFG0_COUPLING_DC     = 0x10;
constexpr uint8_t CH_CFG0_IMP_2_5K        = 0x00;
constexpr uint8_t CH_CFG0_IMP_10K         = 0x04;
constexpr uint8_t CH_CFG0_IMP_20K         = 0x08;
constexpr uint8_t CH_CFG0_DRE_ENABLE      = 0x01;

// --- CHx_CFG1 (PGA gain) ---------------------------------------------------
// bits[7:2] CHx_GAIN: 0..42 (dB). bits[1:0] reserved (write 0).
// Value = gain_db << 2.

// --- CHx_CFG2 (DVOL) -------------------------------------------------------
// 0 = mute. 1 = -100 dB. 201 = 0 dB. 255 = +27 dB. Step = 0.5 dB.
constexpr uint8_t DVOL_MUTE           = 0;
constexpr uint8_t DVOL_0DB            = 201;

// --- CHx_CFG3 (GCAL) -------------------------------------------------------
// bits[7:4] CHx_GCAL: 0 = -0.8 dB, 8 = 0 dB, 15 = +0.7 dB (0.1 dB steps)
constexpr uint8_t GCAL_0DB            = 0x80;

// --- BIAS_CFG fields -------------------------------------------------------
// bits[6:4] MBIAS_VAL: 000 VREF, 001 VREF*1.096, 110 AVDD
// bits[1:0] ADC_FSCALE: 00 2.75V (2 Vrms diff), 01 2.5V, 10 1.375V
constexpr uint8_t MBIAS_VAL_VREF      = 0x00 << 4;
constexpr uint8_t MBIAS_VAL_1P096     = 0x01 << 4;
constexpr uint8_t MBIAS_VAL_AVDD      = 0x06 << 4;
constexpr uint8_t ADC_FSCALE_2V75     = 0x00;
constexpr uint8_t ADC_FSCALE_2V5      = 0x01;
constexpr uint8_t ADC_FSCALE_1V375    = 0x02;

// --- DSP_CFG0 fields -------------------------------------------------------
// bit[5:4] DECI_FILT: 00 linear phase, 01 low-latency, 10 ultra-low-latency
// bit[3:2] CH_SUM: 00 off, 01 2-ch sum, 10 4-ch sum
// bit[1:0] HPF_SEL: 00 programmable IIR, 01 0.00025*fs, 10 0.002*fs, 11 0.008*fs
constexpr uint8_t DECI_LINEAR         = 0x00 << 4;
constexpr uint8_t DECI_LOW_LATENCY    = 0x01 << 4;
constexpr uint8_t DECI_ULTRA_LL       = 0x02 << 4;
constexpr uint8_t CH_SUM_OFF          = 0x00 << 2;
constexpr uint8_t CH_SUM_2CH          = 0x01 << 2;
constexpr uint8_t CH_SUM_4CH          = 0x02 << 2;
constexpr uint8_t HPF_PROGRAMMABLE    = 0x00;
constexpr uint8_t HPF_CUTOFF_12HZ     = 0x01;  // 0.00025*fs (12 Hz at 48 kHz)
constexpr uint8_t HPF_CUTOFF_96HZ     = 0x02;  // 0.002*fs
constexpr uint8_t HPF_CUTOFF_384HZ    = 0x03;  // 0.008*fs

// --- DSP_CFG1 fields -------------------------------------------------------
constexpr uint8_t DSP_CFG1_DVOL_GANG      = 0x80;
constexpr uint8_t DSP_CFG1_BIQUAD_0       = 0x00 << 5;
constexpr uint8_t DSP_CFG1_BIQUAD_1       = 0x01 << 5;
constexpr uint8_t DSP_CFG1_BIQUAD_2       = 0x02 << 5;  // POR default
constexpr uint8_t DSP_CFG1_BIQUAD_3       = 0x03 << 5;
constexpr uint8_t DSP_CFG1_SOFT_STEP_OFF  = 0x10;
constexpr uint8_t DSP_CFG1_SELECT_AGC     = 0x08;  // else DRE

// --- DRE_CFG0 fields -------------------------------------------------------
// bits[7:4] DRE_LVL: 0 → -12 dB, 1 → -18 dB, ..., 7 → -54 dB (default), 9 → -66 dB
// bits[3:0] DRE_MAXGAIN: 0 → 2 dB, 1 → 4 dB, ..., 11 → 24 dB (default), 14 → 30 dB

// --- AGC_CFG0 fields -------------------------------------------------------
// bits[7:4] AGC_LVL: 0 → -6 dB, ..., 14 → -34 dB (default), 15 → -36 dB
// bits[3:0] AGC_MAXGAIN: 0 → 3 dB, ..., 7 → 24 dB (default), 13 → 42 dB

// --- IN_CH_EN / ASI_OUT_CH_EN ---------------------------------------------
// bit 7 = ch1, bit 6 = ch2, ... bit 0 = ch8.
constexpr uint8_t CH_EN_BIT(uint8_t ch) { return 0x80 >> (ch - 1); }

// --- PWR_CFG --------------------------------------------------------------
constexpr uint8_t PWR_CFG_MICBIAS_PDZ = 0x80;
constexpr uint8_t PWR_CFG_ADC_PDZ     = 0x40;
constexpr uint8_t PWR_CFG_PLL_PDZ     = 0x20;
constexpr uint8_t PWR_CFG_DYN_PUPD    = 0x10;

// --- DEV_STS0 bits (per-channel power, read only) -------------------------
// bit 7 = CH1, ..., bit 4 = CH4 (analog channels)
constexpr uint8_t DEV_STS0_CH_STATUS(uint8_t ch) { return 0x80 >> (ch - 1); }

// --- DEV_STS1 MODE_STS (read only) ----------------------------------------
// bits[7:5] encode current mode: 4 sleep, 6 active-no-rec, 7 active-recording
constexpr uint8_t DEV_STS1_MODE_MASK       = 0xE0;
constexpr uint8_t DEV_STS1_MODE_SLEEP      = 4 << 5;
constexpr uint8_t DEV_STS1_MODE_ACTIVE_IDLE= 6 << 5;
constexpr uint8_t DEV_STS1_MODE_RECORDING  = 7 << 5;

}  // namespace tlv320adc6140
