// TAC5212 register addresses and bitfield constants.
//
// Private to the lib/TAC5212/ implementation. Values verified against
// TI TAC5212 datasheet SLASF23A (December 2023, revised January 2025).
//
// Register section references in comments point at the datasheet section
// numbers (§8.1.1.xx) so future edits can cross-check without re-grepping
// the PDF. If you add a new register here, verify it against the datasheet
// and cite the section.
//
// Organization: one namespace per register, containing the address and the
// field shift/mask constants for that register. This keeps the field names
// scoped and avoids a flat dump of macros.

#pragma once

#include <stdint.h>

namespace tac5212 {
namespace reg {

// --- Paging (register 0x00 on every page) -----------------------------------

constexpr uint8_t PAGE_SELECT = 0x00;

// --- Software reset / power / sleep -----------------------------------------

constexpr uint8_t SW_RESET     = 0x01;  // bit[0] = self-clearing reset trigger

namespace sw_reset {
    constexpr uint8_t MASK_TRIGGER = 0x01;
}

// DEV_MISC_CFG (originally labeled "SLEEP_CFG" in earlier scratch code).
// §8.1.1 family — controls sleep/wake and VREF/AVDD/IOVDD modes.
constexpr uint8_t DEV_MISC_CFG = 0x02;

namespace dev_misc_cfg {
    constexpr uint8_t MASK_SLEEP_ENZ      = 0x01;  // 1 = active, 0 = sleep
    constexpr uint8_t MASK_IOVDD_IO_MODE  = 0x02;
    constexpr uint8_t MASK_AVDD_MODE      = 0x04;
    constexpr uint8_t MASK_SLEEP_EXIT_VREF = 0x08;
    constexpr uint8_t MASK_VREF_QCHG      = 0x30;  // bits[5:4]

    // Empirically used default in main.cpp: 0x09 = SLEEP_ENZ | SLEEP_EXIT_VREF_EN
    // (active mode with VREF kept alive on sleep exit).
    constexpr uint8_t VAL_WAKE = 0x09;
}

// --- GPIO / GPI --------------------------------------------------------------

constexpr uint8_t GPIO1_CFG = 0x0A;
constexpr uint8_t GPI1_CFG  = 0x0D;

namespace gpio1_cfg {
    constexpr uint8_t SHIFT_FUNC = 4;
    constexpr uint8_t MASK_FUNC  = 0xF0;
    constexpr uint8_t MASK_DRIVE = 0x07;  // bits[2:0]

    // Function codes (bits[7:4])
    constexpr uint8_t FUNC_DISABLED      = 0x00;
    constexpr uint8_t FUNC_GENERAL_IN    = 0x01;
    constexpr uint8_t FUNC_GENERAL_OUT   = 0x02;
    constexpr uint8_t FUNC_IRQ_OUT       = 0x03;
    constexpr uint8_t FUNC_PDM_CLK       = 0x04;

    // Drive codes (bits[2:0])
    constexpr uint8_t DRIVE_HIZ                = 0x00;
    constexpr uint8_t DRIVE_ACTIVE_HIGH_LOW    = 0x01;
    constexpr uint8_t DRIVE_ACTIVE_LOW_WEAK_HI = 0x02;
}

namespace gpi1_cfg {
    constexpr uint8_t MASK_ENABLE = 0x02;  // bit[1]
}

// --- Audio serial interface / TDM slot routing -------------------------------

constexpr uint8_t INTF_CFG1    = 0x10;
constexpr uint8_t INTF_CFG2    = 0x11;
constexpr uint8_t INTF_CFG4    = 0x13;

constexpr uint8_t PASI_CFG0    = 0x1A;

namespace pasi_cfg0 {
    constexpr uint8_t SHIFT_FORMAT  = 6;
    constexpr uint8_t MASK_FORMAT   = 0xC0;  // 00=TDM 01=I2S 10=LJ
    constexpr uint8_t SHIFT_WORDLEN = 4;
    constexpr uint8_t MASK_WORDLEN  = 0x30;  // 00=16 01=20 10=24 11=32
    constexpr uint8_t MASK_FSYNC_POL= 0x08;
    constexpr uint8_t MASK_BCLK_POL = 0x04;
    constexpr uint8_t MASK_BUS_ERR  = 0x02;
    constexpr uint8_t MASK_BUS_ERR_RCOV = 0x01;

