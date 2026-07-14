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
static const float kOpllVoiceTrim[115] = {
    // ROM 0..14
    1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,
    // PSS-140 15..114
    1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,
    1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,
    1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,
    1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,
    1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,
    1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,
    1.0f,1.0f,1.0f,1.0f,
};

// Bounds-checked accessor (unity for out-of-range).
static inline float opllVoiceTrim(int idx) {
    if (idx < 0 || idx >= 115) return 1.0f;
    return kOpllVoiceTrim[idx];
}
