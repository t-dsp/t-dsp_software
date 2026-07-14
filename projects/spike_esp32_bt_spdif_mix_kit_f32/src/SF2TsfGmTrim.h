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

// Baked from the jay-mint 'N' sweep (2026-07-14, board SN 7681380 w/ /sf2/gm_tsf.sf2):
// K-weighted max-short-term loudness, median target loud=0.12839, peakCeil=1.40,
// clamp [0.10, 6.0]. 128/128 GM programs measured. Serves BOTH tiers (index = GM program).
// (// bank N = 32-program groupings from the sweep printer.)
static const float kSf2TsfGmTrim[128] = {
    // bank 0
    1.454f,1.504f,0.954f,0.883f,1.041f,1.163f,1.229f,0.910f,1.483f,1.160f,0.939f,0.692f,1.316f,1.105f,0.999f,0.960f,
    1.001f,1.583f,0.857f,0.894f,0.927f,0.881f,0.911f,0.860f,1.142f,1.413f,1.067f,1.338f,1.608f,1.303f,0.609f,0.815f,
    // bank 1
    1.235f,1.247f,1.271f,1.249f,1.259f,2.247f,1.128f,0.808f,0.880f,0.899f,0.722f,1.000f,1.188f,0.908f,0.707f,1.142f,
    0.902f,0.764f,0.920f,0.735f,0.952f,0.901f,0.796f,0.865f,1.800f,1.396f,1.583f,2.015f,0.448f,1.785f,0.701f,0.881f,
    // bank 2
    1.489f,1.554f,1.156f,2.175f,1.202f,1.073f,0.953f,1.195f,0.755f,1.529f,0.868f,0.962f,0.784f,1.078f,0.703f,0.829f,
    0.374f,0.624f,0.612f,0.778f,0.581f,0.519f,1.103f,0.648f,1.063f,1.282f,0.857f,0.865f,0.992f,1.068f,0.566f,1.032f,
    // bank 3
    1.185f,0.888f,0.987f,0.857f,0.858f,0.716f,0.664f,1.087f,0.949f,0.934f,1.539f,1.358f,1.008f,0.698f,1.021f,1.091f,
    1.499f,1.401f,1.022f,1.975f,0.944f,1.486f,0.989f,3.943f,1.378f,1.191f,1.370f,0.876f,0.970f,4.329f,3.310f,1.105f,
};

static inline float sf2TsfGmTrim(int idx) {
    if (idx < 0 || idx >= 128) return 1.0f;
    return kSf2TsfGmTrim[idx];
}
