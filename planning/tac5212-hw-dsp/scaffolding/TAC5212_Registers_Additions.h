// TAC5212_Registers_Additions.h — proposed additions to lib/TAC5212/src/TAC5212_Registers.h
//
// Scaffolding file. When this branch ships, these declarations get folded
// into the existing TAC5212_Registers.h (preserving its `namespace tac5212 {
// namespace reg {}}` scope). Kept separate here so the diff against master
// is reviewable in one place.
//
// All section references point at TAC5212 datasheet SLASF23A
// (December 2023, revised January 2025).

#pragma once

#include <stdint.h>

namespace tac5212 {
namespace reg {

// =============================================================================
// DSP_CFG1 — chip-global DAC DSP config (§8.1.1.101)
// =============================================================================
//
// Lives on page 0 at register 0x73. Mirrors the structure of DSP_CFG0 but
// for the playback (DAC) path. The interpolation filter, HPF mode, biquad
// allocation, soft-step disable, and DVOL gang flag share this byte.

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
    //   00 Linear-phase (default, best frequency response, ~1 ms latency)
    //   01 Low-latency (less pre-ringing, ~0.5 ms latency)
    //   10 Ultra-low-latency (lowest latency, more aliasing)
    //   11 Low-power (audible roll-off, lowest current consumption)
    constexpr uint8_t INTX_LINEAR_PHASE      = 0x00;
    constexpr uint8_t INTX_LOW_LATENCY       = 0x40;
    constexpr uint8_t INTX_ULTRA_LOW_LATENCY = 0x80;
    constexpr uint8_t INTX_LOW_POWER         = 0xC0;

    // HPF_SEL[5:4] — DAC high-pass filter (mirrors ADC HPF semantics)
    //   00 Programmable first-order IIR; default coefs (page 17/18) are
    //      all-pass = HPF effectively off.
    //   01 -3 dB at 0.00002 × fS  (=  1 Hz at 48 kHz)
    //   10 -3 dB at 0.00025 × fS  (= 12 Hz at 48 kHz)
    //   11 -3 dB at 0.002   × fS  (= 96 Hz at 48 kHz)
    constexpr uint8_t HPF_PROGRAMMABLE = 0x00;
    constexpr uint8_t HPF_1HZ          = 0x10;
    constexpr uint8_t HPF_12HZ         = 0x20;
    constexpr uint8_t HPF_96HZ         = 0x30;

    // BQ_CFG[3:2] — biquads allocated per DAC channel (chip-global)
    //   00 No biquads (filters bypassed)
    //   01 1 biquad per channel
    //   10 2 biquads per channel  (default)
    //   11 3 biquads per channel
    constexpr uint8_t BQ_NONE = 0x00;
    constexpr uint8_t BQ_1    = 0x04;
    constexpr uint8_t BQ_2    = 0x08;
    constexpr uint8_t BQ_3    = 0x0C;
}

// =============================================================================
// ADC biquad coefficient pages (§8.2.1, §8.2.2)
// =============================================================================
//
// 12 biquads total, allocated as up to 3 per ADC channel × 4 channels via
// DSP_CFG0[3:2]. Page 8 holds biquads 1..6, page 9 holds biquads 7..12.
// Each biquad occupies 20 contiguous bytes (5 × 4-byte coefficients) at
// offset 0x08 + (idx × 0x14).
//
// Coefficient layout per biquad (most-significant byte first):
//   N0[31:0]  N1[31:0]  N2[31:0]  D1[31:0]  D2[31:0]
//   in 5.27 fixed-point two's complement (range ±16, resolution ~7.45e-9).
//
// To convert from textbook biquad coefficients (a0 normalized to 1):
//   register = round(coef * 2^27)
// clamped to [INT32_MIN, INT32_MAX].

namespace adc_biquad {
    constexpr uint8_t PAGE_LO   = 8;     // biquads 1..6
    constexpr uint8_t PAGE_HI   = 9;     // biquads 7..12
    constexpr uint8_t BQ_BASE   = 0x08;  // first coefficient byte
    constexpr uint8_t BQ_STRIDE = 0x14;  // 20 bytes per biquad
    constexpr uint8_t COEF_SIZE = 4;     // 32-bit coefficient

