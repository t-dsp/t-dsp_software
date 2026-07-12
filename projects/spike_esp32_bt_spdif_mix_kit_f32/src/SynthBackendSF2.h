// SynthBackendSF2.h — SF2 General-MIDI sampled-instrument backend for the mix-kit.
//
// A selectable synth backend (Dexed / ymfm OPM / OPL3 / SF2), same interface as
// SynthBackendOpl3.h. Like OPL3 this is a self-contained GM synth, but instead of a
// chip it renders REAL sampled instruments: a pool of AudioSynthWavetable voices
// driven by Sf2GmEngine (lib/TDspSF2), which loads SF2 instruments off SD into PSRAM
// at runtime (manicken/sf22aswt) and does the GM allocation.
//
//   voices (mono int16) --> AudioMixer4 tree --> AudioConvert_I16toF32 --> mix slot 3
//
// SF2 samples are mono, so the summed mono output is duplicated into outL/outR slot 3.
// Included by main.cpp AFTER outL/outR + g_player + g_sdReady.
//
// Needs /sf2/gm.sf2 on the SD card. See lib/TDspSF2/SF2_GM_ENGINE_PLAN.md.
#pragma once
#include <Audio.h>
#include <Sf2GmEngine.h>
#include "SF2Sink.h"

// --- voice pool + summing tree (int16 classic Audio graph) ------------------
// 24 voices target the plan's 24–32 polyphony. Tree: 6 first-level mixers (4 voices
// each) -> 2 -> 1. First-level input gain leaves polyphony headroom before the int16
// sum; upper levels stay at unity.
static constexpr int   kSf2NumVoices = 24;
static constexpr float kSf2VoiceMix  = 0.4f;   // per-voice attenuation into level-1 mixers

AudioSynthWavetable   g_sf2Voice[kSf2NumVoices];
AudioMixer4           g_sf2Mix[9];             // [0..5] L1, [6..7] L2, [8] L3 (mono out)
AudioConvert_I16toF32 g_sf2ToF32;              // mono int16 -> F32

// Static links from the tree root into the F32 mix bus (mono duplicated to L+R).
AudioConnection     c_sf2Root(g_sf2Mix[8], 0, g_sf2ToF32, 0);
AudioConnection_F32 c_sf2L   (g_sf2ToF32,  0, outL, 3);
AudioConnection_F32 c_sf2R   (g_sf2ToF32,  0, outR, 3);

Sf2GmEngine     g_sf2;
SF2Sink         g_sf2Sink(&g_sf2);
tdsp::MidiSink *g_synthSink = &g_sf2Sink;

static int g_synthInstrument = 0;   // app-picker "audition" program (0..127)

static const char *synthName()        { return "SF2 GM"; }
static const char *synthDescription() { return "General MIDI from real sampled instruments (SF2), loaded to PSRAM at runtime: a patch per channel, with drums."; }
static bool        synthIsGM()         { return true; }   // 128 standard GM programs -> app renders names locally
static void        synthSetMpeMode(bool /*mpe*/) {}       // MPE not wired for this backend yet (router still bends)
static int         synthNumInstruments()      { return g_sf2.numMelodic(); }        // 128 GM
static const char *synthInstrumentName(int i) { return g_sf2.melodicName(i); }
static int         synthInstrument()          { return g_synthInstrument; }

// The app's single picker "auditions" one GM program on every channel; a song's own
// Program Change events re-diversify per channel as it plays.
static void synthSetInstrument(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= g_sf2.numMelodic()) idx = g_sf2.numMelodic() - 1;
    for (uint8_t ch = 1; ch <= 16; ch++)
        if (ch != 10) g_sf2.programChange(ch, (uint8_t)idx);   // leave the drum channel alone
    g_synthInstrument = idx;
    Serial.printf("[synth] all channels -> GM %d = %s\n", idx, g_sf2.melodicName(idx));
}

// Build the voice -> mixer-tree connections (dynamic: one-time, persist for the run).
static void sf2BuildGraph() {
    AudioNoInterrupts();
    for (int i = 0; i < kSf2NumVoices; i++) {                 // L1: voices -> mix[0..5]
        new AudioConnection(g_sf2Voice[i], 0, g_sf2Mix[i / 4], i % 4);
        g_sf2Mix[i / 4].gain(i % 4, kSf2VoiceMix);
    }
    new AudioConnection(g_sf2Mix[0], 0, g_sf2Mix[6], 0);      // L2
    new AudioConnection(g_sf2Mix[1], 0, g_sf2Mix[6], 1);
    new AudioConnection(g_sf2Mix[2], 0, g_sf2Mix[6], 2);
    new AudioConnection(g_sf2Mix[3], 0, g_sf2Mix[6], 3);
    new AudioConnection(g_sf2Mix[4], 0, g_sf2Mix[7], 0);
    new AudioConnection(g_sf2Mix[5], 0, g_sf2Mix[7], 1);
    new AudioConnection(g_sf2Mix[6], 0, g_sf2Mix[8], 0);      // L3 -> root
    new AudioConnection(g_sf2Mix[7], 0, g_sf2Mix[8], 1);
    AudioInterrupts();
}

static void synthBegin() {
    sf2BuildGraph();
    g_sf2.attachVoices(g_sf2Voice, kSf2NumVoices);

    if (!g_sdReady) {
        Serial.println("[synth] SF2: no SD card -> engine idle (need /sf2/gm.sf2)");
        return;
    }
    if (!g_sf2.begin("/sf2/gm.sf2")) {
        Serial.println("[synth] SF2: /sf2/gm.sf2 not loaded -> engine idle");
        return;
    }
    g_sf2.setGain(0.9f);

    // SF2 handles GM drums on channel 10 itself, so let the player pass every channel
    // through (the Dexed/OPM backends leave the default kMaskNoDrums).
    g_player.setChannelMask(tdsp::MidiFilePlayer::kMaskAll);
    Serial.printf("[synth] SF2 GM ready: %d GM instruments + drums, %d voices\n",
                  g_sf2.numMelodic(), kSf2NumVoices);
}