    constexpr uint8_t FORMAT_TDM  = 0x00;
    constexpr uint8_t FORMAT_I2S  = 0x40;
    constexpr uint8_t FORMAT_LJ   = 0x80;
    constexpr uint8_t WORDLEN_16  = 0x00;
    constexpr uint8_t WORDLEN_20  = 0x10;
    constexpr uint8_t WORDLEN_24  = 0x20;
    constexpr uint8_t WORDLEN_32  = 0x30;
}

constexpr uint8_t PASI_TX_CFG2 = 0x1D;  // TX BCLK offset
constexpr uint8_t PASI_RX_CFG0 = 0x26;  // RX BCLK offset

constexpr uint8_t TX_CH1_SLOT  = 0x1E;
constexpr uint8_t TX_CH2_SLOT  = 0x1F;
constexpr uint8_t TX_CH3_SLOT  = 0x20;
constexpr uint8_t TX_CH4_SLOT  = 0x21;

constexpr uint8_t RX_CH1_SLOT  = 0x28;
constexpr uint8_t RX_CH2_SLOT  = 0x29;
constexpr uint8_t RX_CH3_SLOT  = 0x2A;
constexpr uint8_t RX_CH4_SLOT  = 0x2B;

namespace slot_cfg {
    constexpr uint8_t MASK_ENABLE = 0x20;  // bit[5]
    constexpr uint8_t MASK_SLOT   = 0x1F;  // bits[4:0]
}

// --- VREF / MICBIAS (0x4D, §8.1.1.65) ----------------------------------------
//
// CRITICAL: VREF_MICBIAS_CFG packs three fields in one byte. Any setter that
// writes a sub-field MUST read-modify-write and validate the resulting
// combination against the allowed set from Table 7-16. Invalid combos return
// Result::error to the caller.

constexpr uint8_t VREF_MICBIAS_CFG = 0x4D;

namespace vref_micbias_cfg {
    constexpr uint8_t SHIFT_EN_MBIAS_GPIO = 6;
    constexpr uint8_t MASK_EN_MBIAS_GPIO  = 0xC0;  // bits[7:6]
    constexpr uint8_t MASK_MICBIAS_LDO_GAIN = 0x10; // bit[4]
    constexpr uint8_t SHIFT_MICBIAS_VAL   = 2;
    constexpr uint8_t MASK_MICBIAS_VAL    = 0x0C;  // bits[3:2]
    constexpr uint8_t SHIFT_VREF_FSCALE   = 0;
    constexpr uint8_t MASK_VREF_FSCALE    = 0x03;  // bits[1:0]

    // EN_MBIAS_GPIO encodings (bits[7:6]):
    //   00 = disabled (I2C controls MICBIAS)
    //   01 = GPIO1 controls MICBIAS
    //   10 = GPIO2 controls MICBIAS
    //   11 = GPI1 controls MICBIAS
    constexpr uint8_t EN_MBIAS_GPIO_DISABLED = 0x00;

    // VREF_FSCALE encoding (bits[1:0]):
    //   00 = 2.75 V VREF (2 Vrms differential / 1 Vrms SE full-scale)
    //   01 = 2.5 V VREF
    //   10 = 1.375 V VREF
    //   11 = reserved
    constexpr uint8_t VREF_FSCALE_2p75 = 0x00;
    constexpr uint8_t VREF_FSCALE_2p5  = 0x01;
    constexpr uint8_t VREF_FSCALE_1p375 = 0x02;

