// RingsVoiceTrim.h — Tier-1 (audition) per-mode loudness trims for the Rings
// backend. Index = mode 0 Modal, 1 String. Ships at UNITY (unswept); measure
// on-device with the 'N' ReplayGain sweep (see REPLAYGAIN.md).
#pragma once

// Swept on-device via the 'N'/@GAIN ReplayGain sweep (String is slightly quieter
// than Modal). Re-run the sweep and paste over this if the engine changes.
static const float kRingsVoiceTrim[2] = { 1.000f, 1.109f };

static inline float ringsVoiceTrim(int mode) {
    if (mode < 0 || mode >= 2) return 1.0f;
    return kRingsVoiceTrim[mode];
}
