// TAC5212_Biquad.cpp — RBJ audio EQ cookbook biquad designers.
//
// All formulas pulled from Robert Bristow-Johnson's "Cookbook formulae
// for audio equalizer biquad filter coefficients" (matching textbook
// values that the TS port in tools/web_dev_surface/src/biquad-design.ts
// must reproduce exactly).
//
// Output convention: textbook biquads have transfer function
//
//   H(z) = (b0 + b1·z⁻¹ + b2·z⁻²) / (a0 + a1·z⁻¹ + a2·z⁻²)
//
// We normalize so a0 = 1, then map to TAC5212's 5.27 fixed-point storage:
//
//   N0 = b0,        N1 = b1 / 2,        N2 = b2,
//                   D1 = -a1 / 2,       D2 = -a2
//
// The chip pre-doubles N1 and D1 internally (datasheet §7.3.9.1.6) and
// flips the sign on the denominator side, hence the halvings and negations.

#include "TAC5212_Biquad.h"

#include <cmath>

namespace tac5212 {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Convert (textbook b0, b1, b2, a0, a1, a2) to TAC5212 5.27 coefficients.
BiquadCoeffs pack(float b0, float b1, float b2,
                  float a0, float a1, float a2) {
    // Normalize so a0 = 1.
    const float inv_a0 = 1.0f / a0;
    b0 *= inv_a0;
    b1 *= inv_a0;
    b2 *= inv_a0;
    a1 *= inv_a0;
    a2 *= inv_a0;

    // Halve N1, D1 (chip pre-doubles); flip sign on D1, D2 (chip's transfer
    // function has the denominator written 2³¹ − 2·D1·z⁻¹ − D2·z⁻², so a
    // textbook +a1 in the numerator-form denominator corresponds to a
    // negative D1 here).
    BiquadCoeffs c;
    c.n0 = toQ27(b0);
    c.n1 = toQ27(b1 * 0.5f);
    c.n2 = toQ27(b2);
    c.d1 = toQ27(-a1 * 0.5f);
    c.d2 = toQ27(-a2);
    return c;
}

}  // anonymous namespace

BiquadCoeffs bqBypass() {
    return BiquadCoeffs::bypass();
}

BiquadCoeffs bqPeak(float fs, float fc, float dB, float Q) {
    if (fs <= 0.0f || fc <= 0.0f || Q <= 0.0f) return bqBypass();

    const float A     = std::pow(10.0f, dB / 40.0f);
    const float w0    = 2.0f * kPi * fc / fs;
    const float cosw0 = std::cos(w0);
    const float sinw0 = std::sin(w0);
    const float alpha = sinw0 / (2.0f * Q);

    const float b0 =  1.0f + alpha * A;
    const float b1 = -2.0f * cosw0;
    const float b2 =  1.0f - alpha * A;
    const float a0 =  1.0f + alpha / A;
    const float a1 = -2.0f * cosw0;
    const float a2 =  1.0f - alpha / A;

    return pack(b0, b1, b2, a0, a1, a2);
}

BiquadCoeffs bqLowShelf(float fs, float fc, float dB, float Q) {
    if (fs <= 0.0f || fc <= 0.0f || Q <= 0.0f) return bqBypass();

    const float A     = std::pow(10.0f, dB / 40.0f);
    const float w0    = 2.0f * kPi * fc / fs;
    const float cosw0 = std::cos(w0);
    const float sinw0 = std::sin(w0);
    const float alpha = sinw0 / (2.0f * Q);
    const float beta  = 2.0f * std::sqrt(A) * alpha;

    const float b0 =       A * ((A + 1.0f) - (A - 1.0f) * cosw0 + beta);
    const float b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosw0);
    const float b2 =       A * ((A + 1.0f) - (A - 1.0f) * cosw0 - beta);
    const float a0 =            (A + 1.0f) + (A - 1.0f) * cosw0 + beta;
    const float a1 =    -2.0f * ((A - 1.0f) + (A + 1.0f) * cosw0);
    const float a2 =            (A + 1.0f) + (A - 1.0f) * cosw0 - beta;

    return pack(b0, b1, b2, a0, a1, a2);
}

BiquadCoeffs bqHighShelf(float fs, float fc, float dB, float Q) {
    if (fs <= 0.0f || fc <= 0.0f || Q <= 0.0f) return bqBypass();

    const float A     = std::pow(10.0f, dB / 40.0f);
    const float w0    = 2.0f * kPi * fc / fs;
    const float cosw0 = std::cos(w0);
    const float sinw0 = std::sin(w0);
    const float alpha = sinw0 / (2.0f * Q);
    const float beta  = 2.0f * std::sqrt(A) * alpha;

    const float b0 =        A * ((A + 1.0f) + (A - 1.0f) * cosw0 + beta);
    const float b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosw0);
    const float b2 =        A * ((A + 1.0f) + (A - 1.0f) * cosw0 - beta);
    const float a0 =             (A + 1.0f) - (A - 1.0f) * cosw0 + beta;
    const float a1 =     2.0f * ((A - 1.0f) - (A + 1.0f) * cosw0);
    const float a2 =             (A + 1.0f) - (A - 1.0f) * cosw0 - beta;

    return pack(b0, b1, b2, a0, a1, a2);
}