    // MICBIAS_VAL encoding (bits[3:2]):
    //   00 = same as VREF output
    //   01 = 0.5 * VREF output
    //   10 = reserved
    //   11 = same as AVDD (bypass)
    constexpr uint8_t MICBIAS_VAL_SAME_AS_VREF = 0x00;
    constexpr uint8_t MICBIAS_VAL_HALF_VREF    = 0x04;
    constexpr uint8_t MICBIAS_VAL_AVDD         = 0x0C;
}

// --- ADC channel configuration (§8.1.1.68 / §8.1.1.73) -----------------------
//
// Each ADC channel has CFG0, CFG2, CFG3, CFG4. Register 0x51 is IADC_CH_CFG,
// NOT ADC_CH1_CFG1 — it belongs to the instrumentation-ADC subsystem. Channel 2
// follows at +0x05 offset from channel 1.

constexpr uint8_t ADC_CH1_CFG0 = 0x50;
constexpr uint8_t ADC_CH1_CFG2 = 0x52;  // DVOL, NOT exposed by library
constexpr uint8_t ADC_CH1_CFG3 = 0x53;  // fine gain trim, NOT exposed
constexpr uint8_t ADC_CH1_CFG4 = 0x54;  // phase cal, NOT exposed
constexpr uint8_t ADC_CH2_CFG0 = 0x55;
constexpr uint8_t ADC_CH2_CFG2 = 0x57;
constexpr uint8_t ADC_CH2_CFG3 = 0x58;
constexpr uint8_t ADC_CH2_CFG4 = 0x59;

namespace adc_cfg0 {
    constexpr uint8_t SHIFT_INSRC  = 6;
    constexpr uint8_t MASK_INSRC   = 0xC0;
    constexpr uint8_t SHIFT_IMP    = 4;
    constexpr uint8_t MASK_IMP     = 0x30;
    constexpr uint8_t SHIFT_CM_TOL = 2;
    constexpr uint8_t MASK_CM_TOL  = 0x0C;
    constexpr uint8_t MASK_FULLSCALE = 0x02;  // bit[1]
    constexpr uint8_t MASK_BW_MODE   = 0x01;  // bit[0]

    // INSRC[7:6]
    constexpr uint8_t INSRC_DIFFERENTIAL = 0x00;
    constexpr uint8_t INSRC_SE_INP       = 0x40;  // INxP as signal, INxM as ground reference
    constexpr uint8_t INSRC_SE_MUX_INP   = 0x80;  // INxP mux only
    constexpr uint8_t INSRC_SE_MUX_INM   = 0xC0;  // INxM mux only

    // IMP[5:4] — input impedance
    constexpr uint8_t IMP_5K  = 0x00;
    constexpr uint8_t IMP_10K = 0x10;
    constexpr uint8_t IMP_40K = 0x20;

    // CM_TOL[3:2] — common-mode tolerance / coupling
    constexpr uint8_t CM_TOL_AC               = 0x00;
    constexpr uint8_t CM_TOL_DC_LOW           = 0x04;  // 500 mVpp SE / 1 Vpp diff
    constexpr uint8_t CM_TOL_DC_RAIL_TO_RAIL  = 0x08;  // high CMRR mode

    // FULLSCALE_VAL[1] — only meaningful when VREF = 2.75 V
    constexpr uint8_t FULLSCALE_2VRMS = 0x00;
    constexpr uint8_t FULLSCALE_4VRMS = 0x02;  // requires high-CMRR + 24 kHz BW

