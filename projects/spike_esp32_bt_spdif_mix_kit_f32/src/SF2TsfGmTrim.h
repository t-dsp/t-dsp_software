// SF2TsfGmTrim.h — ReplayGain per-GM-program trims for the SF2/TSF backend.
//
// TSF is General MIDI, so a single 128-entry per-program loudness table serves BOTH tiers:
//   Tier-1 (audition): the picker forces one GM program on all channels -> g_tsfTrim bus gain.
//   Tier-2 (song norm): each channel runs its own program -> per-channel tsf_channel_set_volume.
// The loudness of program p is the same number either way, so both tiers read this array.
//
// 1.0 = unity. SHIPS AT UNITY — bake by flashing teensy41_sf2_tsf, pressing 'N' (audition
// sweep, see REPLAYGAIN.md), and pasting the printed kSf2TsfGmTrim[128] block below. The
// same values then normalize multitimbral song playback via the sink's Tier-2 path.
#pragma once
#include <stdint.h>

static const float kSf2TsfGmTrim[128] = {
    1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,
    1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,
    1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,
    1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,
    1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,
    1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,
    1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,
    1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,
};

static inline float sf2TsfGmTrim(int idx) {
    if (idx < 0 || idx >= 128) return 1.0f;
    return kSf2TsfGmTrim[idx];
}