BiquadCoeffs bqLowpass(float fs, float fc, float Q) {
    if (fs <= 0.0f || fc <= 0.0f || Q <= 0.0f) return bqBypass();

    const float w0    = 2.0f * kPi * fc / fs;
    const float cosw0 = std::cos(w0);
    const float sinw0 = std::sin(w0);
    const float alpha = sinw0 / (2.0f * Q);

    const float b0 = (1.0f - cosw0) * 0.5f;
    const float b1 =  1.0f - cosw0;
    const float b2 = (1.0f - cosw0) * 0.5f;
    const float a0 =  1.0f + alpha;
    const float a1 = -2.0f * cosw0;
    const float a2 =  1.0f - alpha;

    return pack(b0, b1, b2, a0, a1, a2);
}

BiquadCoeffs bqHighpass(float fs, float fc, float Q) {
    if (fs <= 0.0f || fc <= 0.0f || Q <= 0.0f) return bqBypass();

    const float w0    = 2.0f * kPi * fc / fs;
    const float cosw0 = std::cos(w0);
    const float sinw0 = std::sin(w0);
    const float alpha = sinw0 / (2.0f * Q);

    const float b0 =  (1.0f + cosw0) * 0.5f;
    const float b1 = -(1.0f + cosw0);
    const float b2 =  (1.0f + cosw0) * 0.5f;
    const float a0 =   1.0f + alpha;
    const float a1 =  -2.0f * cosw0;
    const float a2 =   1.0f - alpha;

    return pack(b0, b1, b2, a0, a1, a2);
}

BiquadCoeffs bqBandpass(float fs, float fc, float Q) {
    if (fs <= 0.0f || fc <= 0.0f || Q <= 0.0f) return bqBypass();

    const float w0    = 2.0f * kPi * fc / fs;
    const float cosw0 = std::cos(w0);
    const float sinw0 = std::sin(w0);
    const float alpha = sinw0 / (2.0f * Q);

    // "constant 0 dB peak gain" form (BPF II from RBJ cookbook)
    const float b0 =  alpha;
    const float b1 =  0.0f;
    const float b2 = -alpha;
    const float a0 =  1.0f + alpha;
    const float a1 = -2.0f * cosw0;
    const float a2 =  1.0f - alpha;

    return pack(b0, b1, b2, a0, a1, a2);
}

BiquadCoeffs bqNotch(float fs, float fc, float Q) {
    if (fs <= 0.0f || fc <= 0.0f || Q <= 0.0f) return bqBypass();

    const float w0    = 2.0f * kPi * fc / fs;
    const float cosw0 = std::cos(w0);
    const float sinw0 = std::sin(w0);
    const float alpha = sinw0 / (2.0f * Q);

    const float b0 =  1.0f;
    const float b1 = -2.0f * cosw0;
    const float b2 =  1.0f;
    const float a0 =  1.0f + alpha;
    const float a1 = -2.0f * cosw0;
    const float a2 =  1.0f - alpha;

    return pack(b0, b1, b2, a0, a1, a2);
}

float bqMagnitude(const BiquadCoeffs &c, float fs, float f) {
    if (fs <= 0.0f || f <= 0.0f) return 1.0f;

    // Reverse the storage convention: textbook (b0, b1, b2, a1, a2)
    const float b0 = fromQ27(c.n0);
    const float b1 = fromQ27(c.n1) * 2.0f;
    const float b2 = fromQ27(c.n2);
    const float a1 = -fromQ27(c.d1) * 2.0f;
    const float a2 = -fromQ27(c.d2);

    const float w  = 2.0f * kPi * f / fs;
    const float cw = std::cos(w);
    const float sw = std::sin(w);
    // Evaluate H(e^{jw}). Numerator and denominator complex values.
    const float numRe = b0 + b1 * cw + b2 * std::cos(2.0f * w);
    const float numIm = -b1 * sw - b2 * std::sin(2.0f * w);
    const float denRe = 1.0f + a1 * cw + a2 * std::cos(2.0f * w);
    const float denIm = -a1 * sw - a2 * std::sin(2.0f * w);

    const float numMag = std::sqrt(numRe * numRe + numIm * numIm);
    const float denMag = std::sqrt(denRe * denRe + denIm * denIm);
    if (denMag <= 1e-30f) return 0.0f;
    return numMag / denMag;
}

float bqMagnitudeDb(const BiquadCoeffs &c, float fs, float f) {
    const float m = bqMagnitude(c, fs, f);
    if (m <= 1e-10f) return -200.0f;
    return 20.0f * std::log10(m);
}

}  // namespace tac5212