    // BW_MODE[0]
    constexpr uint8_t BW_24K_AUDIO = 0x00;
    constexpr uint8_t BW_96K_WIDE  = 0x01;  // requires 40k impedance
}

// --- ADC digital volume (§8.1.1.69 / §8.1.1.74) -----------------------------
//
// ADC_CHx_CFG2 is the per-channel digital volume (DVOL) register. Encoding:
//   0       = mute
//   1..200  = -100.0 dB to -0.5 dB  (0.5 dB steps)
//   201     = 0.0 dB (POR default, unity)
//   202..255= +0.5 dB to +27.0 dB   (0.5 dB steps)
// Formula:  reg = 201 + round(dB * 2)
//           dB  = (reg - 201) * 0.5
namespace adc_dvol {
    constexpr uint8_t MUTE        = 0;
    constexpr uint8_t UNITY_0DB   = 201;
    constexpr uint8_t MAX_PLUS_27 = 255;

    constexpr float   DB_MIN      = -100.0f;
    constexpr float   DB_MAX      =  +27.0f;
    constexpr float   DB_STEP     =    0.5f;

    // Convert dB to register value (clamps to valid range).
    inline constexpr uint8_t fromDb(float dB) {
        if (dB <= -100.5f) return MUTE;  // treat anything below -100 as mute
        int reg = 201 + static_cast<int>(dB * 2.0f + (dB >= 0 ? 0.5f : -0.5f));
        if (reg < 1)   reg = 1;    // 0 is mute, 1 is -100 dB
        if (reg > 255)  reg = 255;
        return static_cast<uint8_t>(reg);
    }

    // Convert register value to dB.
    inline constexpr float toDb(uint8_t reg) {
        if (reg == 0) return -120.0f;  // mute sentinel
        return (static_cast<float>(reg) - 201.0f) * 0.5f;
    }
}

// --- Chip-global DSP configuration (§8.1.1.100) ------------------------------
//
// DSP_CFG0 holds chip-global ADC DSP settings — decimation filter, HPF mode,
// biquad count, soft-step disable, and channel-gang. The HPF setting applies
// to BOTH ADC channels at once per datasheet §7.3.x, so it's exposed on the
// library as a chip-global method (TAC5212::setAdcHpf), not on AdcChannel.

constexpr uint8_t DSP_CFG0 = 0x72;

namespace dsp_cfg0 {
    constexpr uint8_t SHIFT_DECI_FILT = 6;
    constexpr uint8_t MASK_DECI_FILT  = 0xC0;  // bits[7:6]
    constexpr uint8_t SHIFT_HPF_SEL   = 4;
    constexpr uint8_t MASK_HPF_SEL    = 0x30;  // bits[5:4]
    constexpr uint8_t SHIFT_BQ_CFG    = 2;
    constexpr uint8_t MASK_BQ_CFG     = 0x0C;  // bits[3:2]
    constexpr uint8_t MASK_DISABLE_SOFT_STEP = 0x02;  // bit[1]
    constexpr uint8_t MASK_DVOL_GANG  = 0x01;  // bit[0]

    // ADC_DSP_HPF_SEL encodings (bits[5:4]):
    //   00 = Programmable first-order IIR; default coefficients in pages
    //        10-11 are the all-pass filter. Used as "HPF off" since the
    //        default behavior is signal pass-through.
    //   01 = HPF with -3 dB cutoff at 0.00002 * fS  (=   1 Hz at 48 kHz)
    //   10 = HPF with -3 dB cutoff at 0.00025 * fS  (=  12 Hz at 48 kHz)
    //   11 = HPF with -3 dB cutoff at 0.002   * fS  (=  96 Hz at 48 kHz)
    constexpr uint8_t HPF_SEL_PROGRAMMABLE = 0x00;  // 00 << 4 — default all-pass = "off"
    constexpr uint8_t HPF_SEL_1HZ          = 0x10;  // 01 << 4
    constexpr uint8_t HPF_SEL_12HZ         = 0x20;  // 10 << 4 — typical audio HPF
    constexpr uint8_t HPF_SEL_96HZ         = 0x30;  // 11 << 4
}

// --- DAC output drivers (§8.1.1.86 / §8.1.1.87 / §8.1.1.88) ------------------

constexpr uint8_t OUT1_CFG0 = 0x64;
constexpr uint8_t OUT1_CFG1 = 0x65;
constexpr uint8_t OUT1_CFG2 = 0x66;
constexpr uint8_t OUT2_CFG0 = 0x6B;
constexpr uint8_t OUT2_CFG1 = 0x6C;
constexpr uint8_t OUT2_CFG2 = 0x6D;

namespace out_cfg0 {
    constexpr uint8_t SHIFT_SRC = 5;
    constexpr uint8_t MASK_SRC  = 0xE0;  // bits[7:5]
    constexpr uint8_t SHIFT_ROUTE = 2;
    constexpr uint8_t MASK_ROUTE  = 0x1C; // bits[4:2]
    constexpr uint8_t MASK_VCOM   = 0x02; // bit[1]

