// TAC5212_Biquad.h — biquad coefficient designer for the TAC5212 codec.
//
// All math is single-precision float. Pure functions; no I²C, no allocation.
// Safe to call from Teensy or to mirror in TypeScript on the dev surface.
//
// Coefficients are emitted in 5.27 fixed-point two's complement (TAC5212
// format, datasheet §7.3.9.1.6). The on-chip transfer function is:
//
//   H(z) = (N0 + 2·N1·z⁻¹ + N2·z⁻²) / (2³¹ − 2·D1·z⁻¹ − D2·z⁻²)
//
// which is the textbook biquad with a0 normalized to 1, **but with N1 and
// D1 stored at half their textbook values** (to make the on-chip 32-bit
// adder symmetric). The designers in this header handle the halving.
//
// Reference: Robert Bristow-Johnson's audio EQ cookbook formulas.

#pragma once

#include <stdint.h>

namespace tac5212 {

struct BiquadCoeffs {
    int32_t n0;
    int32_t n1;  // stored as textbook-N1 / 2 (chip pre-doubles)
    int32_t n2;
    int32_t d1;  // stored as textbook-D1 / 2 (chip pre-doubles)
    int32_t d2;

    // POR all-pass: y[n] = x[n]
    static constexpr BiquadCoeffs bypass() {
        return BiquadCoeffs{0x7FFFFFFF, 0, 0, 0, 0};
    }
};

// Sample rate in Hz. The codec runs at the FSYNC rate selected via the
// audio serial interface; 48000 is the default for this project.
//
// All `bq*` functions take fs explicitly so they're pure and testable.

BiquadCoeffs bqBypass();

// Peak (parametric) — boost or cut a region around fc with bandwidth set
// by Q. Q=0.7 is a smooth one-octave bell; Q=2 is narrow.
BiquadCoeffs bqPeak(float fs, float fc, float dB, float Q);

// Low-shelf — gain change below fc, flat above.
BiquadCoeffs bqLowShelf(float fs, float fc, float dB, float Q);

// High-shelf — gain change above fc, flat below.
BiquadCoeffs bqHighShelf(float fs, float fc, float dB, float Q);

// 2nd-order low-pass (Butterworth-ish; Q=0.707 = maximally flat).
BiquadCoeffs bqLowpass(float fs, float fc, float Q);

// 2nd-order high-pass.
BiquadCoeffs bqHighpass(float fs, float fc, float Q);

// 2nd-order band-pass (constant skirt gain, peak gain = Q).
BiquadCoeffs bqBandpass(float fs, float fc, float Q);

// 2nd-order notch.
BiquadCoeffs bqNotch(float fs, float fc, float Q);

// Compute response magnitude (dB) at frequency `f` for the given coefs.
// Used by the dev surface to draw the live frequency-response curve.
// Implementation evaluates H(e^{jω}) at ω = 2π·f/fs.
float bqMagnitudeDb(const BiquadCoeffs &c, float fs, float f);

// Compose multiple cascaded biquads' magnitude response — sum of dB.
// Arrays small (max 3 per channel), inline-friendly.
float bqMagnitudeDbCascade(const BiquadCoeffs *coefs, int n, float fs, float f);

// Convert from textbook coefficients (a0 == 1) to the chip's 5.27 layout.
// Public so unit tests can verify both halves of the conversion.
inline constexpr int32_t toQ27(float coef) {
    constexpr float scale = static_cast<float>(1u << 27);
    float scaled = coef * scale;
    if (scaled >  2147483520.0f) return  0x7FFFFFFF;
    if (scaled < -2147483648.0f) return  static_cast<int32_t>(0x80000000);
    return static_cast<int32_t>(scaled);
}

inline constexpr float fromQ27(int32_t reg) {
    constexpr float invScale = 1.0f / static_cast<float>(1u << 27);
    return static_cast<float>(reg) * invScale;
}

}  // namespace tac5212
