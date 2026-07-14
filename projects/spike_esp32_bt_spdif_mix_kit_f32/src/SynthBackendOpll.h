// SynthBackendOpll.h — OPLL (YM2413) General-MIDI backend for the mix-kit.
//
// A selectable synth backend (same interface as SynthBackendOpl3.h). This is the
// "PSS-140 chip": Yamaha's YM2413 OPLL, 9 channels of 2-op FM, 15 built-in ROM
// instruments + a 5-piece rhythm section. GM songs play by mapping each channel's
// program onto one of the 15 timbres (see AudioSynthYmfmOPLL::gmToInstrument), and
// channel 10 drives the built-in drums — so a MIDI song is playable end-to-end with
// no external bank.
//
// NOTE: this reproduces the OPLL *chip*, not the PSS-140's 100 custom firmware voices
// (those live in the keyboard's ROM as user-voice register sets; a verified dump is
// plgDavid's github.com/plgDavid/misc "OPLL Synth Patches"/pss140_patches.txt, marked
// study-only). Loading those into the chip's user voice is a future enhancement; today
// this backend uses the 15 authentic chip-ROM instruments.
//
// Engine is stereo int16 -> bridge each channel to F32 into mix slot 3 (the mix-kit
// bus is F32). Included by main.cpp AFTER outL/outR + g_player + g_sdReady.
#pragma once
#include <AudioSynthYmfmOPLL.h>
#include "OpllSink.h"

AudioSynthYmfmOPLL  g_opll;                               // stereo int16: out 0=L, 1=R
AudioConvert_I16toF32 g_synthToF32L, g_synthToF32R;       // int16 -> F32 bridges (L, R)
AudioConnection     c_opllL(g_opll, 0, g_synthToF32L, 0);
AudioConnection     c_opllR(g_opll, 1, g_synthToF32R, 0);
AudioConnection_F32 c_synthL(g_synthToF32L, 0, outL, 3);
AudioConnection_F32 c_synthR(g_synthToF32R, 0, outR, 3);
OpllSink        g_opllSink(&g_opll);
tdsp::MidiSink *g_synthSink = &g_opllSink;

static int g_synthInstrument = 0;   // app-picker "audition" program (0..127)

static const char *synthName()        { return "OPLL (YM2413)"; }
static const char *synthDescription() { return "Yamaha YM2413 OPLL — the PSS-140 chip: 2-op FM, 15 ROM instruments + rhythm, General MIDI."; }
static bool        synthIsGM()         { return true; }   // songs drive per-channel GM programs
static void        synthSetMpeMode(bool /*mpe*/) {}       // MPE not wired for this backend

// Picker exposes the GM program space; the engine folds each onto its 15 timbres. We
// report the OPLL instrument a program lands on, so the app shows something honest.
static int         synthNumInstruments()      { return 128; }
static const char *synthInstrumentName(int i) {
    if (i < 0 || i > 127) return "";
    return g_opll.instrumentName(g_opll.gmToInstrument((uint8_t)i));
}
static int         synthInstrument()          { return g_synthInstrument; }

// The app's single picker "auditions" one GM program on every channel; a song's own
// Program Change events re-diversify per channel as it plays.
static void synthSetInstrument(int idx) {
    if (idx < 0) idx = 0;
    if (idx > 127) idx = 127;
    for (uint8_t ch = 1; ch <= 16; ch++)
        if (ch != 10) g_opll.programChange(ch, (uint8_t)idx);   // leave the drum channel alone
    g_synthInstrument = idx;
    Serial.printf("[synth] all channels -> GM %d = OPLL %s\n", idx, synthInstrumentName(idx));
}

static void synthBegin() {
    g_opll.begin();                    // reset chip + resampler (instruments are chip-ROM)
    g_opll.setGain(3.5f);              // OPLL's 9-bit DAC runs quiet; lift for a usable level
    // OPLL handles GM drums on channel 10 itself (rhythm section), so let the player
    // pass every channel through (Dexed/OPM leave the default kMaskNoDrums).
    g_player.setChannelMask(tdsp::MidiFilePlayer::kMaskAll);
    Serial.printf("[synth] OPLL (YM2413) ready: %d ROM instruments + rhythm\n", g_opll.numInstruments());
}