    // SRC[7:5]
    constexpr uint8_t SRC_DAC            = 0x20;  // 001
    constexpr uint8_t SRC_BYPASS         = 0x40;  // 010
    constexpr uint8_t SRC_DAC_AND_BYPASS = 0x60;  // 011

    // ROUTE[4:2]
    constexpr uint8_t ROUTE_DIFF        = 0x00;  // 000
    constexpr uint8_t ROUTE_STEREO_SE   = 0x04;  // 001  DAC1A->OUTxP, DAC1B->OUTxM
    constexpr uint8_t ROUTE_MONO_SE_P   = 0x08;  // 010  DAC1A+DAC1B summed at OUTxP
    constexpr uint8_t ROUTE_MONO_SE_M   = 0x0C;  // 011  DAC1A+DAC1B summed at OUTxM
    constexpr uint8_t ROUTE_PSEUDO_DIFF_VCOM_M = 0x10;  // 100
}

namespace out_cfg1 {
    // Same layout applies to CFG2 for OUTxM driver.
    constexpr uint8_t SHIFT_DRIVE = 6;
    constexpr uint8_t MASK_DRIVE  = 0xC0;  // bits[7:6]
    constexpr uint8_t SHIFT_LVL   = 3;
    constexpr uint8_t MASK_LVL    = 0x38;  // bits[5:3] — MUST stay at 100b (0 dB)
                                           //   outside analog bypass mode

    // DRIVE[7:6]
    constexpr uint8_t DRIVE_LINE      = 0x00;  // 00  line driver, min 300 Ohm load
    constexpr uint8_t DRIVE_HEADPHONE = 0x40;  // 01  HP driver,   min 16 Ohm load
    constexpr uint8_t DRIVE_4OHM      = 0x80;  // 10  4 Ohm speaker driver
    constexpr uint8_t DRIVE_FD_RECEIVER = 0xC0; // 11 FD receiver

