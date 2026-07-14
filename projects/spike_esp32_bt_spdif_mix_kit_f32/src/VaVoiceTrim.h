// VaVoiceTrim.h — Tier-1 (audition) per-preset loudness trims for the DaisySP VA
// backend. Index = preset 0..4. Ships at UNITY (unswept); measure on-device with
// the 'N'/@GAIN ReplayGain sweep (see REPLAYGAIN.md).
#pragma once

// Swept on-device via the 'N'/@GAIN ReplayGain sweep. Re-run + paste if presets change.
static const float kVaVoiceTrim[5] = { 1.000f, 1.114f, 0.885f, 0.737f, 1.067f };

static inline float vaVoiceTrim(int preset) {
    if (preset < 0 || preset >= 5) return 1.0f;
    return kVaVoiceTrim[preset];
}
