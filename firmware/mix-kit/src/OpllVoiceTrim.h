// OpllVoiceTrim.h — Tier-1 (audition) ReplayGain trims for the OPLL picker voices.
//
// index 0..14   -> ROM instrument 1..15
// index 15..114 -> PSS-140 user-voice patch 0..99   (15 + kPss140Count = 115 total)
//
// Same role as DexedVoiceGains.h, for the OPLL backend: a per-picker-voice loudness trim
// applied as one bus gain (g_opllTrim) when a voice is auditioned on all channels. OPLL's
// 15 ROM timbres + 100 PSS-140 patches vary in level, so switching in the app picker jumps
// in loudness without this.
//
// SHIPS AT UNITY. To bake real values: flash teensy41_opll, open the serial monitor, press
// 'N' (see REPLAYGAIN.md), and paste the printed kOpllVoiceTrim[115] block over the array
// below. If a voice faults mid-sweep, resume with "@GAIN=<next index>".
//
// NOTE: this is the *audition* trim (all channels = one voice). Multitimbral song playback
// is normalized separately (Tier-2, per-GM-program) — see SynthBackendOpll.h / the engine's
// per-program attenuation table.
#pragma once
#include <stdint.h>

// 115 entries (15 ROM + 100 PSS-140). 1.0 = unity until swept.
// Baked from the jay-mint 'N' sweep (2026-07-14): K-weighted max-short-term loudness,
// median target loud=0.09501, peakCeil=1.40, clamp [0.10, 6.0]. 115/115 measured.
// (// bank N = 32-voice groupings from the sweep printer; index 0..14 = ROM, 15..114 = PSS-140.)
static const float kOpllVoiceTrim[115] = {
    // bank 0
    2.880f,0.980f,0.765f,0.903f,0.873f,0.773f,0.897f,1.365f,1.245f,1.259f,1.000f,1.043f,1.113f,0.700f,0.893f,0.640f,
    1.129f,0.763f,1.258f,1.224f,0.950f,0.673f,1.073f,0.846f,1.015f,0.715f,1.534f,1.370f,1.103f,0.609f,0.858f,0.763f,
    // bank 1
    1.381f,1.971f,0.899f,0.822f,0.999f,0.768f,0.674f,1.088f,1.014f,0.766f,0.683f,0.756f,1.275f,0.736f,1.536f,1.217f,
    1.060f,0.715f,1.558f,0.559f,0.792f,1.134f,1.147f,1.229f,1.038f,1.069f,1.113f,0.783f,1.293f,1.198f,1.568f,1.012f,
    // bank 2
    1.378f,1.192f,1.070f,0.982f,0.963f,0.886f,1.077f,0.574f,1.213f,0.976f,1.308f,0.775f,1.038f,0.747f,0.773f,0.954f,
    0.795f,1.039f,1.589f,0.934f,1.221f,0.908f,1.711f,0.533f,0.958f,1.039f,0.887f,1.350f,0.895f,0.727f,0.930f,0.819f,
    // bank 3
    1.220f,1.082f,0.746f,1.010f,1.279f,1.019f,1.288f,0.699f,0.916f,0.985f,1.048f,0.891f,1.307f,2.139f,2.044f,0.954f,
    2.540f,0.660f,0.715f,
};

// Bounds-checked accessor (unity for out-of-range).
static inline float opllVoiceTrim(int idx) {
    if (idx < 0 || idx >= 115) return 1.0f;
    return kOpllVoiceTrim[idx];
}
