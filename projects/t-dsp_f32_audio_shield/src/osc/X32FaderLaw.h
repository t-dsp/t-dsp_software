// X32 fader law — 4-segment piecewise-linear mapping between a 0..1
// "fader" float and a -inf..+10 dB level.
//
// This is the X32 / M32 OSC convention (see Patrick-Gilles Maillot's
// "Unofficial X32/M32 OSC Remote Protocol", v4.06-09, p. 132). Every
// `mix/fader` and `mix/NN/level` leaf in this firmware's OSC tree uses
// this encoding — the wire format is always 0..1 floats; the audio
// graph multiplies by the linear gain `10^(dB/20)`.
//
// Cardinal points worth memorizing:
//
//     dB     |  fader (f)
//     ---    |   ------
//     -inf   |  0.0000     (minimum; reported as -90 dB on read)
//     -60    |  0.0625     (seg-1/seg-2 boundary)
//     -30    |  0.2500     (seg-2/seg-3 boundary)
//     -10    |  0.5000     (seg-3/seg-4 boundary)
//       0    |  0.7500     (unity)
//     +10    |  1.0000     (maximum)
//
// Quantization: faders snap to a 1024-step grid (`mix/fader`); send
// levels snap to a 161-step grid (`mix/NN/level`).
//
// Header-only. Used by both the OSC dispatch (clamping inputs, snapping
// before applying to the audio graph) and the snapshot / echo path
// (reporting the snapped value so clients can confirm the write took).

#pragma once

#include <math.h>

namespace tdsp {
namespace x32 {

// Convert a 0..1 fader value to dB. Below 0 returns -90 (X32 convention
// for "-inf"); above 1 returns +10. NaN coerces to -90.
inline float faderToDb(float f) {
    if (!(f == f)) return -90.0f;             // NaN -> mute
    if (f <= 0.0f) return -90.0f;             // -inf clamped to -90
    if (f >= 1.0f) return 10.0f;
    if (f >= 0.5f)    return f * 40.0f  - 30.0f;   // -10 .. +10  dB
    if (f >= 0.25f)   return f * 80.0f  - 50.0f;   // -30 .. -10  dB
    if (f >= 0.0625f) return f * 160.0f - 70.0f;   // -60 .. -30  dB
    return                  f * 480.0f - 90.0f;   // -inf .. -60 dB
}

// Convert a dB value to the corresponding 0..1 fader position. Outside
// [-90, +10] dB the result is clamped to [0, 1].
inline float dbToFader(float dB) {
    if (dB <= -90.0f) return 0.0f;
    if (dB >=  10.0f) return 1.0f;
    if (dB <  -60.0f) return (dB + 90.0f) / 480.0f;
    if (dB <  -30.0f) return (dB + 70.0f) / 160.0f;
    if (dB <  -10.0f) return (dB + 50.0f) /  80.0f;
    return                   (dB + 30.0f) /  40.0f;
}

// Linear gain (the value you'd actually pass to AudioEffectGain_F32)
// for a given fader position. Equivalent to `pow(10, faderToDb(f)/20)`
// but short-circuits the 0 / 1 endpoints.
inline float faderToLinear(float f) {
    if (!(f == f)) return 0.0f;
    if (f <= 0.0f) return 0.0f;
    if (f >= 1.0f) return 3.16227766f;        // +10 dB
    return powf(10.0f, faderToDb(f) / 20.0f);
}

// Snap a fader float to the 1024-step X32 fader grid. Pass-through for
// values that are already on a grid step. Uses the X32 PDF formula
// `(int)(f * 1023.5) / 1023.0` exactly — including the .5 bias that
// rounds-to-nearest.
inline float quantizeFader(float f) {
    if (!(f == f)) return 0.0f;
    if (f <= 0.0f) return 0.0f;
    if (f >= 1.0f) return 1.0f;
    int step = (int)(f * 1023.5f);
    if (step < 0)    step = 0;
    if (step > 1023) step = 1023;
    return (float)step / 1023.0f;
}

// Snap a send-level float to the 161-step X32 send grid (`mix/NN/level`).
inline float quantizeSendLevel(float f) {
    if (!(f == f)) return 0.0f;
    if (f <= 0.0f) return 0.0f;
    if (f >= 1.0f) return 1.0f;
    int step = (int)(f * 160.5f);
    if (step < 0)   step = 0;
    if (step > 160) step = 160;
    return (float)step / 160.0f;
}

}  // namespace x32
}  // namespace tdsp