    // LVL[5:3] — only valid in analog bypass mode. In DAC playback it MUST
    // be 100b (0 dB) or the field enters "Reserved; Don't use" territory.
    constexpr uint8_t LVL_0DB_SAFE = 0x20;  // 100 << 3
}

// --- Channel enable / power / status -----------------------------------------

constexpr uint8_t CH_EN   = 0x76;
namespace ch_en {
    // Top nibble = input channels (IN_CH1..4), bottom nibble = output channels
    constexpr uint8_t MASK_IN_CH1  = 0x80;
    constexpr uint8_t MASK_IN_CH2  = 0x40;
    constexpr uint8_t MASK_IN_CH3  = 0x20;  // PDM channel
    constexpr uint8_t MASK_IN_CH4  = 0x10;  // PDM channel
    constexpr uint8_t MASK_OUT_CH1 = 0x08;
    constexpr uint8_t MASK_OUT_CH2 = 0x04;
    constexpr uint8_t MASK_OUT_CH3 = 0x02;
    constexpr uint8_t MASK_OUT_CH4 = 0x01;
}

constexpr uint8_t PWR_CFG = 0x78;
namespace pwr_cfg {
    constexpr uint8_t MASK_ADC_PDZ     = 0x80;  // bit[7]
    constexpr uint8_t MASK_DAC_PDZ     = 0x40;  // bit[6]
    constexpr uint8_t MASK_MICBIAS_PDZ = 0x20;  // bit[5] — the ONE MICBIAS enable
    constexpr uint8_t MASK_UAD_EN      = 0x08;  // bit[3]
    constexpr uint8_t MASK_VAD_EN      = 0x04;  // bit[2]
    constexpr uint8_t MASK_UAG_EN      = 0x02;  // bit[1]
}

constexpr uint8_t DEV_STS0 = 0x79;  // channel power status (read-only)
constexpr uint8_t DEV_STS1 = 0x7A;  // mode/PLL/fault status (read-only)

namespace dev_sts1 {
    constexpr uint8_t MASK_MODE_STS     = 0xE0;  // bits[7:5]
    constexpr uint8_t MASK_PLL_LOCKED   = 0x10;  // bit[4]
    constexpr uint8_t MASK_MICBIAS_STS  = 0x08;  // bit[3]
    constexpr uint8_t MASK_BOOST_STS    = 0x04;  // bit[2]
    constexpr uint8_t MASK_FAULT_ACTIVE = 0x02;  // bit[1]
    constexpr uint8_t MASK_MICBIAS_FAULT = 0x01; // bit[0]
}

// --- Slot assignment helper --------------------------------------------------

inline constexpr uint8_t makeSlot(uint8_t n, bool enable = true) {
    return (enable ? slot_cfg::MASK_ENABLE : uint8_t{0}) | (n & slot_cfg::MASK_SLOT);
}

// =============================================================================
// DSP_CFG1 — chip-global DAC DSP config (§8.1.1.101)
// =============================================================================

constexpr uint8_t DSP_CFG1 = 0x73;

namespace dsp_cfg1 {
    constexpr uint8_t SHIFT_INTX_FILT = 6;
    constexpr uint8_t MASK_INTX_FILT  = 0xC0;  // bits[7:6]
    constexpr uint8_t SHIFT_HPF_SEL   = 4;
    constexpr uint8_t MASK_HPF_SEL    = 0x30;  // bits[5:4]
    constexpr uint8_t SHIFT_BQ_CFG    = 2;
    constexpr uint8_t MASK_BQ_CFG     = 0x0C;  // bits[3:2]
    constexpr uint8_t MASK_DISABLE_SOFT_STEP = 0x02;  // bit[1]
    constexpr uint8_t MASK_DVOL_GANG  = 0x01;  // bit[0]

    // INTX_FILT[7:6] — DAC interpolation filter response
    constexpr uint8_t INTX_LINEAR_PHASE      = 0x00;
    constexpr uint8_t INTX_LOW_LATENCY       = 0x40;
    constexpr uint8_t INTX_ULTRA_LOW_LATENCY = 0x80;
    constexpr uint8_t INTX_LOW_POWER         = 0xC0;

    // HPF_SEL[5:4] — DAC high-pass filter (mirrors ADC HPF semantics)
    constexpr uint8_t HPF_PROGRAMMABLE = 0x00;  // all-pass = HPF off
    constexpr uint8_t HPF_1HZ          = 0x10;
    constexpr uint8_t HPF_12HZ         = 0x20;
    constexpr uint8_t HPF_96HZ         = 0x30;

    // BQ_CFG[3:2] — biquads allocated per DAC channel (chip-global)
    constexpr uint8_t BQ_NONE = 0x00;
    constexpr uint8_t BQ_1    = 0x04;
    constexpr uint8_t BQ_2    = 0x08;
    constexpr uint8_t BQ_3    = 0x0C;
}

// =============================================================================
// ADC biquad coefficient pages (§8.2.1, §8.2.2)
// =============================================================================
//
// Each biquad occupies 20 contiguous bytes (5 × 4-byte coefficients) at
// offset 0x08 + (idx × 0x14). 12 biquads total, page 8 holds 1..6, page 9
// holds 7..12. Coefficient layout MSB-first per coef:
//   N0[31:0]  N1[31:0]  N2[31:0]  D1[31:0]  D2[31:0]
// in 5.27 fixed-point two's complement.

namespace adc_biquad {
    constexpr uint8_t PAGE_LO   = 8;
    constexpr uint8_t PAGE_HI   = 9;
    constexpr uint8_t BQ_BASE   = 0x08;
    constexpr uint8_t BQ_STRIDE = 0x14;  // 20 bytes per biquad
    constexpr uint8_t COEF_SIZE = 4;

