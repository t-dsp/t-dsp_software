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
#include <AudioEffectGain_F32.h>
#include "OpllSink.h"
#include "Pss140Patches.h"   // baked 100 PSS-140 user-voice patches (study-only; see NOTICE.md)
#include "ReplayGain.h"      // shared K-weighted meter + ILoudnessMeter (Tier-1 sweep)
#include "OpllVoiceTrim.h"   // Tier-1 (audition) per-picker-voice trims (Tier-2 derives from these)

// OPLL is MONO (update() transmits identical L/R): convert one channel to F32, then fan the
// (Tier-1 audition-)trimmed signal to both mix channels. The loudness probe taps the RAW
// (pre-trim) F32 so the 'N' sweep measures unnormalized voice level. See REPLAYGAIN.md.
AudioSynthYmfmOPLL  g_opll;                               // int16, out 0=L (== out 1=R)
AudioConvert_I16toF32 g_synthToF32;                       // int16 -> F32 bridge (mono)
AudioConnection     c_opll(g_opll, 0, g_synthToF32, 0);
tdsp::LoudnessProbe_F32 g_opllProbe;                      // K-weighted meter on the RAW sum
AudioConnection_F32 c_opllProbe(g_synthToF32, 0, g_opllProbe, 0);
AudioEffectGain_F32 g_opllTrim;                           // Tier-1 audition bus gain
AudioConnection_F32 c_opllTrimIn(g_synthToF32, 0, g_opllTrim, 0);
AudioConnection_F32 c_synthL(g_opllTrim, 0, outL, 3);     // mono -> both mix channels
AudioConnection_F32 c_synthR(g_opllTrim, 0, outR, 3);
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
    // Tier-1 audition trim: all channels now play this one voice, so a single bus gain
    // normalizes exactly what's sounding (unity until OpllVoiceTrim.h is swept).
    g_opllTrim.setGain(opllVoiceTrim(idx));
    Serial.printf("[synth] all channels -> %s (trim=%.3f)\n", synthInstrumentName(idx), (double)opllVoiceTrim(idx));
}

// --- ReplayGain hooks (see REPLAYGAIN.md) ------------------------------------
// OPLL is multitimbral during song playback, so it uses BOTH tiers: Tier-1 audition bus
// gain (g_opllTrim, above) and Tier-2 per-GM-program engine attenuation (setGmSongTrim).
#define TDSP_HAS_REPLAYGAIN 1
#define TDSP_REPLAYGAIN_MULTITIMBRAL 1     // songStart neutralizes the audition trim
static tdsp::ILoudnessMeter *synthLoudness()     { return &g_opllProbe; }
static AudioEffectGain_F32  *synthAuditionTrim() { return &g_opllTrim; }
static float                 synthVoiceTrim(int idx) { return opllVoiceTrim(idx); }
static const char           *synthTrimSymbol()   { return "kOpllVoiceTrim"; }

static void synthBegin() {
    g_opll.begin();                    // reset chip + resampler (instruments are chip-ROM)
    g_opll.setGain(3.5f);              // OPLL's 9-bit DAC runs quiet; lift for a usable level
    // Tier-2 song norm is DERIVED, not swept: in a GM song every channel's program is
    // funneled by gmToInstrument() onto one of the 15 ROM voices, whose loudness we already
    // measured in the Tier-1 audition table (kOpllVoiceTrim[0..14]). So a program's trim ==
    // its mapped ROM voice's trim — no separate per-GM sweep needed (PSS-140 patches never
    // play in a song; they're picker-only). Rebuild this whenever kOpllVoiceTrim is re-swept.
    static float s_gmSongTrim[128];
    for (int p = 0; p < 128; ++p)
        s_gmSongTrim[p] = opllVoiceTrim(g_opll.gmToInstrument((uint8_t)p) - 1);  // ROM 1..15 -> idx 0..14
    g_opll.setGmSongTrim(s_gmSongTrim);
    // OPLL handles GM drums on channel 10 itself (rhythm section), so let the player
    // pass every channel through (Dexed/OPM leave the default kMaskNoDrums).
    g_player.setChannelMask(tdsp::MidiFilePlayer::kMaskAll);
    Serial.printf("[synth] OPLL (YM2413) ready: %d voices (%d ROM + %d PSS-140) + rhythm\n",
                  kNumRom + kPss140Count, kNumRom, kPss140Count);
}
