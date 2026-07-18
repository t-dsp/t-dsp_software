// HeteroOpll.h — MELODIC OPLL voice(s) for a HETEROGENEOUS engine inventory (Thread D).
//
// The heterogeneous-inventory payoff (planning/tracks/DESIGN.md §"Build-time engine
// inventory"): today every synth voice is the SAME engine (the Dexed pool, windowed into
// N slots). This header adds one or more MELODIC OPLL (YM2413) engines ALONGSIDE the Dexed
// pool, each a full Track peer — so a build can be "2 Dexed + 1 OPLL": a DX7 lead + a DX7
// pad + an FM-chiptune voice, each independently addressable via @TRK<i>.
//
// It is the melodic sibling of DrumVoice.h (which puts an OPLL on ch10 as a rhythm voice).
// The pattern is identical — a second engine object + its own OpllSink on a free mix slot —
// but here the sink is a MELODIC voice bound to a Track (its own player / arp / router /
// live-MIDI subscription), not the drum groove. OPLL is FM: CPU-bound, RAM-cheap (~9 KB, no
// PSRAM), so multiple instances are the cheap axis (DESIGN's sizing rule).
//
//   g_playerV[v]/router[v] -> arp[v] -> g_hoOpllSink[k] -> g_hoOpll[k] (int16 mono)
//                                    -> I16toF32 -> g_hoOpllTrim[k] -> outL/outR[HO slot]
//
// Build flags (set by the env, e.g. teensy41_dexed2_opll1):
//   -D TDSP_HETERO=1          ; enable the heterogeneous-inventory path (main.cpp)
//   -D TDSP_DEXED_VOICES=2    ; number of Dexed pool windows (tracks 0..N-1)
//   -D TDSP_OPLL_ENGINES=1    ; number of melodic OPLL voices appended after the Dexed ones
//
// Included by main.cpp AFTER the Dexed pool backend (so outL/outR + g_pool already exist)
// and gated on TDSP_HETERO. A separate g_hoOpll instance (not the OPLL backend's g_opll,
// which isn't compiled here — the main backend is the Dexed pool) — mirrors DrumVoice.h.
#pragma once
#if TDSP_HETERO
#include <AudioSynthYmfmOPLL.h>
#include "OpllSink.h"

#ifndef TDSP_OPLL_ENGINES
#define TDSP_OPLL_ENGINES 1
#endif
// The minimal proof wires ONE melodic OPLL voice. Supporting N>1 needs per-voice song-player
// STATE arrays in main.cpp (today the extra-voice state globals cover a single index); when
// that generalizes, lift this and turn the objects below into arrays. See DESIGN §"The work".
static_assert(TDSP_OPLL_ENGINES == 1, "HeteroOpll.h currently wires exactly one melodic OPLL voice");

// The free mix slot this voice's bus lands on. On the nobt board slot 0 (BT) is unused and
// slot 2 (S/PDIF-in) is freed by TDSP_NO_SPDIF_IN; the Dexed pool owns slot 3, the test tone
// slot 1. Default to slot 0 (always free on a no-BT build). Override with -D TDSP_HO_SLOT=n.
#ifndef TDSP_HO_SLOT
#define TDSP_HO_SLOT 0
#endif

AudioSynthYmfmOPLL    g_hoOpll;                       // int16 mono (out 0 == out 1), melodic
AudioConvert_I16toF32 g_hoOpllToF32;                  // int16 -> F32 bridge
AudioEffectGain_F32   g_hoOpllTrim;                   // per-voice user level (0..1.5)
AudioConnection       c_ho_conv(g_hoOpll, 0, g_hoOpllToF32, 0);
AudioConnection_F32   c_ho_trim(g_hoOpllToF32, 0, g_hoOpllTrim, 0);
AudioConnection_F32   c_ho_outL(g_hoOpllTrim, 0, outL, TDSP_HO_SLOT);   // mono -> both mix channels
AudioConnection_F32   c_ho_outR(g_hoOpllTrim, 0, outR, TDSP_HO_SLOT);
OpllSink              g_hoOpllSink(&g_hoOpll);
// Slot registry (Thread D): the melodic OPLL sinks, indexed 0..TDSP_OPLL_ENGINES-1. main.cpp
// binds track (kDexedVoices + k) to g_hoOpllVoiceSink[k]. One entry today (see static_assert).
tdsp::MidiSink       *g_hoOpllVoiceSink[TDSP_OPLL_ENGINES] = { &g_hoOpllSink };

static const int kHoNumRom = 15;                      // OPLL built-in ROM instruments 1..15
static int       g_hoInstrument = 0;                  // current melodic voice (0..14 -> ROM 1..15)

static int         heteroOpllNumInstruments()      { return kHoNumRom; }
static const char *heteroOpllInstrumentName(int i) {
    static char buf[40];
    if (i < 0 || i >= kHoNumRom) return "";
    snprintf(buf, sizeof(buf), "OPLL: %s", g_hoOpll.instrumentName(i + 1));   // "OPLL: " -> app groups it
    return buf;
}
static int heteroOpllInstrument() { return g_hoInstrument; }

// Force one ROM voice on every melodic channel (mirrors SynthBackendOpll::synthSetInstrument).
// A song's own Program Change events later re-diversify per channel; ch10 is left for drums.
static void heteroOpllSetInstrument(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= kHoNumRom) idx = kHoNumRom - 1;
    for (uint8_t ch = 1; ch <= 16; ch++)
        if (ch != 10) g_hoOpll.setInstrumentOverride(ch, idx + 1);   // ROM 1..15
    g_hoInstrument = idx;
    Serial.printf("[hetero-opll] all channels -> %s\n", heteroOpllInstrumentName(idx));
}

// Per-voice user level (0..150 %), like synthSetVoice2Vol for a Dexed voice. Rides the F32
// trim node so it's independent of the fixed slot make-up.
static int  g_hoVolPct = 100;
static void heteroOpllSetVol(int pct) {
    if (pct < 0) pct = 0; if (pct > 150) pct = 150;
    g_hoVolPct = pct;
    g_hoOpllTrim.setGain(pct / 100.0f);
}

// Bring up the melodic OPLL and open its mix slot. Called from setup() AFTER synthBegin()
// (mirrors drumVoiceBegin()). Returns true (OPLL needs no font -> always succeeds).
static bool heteroOpllBegin() {
    g_hoOpll.begin();
    g_hoOpll.setGain(5.5f);                                     // OPLL's 9-bit DAC runs quiet (match the OPLL backend)
    g_hoOpllTrim.setGain(g_hoVolPct / 100.0f);                  // user level (default unity)
    outL.gain(TDSP_HO_SLOT, TDSP_DEFAULT_SYNTH_MAKEUP);         // bus make-up (matches the synth slot)
    outR.gain(TDSP_HO_SLOT, TDSP_DEFAULT_SYNTH_MAKEUP);
    heteroOpllSetInstrument(g_hoInstrument);                    // a default melodic voice on every channel
    Serial.printf("[hetero-opll] melodic OPLL voice ready -> mix slot %d\n", TDSP_HO_SLOT);
    return true;
}
#endif  // TDSP_HETERO