    inline constexpr uint8_t pageFor(uint8_t bq /* 1..12 */) {
        return bq <= 6 ? PAGE_LO : PAGE_HI;
    }
    inline constexpr uint8_t baseFor(uint8_t bq /* 1..12 */) {
        return BQ_BASE + ((bq - 1) % 6) * BQ_STRIDE;
    }

    constexpr uint8_t OFFSET_N0 = 0x00;
    constexpr uint8_t OFFSET_N1 = 0x04;
    constexpr uint8_t OFFSET_N2 = 0x08;
    constexpr uint8_t OFFSET_D1 = 0x0C;
    constexpr uint8_t OFFSET_D2 = 0x10;
}

// =============================================================================
// DAC biquad coefficient pages (§8.2.5, §8.2.6)
// =============================================================================

namespace dac_biquad {
    constexpr uint8_t PAGE_LO   = 15;
    constexpr uint8_t PAGE_HI   = 16;
    constexpr uint8_t BQ_BASE   = 0x08;
    constexpr uint8_t BQ_STRIDE = 0x14;
    constexpr uint8_t COEF_SIZE = 4;

    inline constexpr uint8_t pageFor(uint8_t bq) {
        return bq <= 6 ? PAGE_LO : PAGE_HI;
    }
    inline constexpr uint8_t baseFor(uint8_t bq) {
        return BQ_BASE + ((bq - 1) % 6) * BQ_STRIDE;
    }

    constexpr uint8_t OFFSET_N0 = 0x00;
    constexpr uint8_t OFFSET_N1 = 0x04;
    constexpr uint8_t OFFSET_N2 = 0x08;
    constexpr uint8_t OFFSET_D1 = 0x0C;
    constexpr uint8_t OFFSET_D2 = 0x10;
}

// =============================================================================
// DAC digital volume control (§8.2.8)
// =============================================================================
//
// Encoding (extended range vs. ADC DVOL):
//   0       = mute
//   1..200  = -100.0 dB to -0.5 dB (0.5 dB steps)
//   201     = 0.0 dB unity (POR default)
//   202..255= +0.5 dB to +27.0 dB (0.5 dB steps)

namespace dac_dvol {
    constexpr uint8_t PAGE      = 18;
    constexpr uint8_t CH1A_BASE = 0x0C;
    constexpr uint8_t CH1B_BASE = 0x10;
    constexpr uint8_t CH2A_BASE = 0x14;
    constexpr uint8_t CH2B_BASE = 0x18;

    constexpr uint8_t MUTE        = 0;
    constexpr uint8_t UNITY_0DB   = 201;
    constexpr uint8_t MAX_PLUS_27 = 255;

    constexpr float DB_MIN  = -100.0f;
    constexpr float DB_MAX  =  +27.0f;
    constexpr float DB_STEP =    0.5f;

    inline constexpr uint8_t fromDb(float dB) {
        if (dB <= -100.5f) return MUTE;
        int reg = 201 + static_cast<int>(dB * 2.0f + (dB >= 0 ? 0.5f : -0.5f));
        if (reg < 1)   reg = 1;
        if (reg > 255) reg = 255;
        return static_cast<uint8_t>(reg);
    }

    inline constexpr float toDb(uint8_t reg) {
        if (reg == 0) return -120.0f;
        return (static_cast<float>(reg) - 201.0f) * 0.5f;
    }
}

}  // namespace reg
}  // namespace tac5212
