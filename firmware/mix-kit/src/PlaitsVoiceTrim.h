// PlaitsVoiceTrim.h — Tier-1 (audition) per-engine loudness trims for the Plaits
// backend. Index = Plaits engine 0..15 (VA, Waveshaping, FM, ... Hi-Hat).
//
// Ships at UNITY (unswept). Measure on-device with the 'N' ReplayGain sweep
// (see REPLAYGAIN.md), which plays C4 through each engine and prints a
// paste-ready replacement for kPlaitsVoiceTrim[] below. Plaits engines vary a
// lot in level (Wavetable is hot, String is quiet), so a sweep is worthwhile.
#pragma once

#include <stdint.h>

static const float kPlaitsVoiceTrim[16] = {
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
};

static inline float plaitsVoiceTrim(int engine) {
    if (engine < 0 || engine >= 16) return 1.0f;
    return kPlaitsVoiceTrim[engine];
}