    inline constexpr uint8_t pageFor(uint8_t bq /* 1..12 */) {
        return bq <= 6 ? PAGE_LO : PAGE_HI;
    }
    inline constexpr uint8_t baseFor(uint8_t bq /* 1..12 */) {
        return BQ_BASE + ((bq - 1) % 6) * BQ_STRIDE;
    }

    // Per-biquad coefficient offsets (relative to baseFor(bq))
    constexpr uint8_t OFFSET_N0 = 0x00;
    constexpr uint8_t OFFSET_N1 = 0x04;
    constexpr uint8_t OFFSET_N2 = 0x08;
    constexpr uint8_t OFFSET_D1 = 0x0C;
    constexpr uint8_t OFFSET_D2 = 0x10;

    // ADC biquad → output channel allocation (datasheet Table 7-22).
    // With BQ_CFG = N biquads/channel, biquad index I maps to:
    //   N=1: BQ1=ch1, BQ2=ch2, BQ3=ch3, BQ4=ch4
    //   N=2: BQ1=ch1, BQ2=ch2, BQ3=ch3, BQ4=ch4, BQ5=ch1, BQ6=ch2, BQ7=ch3, BQ8=ch4
    //   N=3: each output gets 3 biquads spread across BQ1..12 in the same pattern
    inline constexpr uint8_t channelFor(uint8_t bq /* 1..12 */) {
        return ((bq - 1) % 4) + 1;  // 1-indexed
    }
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

    inline constexpr uint8_t channelFor(uint8_t bq /* 1..12 */) {
        return ((bq - 1) % 4) + 1;
    }
}

// =============================================================================
// DAC programmable IIR (HPF custom coefficients, §8.2.7, §8.2.8)
// =============================================================================
//
// First-order IIR with three coefficients (N0, N1, D1) used when
// dsp_cfg1::HPF_PROGRAMMABLE is selected. POR defaults are an all-pass
// shape (HPF effectively off).

namespace dac_iir {
    constexpr uint8_t PAGE_NUM    = 17;  // N0 + N1 here
    constexpr uint8_t PAGE_DEN    = 18;  // D1 here
    constexpr uint8_t N0_BASE     = 0x78;  // 4 bytes (page 17)
    constexpr uint8_t N1_BASE     = 0x7C;  // 4 bytes (page 17)
    constexpr uint8_t D1_BASE     = 0x08;  // 4 bytes (page 18)
}

// =============================================================================
// DAC digital volume control (§8.2.8)
// =============================================================================
//
// Encoding (matches ADC DVOL conceptually but with extended range):
//   0       = mute
//   1..200  = -100.0 dB to -0.5 dB (0.5 dB steps)
//   201     = 0.0 dB unity (POR default)
//   202..255= +0.5 dB to +27.0 dB (0.5 dB steps)
//
// Each DAC channel has a 4-byte volume register starting at the indicated
// base. Only the most significant byte of the four contains the encoding;
// the remaining 3 bytes are written as 0x00 (the spec calls for a 32-bit
// write so the auto-increment burst layout is consistent across pages).

namespace dac_dvol {
    constexpr uint8_t PAGE      = 18;
    constexpr uint8_t CH1A_BASE = 0x0C;
    constexpr uint8_t CH1B_BASE = 0x10;
    constexpr uint8_t CH2A_BASE = 0x14;
    constexpr uint8_t CH2B_BASE = 0x18;
    constexpr uint8_t COEF_SIZE = 4;

    constexpr uint8_t MUTE        = 0;
    constexpr uint8_t UNITY_0DB   = 201;
    constexpr uint8_t MAX_PLUS_27 = 255;

