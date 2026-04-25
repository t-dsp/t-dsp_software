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

#include "TAC5212.h"   // for BiquadCoeffs

#include <stdint.h>

namespace tac5212 {

// Sample rate `fs` in Hz. The codec runs at the FSYNC rate selected via the
// audio serial interface; 48000 is the typical project default.
//
// All `bq*` designers take fs explicitly so they're pure and testable.

BiquadCoeffs bqBypass();

// Peak (parametric) — boost or cut around fc. Q ≈ 0.7 is a smooth one-octave
// bell; Q ≈ 2 is narrow.
BiquadCoeffs bqPeak(float fs, float fc, float dB, float Q);

// Low-shelf — gain change below fc, flat above.
BiquadCoeffs bqLowShelf(float fs, float fc, float dB, float Q);

// High-shelf — gain change above fc, flat below.
BiquadCoeffs bqHighShelf(float fs, float fc, float dB, float Q);

// 2nd-order low-pass (Butterworth-ish; Q = 0.707 = maximally flat).
BiquadCoeffs bqLowpass(float fs, float fc, float Q);

// 2nd-order high-pass.
BiquadCoeffs bqHighpass(float fs, float fc, float Q);

// 2nd-order band-pass (constant skirt gain, peak gain = Q).
BiquadCoeffs bqBandpass(float fs, float fc, float Q);

// 2nd-order notch.
BiquadCoeffs bqNotch(float fs, float fc, float Q);

// Compute response magnitude (linear) at frequency `f` for the given coefs.
// Used by the dev surface to draw the live frequency-response curve.
float bqMagnitude(const BiquadCoeffs &c, float fs, float f);

// Same as bqMagnitude but in dB. Returns -200 dB if magnitude is zero (avoids
// log(0)).
float bqMagnitudeDb(const BiquadCoeffs &c, float fs, float f);

// Convert from textbook biquad coefficients (a0 normalized to 1) to the
// chip's coefficient layout. Despite the historical "Q5.27" framing in
// older codec families, the TAC5212 unity reference is at full int32
// scale (~2³¹), not 2²⁷ — confirmed by BiquadCoeffs::bypass() using
// 0x7FFFFFFF for N0 and by the datasheet's denominator constant of
// 2³¹ in H(z) = (N0 + 2·N1·z⁻¹ + N2·z⁻²) / (2³¹ − 2·D1·z⁻¹ − D2·z⁻²).
// Coefficients with |x| ≥ 1 saturate to 0x7FFFFFFF / 0x80000000 — fine
// for typical EQ designs where normalized values stay in (-1, +1) after
// the N1/D1 halving in pack(). Public so unit tests can verify both halves.
inline constexpr int32_t toQ27(float coef) {
    constexpr float scale = 2147483648.0f;  // 2^31
    const float scaled = coef * scale;
    if (scaled >=  2147483647.0f) return  0x7FFFFFFF;
    if (scaled <  -2147483648.0f) return  static_cast<int32_t>(0x80000000);
    return static_cast<int32_t>(scaled);
}

inline constexpr float fromQ27(int32_t reg) {
    constexpr float invScale = 1.0f / 2147483648.0f;  // 1 / 2^31
    return static_cast<float>(reg) * invScale;
}

}  // namespace tac5212
