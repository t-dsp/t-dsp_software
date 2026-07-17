// DexedVoiceGains.h — ReplayGain-style per-voice loudness trims for the 320
// bundled DX7 voices, BAKED into firmware.
//
// DX7 patches vary wildly in output level (a bright brass patch can be 15-20 dB
// hotter than a soft pad), so switching instruments produces big loudness jumps.
// There's no field in the patch you can read to predict it — you have to render
// the voice and measure. The 'N' command in main.cpp (runGainSweep) does exactly
// that on-device: it plays a fixed reference note through every voice, measures the
// MAX short-term K-weighted (ITU-R BS.1770) loudness + raw peak, and prints a
// ready-to-paste table. K-weighting matches PERCEIVED loudness, so bright and
// percussive patches no longer read quiet and then blast.
//
// WORKFLOW to (re)generate this table:
//   1. Flash the firmware, open the serial monitor.
//   2. Press 'N'. It plays every voice and prints one "V=<i> loud= peak=" line each.
//   3. Compute the table from those lines (median target, min(target/loud, ceil/peak),
//      clamp [0.10, 6.0]) and paste the kDexedVoiceTrim array below. If a voice faults
//      the audio ISR mid-sweep (e.g. "EXPLOSION"), resume with control line "@GAIN=<i+1>"
//      after the auto-reboot; the host stitches the V= lines.
//   4. Reflash.
//
// The trim is applied as a single bus gain (dxpTrim) on the summed pool output in
// synthSetInstrument() — correct because the pool is single-timbre (one voice at a
// time). 1.0 = unity. The sweep centers trims near 1.0 (target = median voice
// loudness) and caps by a raw-peak ceiling so a normalized voice can't clip harder
// than the loudest raw voice already does.

#pragma once

#include <stdint.h>

// index = bank * 32 + voice  (matches DexedVoiceBank ordering). 10 banks x 32.
// Baked from the perceptual 'N' sweep: K-weighted max-short-term loudness, target
// median loud=0.0739, peakCeil=1.40, clamp [0.10, 6.0]. 319/320 measured; voice 319
// ("EXPLOSION") faulted the ISR during the sweep, defaulted to 0.60.
static const float kDexedVoiceTrim[320] = {
    // bank 0
    0.656f,0.556f,3.257f,1.253f,0.743f,0.457f,0.530f,0.591f,0.508f,0.692f,0.690f,0.766f,0.983f,1.106f,0.784f,1.372f,
    1.031f,0.723f,0.776f,1.009f,0.870f,0.868f,1.349f,0.749f,0.858f,1.031f,0.634f,1.188f,1.116f,0.894f,0.747f,1.619f,
    // bank 1
    1.340f,0.567f,1.537f,0.273f,0.273f,1.115f,0.534f,0.690f,1.299f,1.688f,0.509f,1.022f,0.842f,1.586f,1.045f,0.263f,
    0.738f,0.750f,1.348f,2.819f,1.514f,0.456f,0.745f,0.789f,0.637f,0.567f,1.418f,1.108f,0.684f,0.637f,0.863f,0.881f,
    // bank 2
    0.550f,0.578f,1.536f,1.120f,1.543f,0.829f,1.078f,0.679f,1.346f,1.477f,0.607f,1.138f,2.221f,1.078f,1.688f,1.415f,
    0.642f,0.734f,1.439f,1.018f,0.738f,0.786f,1.511f,1.117f,0.765f,0.729f,0.684f,1.237f,1.486f,1.000f,1.992f,6.000f,
    // bank 3
    2.292f,0.626f,1.335f,0.422f,0.629f,0.715f,0.417f,1.020f,5.639f,2.519f,1.780f,0.620f,0.467f,0.533f,0.831f,1.091f,
    3.029f,1.245f,1.290f,1.408f,1.680f,1.327f,2.325f,3.312f,3.421f,1.146f,1.010f,1.583f,0.570f,0.640f,1.171f,1.497f,
    // bank 4
    0.825f,0.961f,1.281f,0.829f,1.330f,0.957f,1.484f,2.246f,0.944f,1.418f,1.487f,0.416f,1.381f,0.660f,0.678f,0.665f,
    0.439f,2.754f,1.074f,0.959f,0.906f,1.070f,1.164f,0.856f,4.339f,1.120f,1.133f,1.038f,0.914f,1.708f,1.264f,0.904f,
    // bank 5
    0.264f,0.611f,0.346f,0.800f,0.998f,0.827f,0.624f,0.400f,0.612f,0.304f,0.508f,1.082f,0.684f,0.622f,0.846f,0.644f,
    1.129f,0.792f,0.823f,0.734f,0.689f,2.070f,0.649f,6.000f,1.572f,2.763f,0.815f,0.563f,1.202f,2.191f,0.872f,6.000f,
    // bank 6
    1.797f,1.439f,1.012f,0.822f,1.842f,1.521f,0.563f,0.604f,0.697f,0.734f,0.636f,1.691f,1.795f,2.754f,1.169f,0.642f,
    1.874f,1.694f,0.804f,1.343f,1.246f,0.673f,2.604f,1.055f,0.620f,0.632f,0.627f,2.204f,1.486f,1.098f,1.048f,6.000f,
    // bank 7
    0.715f,1.238f,0.819f,1.210f,1.380f,0.521f,0.423f,0.586f,1.761f,1.480f,1.528f,1.447f,0.530f,0.419f,1.049f,1.020f,
    1.488f,2.285f,2.402f,0.971f,2.010f,1.511f,0.937f,3.461f,3.420f,0.896f,1.585f,0.889f,0.571f,1.194f,1.328f,1.567f,
    // bank 8
    0.825f,0.961f,1.213f,0.812f,0.956f,0.798f,1.530f,1.333f,1.237f,0.791f,1.371f,0.579f,1.585f,0.751f,0.942f,1.570f,
    1.537f,2.172f,2.249f,0.943f,1.038f,0.729f,0.433f,4.339f,0.911f,0.865f,0.923f,1.688f,0.797f,0.901f,1.070f,0.704f,
    // bank 9
    1.064f,0.365f,0.676f,1.816f,0.689f,1.134f,0.792f,1.654f,1.018f,1.034f,1.078f,0.981f,0.708f,0.694f,2.070f,0.958f,
    0.894f,1.020f,0.446f,3.553f,0.643f,1.430f,0.650f,6.000f,1.572f,2.763f,0.452f,0.564f,1.212f,0.652f,0.872f,0.600f,
};

// Bounds-checked accessor for the per-voice trim (unity for out-of-range).
static inline float dexedVoiceTrim(int idx) {
    if (idx < 0 || idx >= 320) return 1.0f;
    return kDexedVoiceTrim[idx];
}