    constexpr float   DB_MIN  = -100.0f;
    constexpr float   DB_MAX  =  +27.0f;
    constexpr float   DB_STEP =    0.5f;

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

// =============================================================================
// DAC distortion limiter coefficients (§8.2.10, page 25)
// =============================================================================
//
// Six 32-bit coefficients control the limiter's behavior. Helpers in
// TAC5212.cpp convert engineering units (ms, dB) to the on-chip exponential
// time-constant / linear-gain encodings.

namespace dac_limiter {
    constexpr uint8_t PAGE              = 25;
    constexpr uint8_t ATTACK_COEFF      = 0x60;
    constexpr uint8_t RELEASE_COEFF     = 0x64;
    constexpr uint8_t ENV_DECAY_COEFF   = 0x68;
    constexpr uint8_t THRESHOLD_MAX     = 0x6C;
    constexpr uint8_t THRESHOLD_MIN     = 0x70;
    constexpr uint8_t INFLECTION_POINT  = 0x74;
    constexpr uint8_t SLOPE             = 0x78;
    constexpr uint8_t RESET_COUNTER     = 0x7C;
    constexpr uint8_t COEF_SIZE         = 4;
}

// =============================================================================
// DAC DRC (dynamic range controller) coefficients (§8.2.13, page 28)
// =============================================================================
//
// Eight 32-bit coefficients. Engineering-unit helpers (attackMs → coef,
// thresholdDb → linear) live in TAC5212.cpp.

namespace dac_drc {
    constexpr uint8_t PAGE             = 28;
    constexpr uint8_t MAX_GAIN         = 0x1C;
    constexpr uint8_t MIN_GAIN         = 0x20;
    constexpr uint8_t ATTACK_TC        = 0x24;
    constexpr uint8_t RELEASE_TC       = 0x28;
    constexpr uint8_t RELEASE_HOLD_CT  = 0x2C;
    constexpr uint8_t RELEASE_HYST     = 0x30;
    constexpr uint8_t INV_RATIO        = 0x34;
    constexpr uint8_t INFLECTION_POINT = 0x38;
    constexpr uint8_t COEF_SIZE        = 4;
}

// =============================================================================
// AGC / DRC / limiter master enables (page 1)
// =============================================================================
//
// Per-channel enable bits for AGC (ADC) and DRC (DAC) live on page 1
// register 0x24 (AGC_DRC_CFG, §8.1.2.16). Limiter input/output routing
// lives at page 1 register 0x23 (LIMITER_CFG, §8.1.2.15).

namespace mixer_misc {
    constexpr uint8_t PAGE = 1;
    constexpr uint8_t LIMITER_CFG = 0x23;
    constexpr uint8_t AGC_DRC_CFG = 0x24;

    namespace agc_drc_cfg {
        constexpr uint8_t MASK_AGC_CH1 = 0x80;
        constexpr uint8_t MASK_AGC_CH2 = 0x40;
        constexpr uint8_t MASK_AGC_CH3 = 0x20;
        constexpr uint8_t MASK_AGC_CH4 = 0x10;
        constexpr uint8_t MASK_DRC_CH1 = 0x08;
        constexpr uint8_t MASK_DRC_CH2 = 0x04;
        constexpr uint8_t MASK_DRC_CH3 = 0x02;
        constexpr uint8_t MASK_DRC_CH4 = 0x01;
    }

    namespace limiter_cfg {
        constexpr uint8_t SHIFT_INP_SEL  = 6;
        constexpr uint8_t MASK_INP_SEL   = 0xC0;
        constexpr uint8_t SHIFT_OUT_SEL  = 4;
        constexpr uint8_t MASK_OUT_SEL   = 0x30;

        // INP_SEL[7:6]
        constexpr uint8_t INP_MAX_BOTH   = 0x00;  // max(ch0, ch1)
        constexpr uint8_t INP_CH1_ONLY   = 0x40;
        constexpr uint8_t INP_CH0_ONLY   = 0x80;
        constexpr uint8_t INP_AVG        = 0xC0;

        // OUT_SEL[5:4]
        constexpr uint8_t OUT_BOTH       = 0x00;  // applied on both
        constexpr uint8_t OUT_CH1_ONLY   = 0x10;
        constexpr uint8_t OUT_CH0_ONLY   = 0x20;
        constexpr uint8_t OUT_NONE       = 0x30;
    }
}

}  // namespace reg
}  // namespace tac5212
