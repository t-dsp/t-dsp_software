// SynthBackendYmfm.h — ymfm OPM MULTITIMBRAL backend for the mix-kit.
//
// The BSD-3-Clause counterpart to SynthBackendDexed.h (same interface). Rather
// than one OPM playing a single patch (like Dexed), this runs FOUR independent
// OPM banks routed by MIDI channel via YmfmOpmMulti, so the player's
// multi-channel songs play a DIFFERENT instrument per part — the "more than one
// instrument at a time" the module was heading toward. Program Change events in
// the song pick each channel's patch from the voice table.
//
// Banks are int16 stereo -> int16 sub-mix -> bridged to F32 into mix slot 3
// (the mix-kit bus is F32; the doc's spike wired int16 straight to slot 0).
// Included by main.cpp AFTER outL/outR are declared.
#pragma once
#include <AudioSynthYmfmOPM.h>
#include <YmfmOpmMulti.h>
#include "YmfmMultiSink.h"

constexpr int kBanks = 4;
AudioSynthYmfmOPM g_b0, g_b1, g_b2, g_b3;               // 4 OPM chips (each stereo int16)
AudioMixer4       g_synMixL, g_synMixR;                  // int16 sub-mix of the 4 banks
AudioConnection   sb0L(g_b0, 0, g_synMixL, 0), sb0R(g_b0, 1, g_synMixR, 0);
AudioConnection   sb1L(g_b1, 0, g_synMixL, 1), sb1R(g_b1, 1, g_synMixR, 1);
AudioConnection   sb2L(g_b2, 0, g_synMixL, 2), sb2R(g_b2, 1, g_synMixR, 2);
AudioConnection   sb3L(g_b3, 0, g_synMixL, 3), sb3R(g_b3, 1, g_synMixR, 3);
AudioConvert_I16toF32 g_synthToF32L, g_synthToF32R;      // int16 sub-mix -> F32
AudioConnection     c_synMixL(g_synMixL, 0, g_synthToF32L, 0);
AudioConnection     c_synMixR(g_synMixR, 0, g_synthToF32R, 0);
AudioConnection_F32 c_synthL (g_synthToF32L, 0, outL, 3);
AudioConnection_F32 c_synthR (g_synthToF32R, 0, outR, 3);

AudioSynthYmfmOPM *g_banks[kBanks] = { &g_b0, &g_b1, &g_b2, &g_b3 };
tdsp::ymfmopm::YmfmOpmMulti g_multi(g_banks, kBanks);
YmfmMultiSink   g_ymfmSink(&g_multi);
tdsp::MidiSink *g_synthSink = &g_ymfmSink;

static const tdsp::ymfmopm::OpmVoice *kVoices[] = {
    &tdsp::ymfmopm::kAdditiveOrgan, &tdsp::ymfmopm::kElectricPiano,
    &tdsp::ymfmopm::kFmBass,        &tdsp::ymfmopm::kBellVibes,
};
static const int kNumVoices = sizeof(kVoices) / sizeof(kVoices[0]);
static int g_synthInstrument = 0;

static const char *synthName()        { return "ymfm OPM x4"; }
static const char *synthDescription() { return "YM2151 (OPM) 4-op FM, 4-part multitimbral: songs play a different patch per channel."; }
static int         synthNumInstruments()        { return kNumVoices; }
static const char *synthInstrumentName(int i)   { return kVoices[i]->name; }
static int         synthInstrument()            { return g_synthInstrument; }

// The app's single instrument picker forces ALL banks to one patch (handy for
// live MIDI / auditioning one sound). A song's Program Change events then
// re-diversify per channel as it plays.
static void synthSetInstrument(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= kNumVoices) idx = kNumVoices - 1;
    for (int i = 0; i < kBanks; i++) g_multi.setBankVoice(i, *kVoices[idx]);
    g_synthInstrument = idx;
    Serial.printf("[synth] all banks -> voice %d = %s\n", idx, kVoices[idx]->name);
}

static void synthBegin() {
    g_multi.begin();
    // Seed each bank with a distinct patch so a multi-channel song is instantly
    // multi-instrument even before its first Program Change; the voice table
    // lets those Program Changes remap per channel afterward.
    for (int i = 0; i < kBanks; i++) g_multi.setBankVoice(i, *kVoices[i % kNumVoices]);
    g_multi.setVoiceTable(kVoices, kNumVoices);   // song Program Change -> voice
    const float g = 0.7f;                          // per-bank sub-mix headroom
    g_synMixL.gain(0, g); g_synMixL.gain(1, g); g_synMixL.gain(2, g); g_synMixL.gain(3, g);
    g_synMixR.gain(0, g); g_synMixR.gain(1, g); g_synMixR.gain(2, g); g_synMixR.gain(3, g);
}
