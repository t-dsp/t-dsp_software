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
#include "Pss140Patches.h"   // baked 100 PSS-140 user-voice patches (study-only; see NOTICE.md)

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
static const char *synthDescription() { return "Yamaha YM2413 OPLL — the PSS-140 chip: 2-op FM. 15 built-in ROM voices + the 100 real PSS-140 patches."; }
static bool        synthIsGM()         { return false; }  // picker streams our own two-bank list
static void        synthSetMpeMode(bool /*mpe*/) {}       // MPE not wired for this backend

// Two banks exposed directly (no lossy GM->15 collapse):
//   index  0..14   -> "ROM: <name>"      = OPLL built-in instrument 1..15
//   index 15..114  -> "PSS-140: <name>"  = a baked PSS-140 user-voice patch
// The "ROM: "/"PSS-140: " prefixes make control.html group them into two banks
// (it splits names on ": " into <optgroup>s). Songs still map their per-channel GM
// program changes onto the 15 ROM voices via the engine's gmToInstrument().
static const int kNumRom = 15;
static int synthNumInstruments() { return kNumRom + kPss140Count; }
static const char *synthInstrumentName(int i) {
    static char buf[40];                       // catalog prints entries sequentially -> one buffer is fine
    if (i < 0 || i >= kNumRom + kPss140Count) return "";
    if (i < kNumRom) snprintf(buf, sizeof(buf), "ROM: %s", g_opll.instrumentName(i + 1));
    else             snprintf(buf, sizeof(buf), "PSS-140: %s", kPss140Names[i - kNumRom]);
    return buf;
}
static int         synthInstrument()          { return g_synthInstrument; }

// The picker "auditions" one voice on every channel. ROM entries force a built-in
// instrument; PSS-140 entries load that 8-byte user voice and force slot 0. A song's
// own Program Change events clear the override per channel and re-diversify as it plays.
static void synthSetInstrument(int idx) {
    const int total = kNumRom + kPss140Count;
    if (idx < 0) idx = 0;
    if (idx >= total) idx = total - 1;
    if (idx >= kNumRom) g_opll.setUserVoice(kPss140Patches[idx - kNumRom]);
    const int inst = (idx < kNumRom) ? (idx + 1) : 0;   // ROM 1..15, or 0 = user voice
    for (uint8_t ch = 1; ch <= 16; ch++)
        if (ch != 10) g_opll.setInstrumentOverride(ch, inst);   // leave the drum channel alone
    g_synthInstrument = idx;
    Serial.printf("[synth] all channels -> %s\n", synthInstrumentName(idx));
}

static void synthBegin() {
    g_opll.begin();                    // reset chip + resampler (instruments are chip-ROM)
    g_opll.setGain(3.5f);              // OPLL's 9-bit DAC runs quiet; lift for a usable level
    // OPLL handles GM drums on channel 10 itself (rhythm section), so let the player
    // pass every channel through (Dexed/OPM leave the default kMaskNoDrums).
    g_player.setChannelMask(tdsp::MidiFilePlayer::kMaskAll);
    Serial.printf("[synth] OPLL (YM2413) ready: %d voices (%d ROM + %d PSS-140) + rhythm\n",
                  kNumRom + kPss140Count, kNumRom, kPss140Count);
}
