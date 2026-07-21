// HeteroOpll.h — MELODIC OPLL voice(s) for a HETEROGENEOUS engine inventory (Thread D).
//
// The heterogeneous-inventory payoff (planning/tracks/DESIGN.md §"Build-time engine
// inventory"): today every synth voice is the SAME engine (the Dexed pool, windowed into
// N slots). This header adds one or more MELODIC OPLL (YM2413) engines ALONGSIDE the Dexed
// pool, each a full Track peer — so a build can be "2 Dexed + 2 OPLL": two DX7 voices plus
// two FM-chiptune voices, each independently addressable via @TRK<i>.
//
// It is the melodic sibling of DrumVoice.h (which puts an OPLL on ch10 as a rhythm voice).
// The pattern is identical — an engine object + its own OpllSink on a mix slot — but here
// the sink is a MELODIC voice bound to a Track (its own player / arp / router / live-MIDI
// subscription), not the drum groove. OPLL is FM: CPU-bound, RAM-cheap (~9 KB, no PSRAM),
// so multiple instances are the cheap axis (DESIGN's sizing rule).
//
//   g_playerV[v]/router[v] -> arp[v] -> g_hoOpllVoiceSink[k] -> g_hoOpll[k] (int16 mono)
//                                    -> I16toF32 -> g_hoOpllTrim[k] --+
//                                                                     +-> g_hoMixL/R -> outL/outR[HO slot]
//
// The N melodic OPLLs SUM through a private 2-in sub-mixer (g_hoMixL/R) into ONE mix slot,
// because outL/outR is only a 4-in AudioMixer4 and the other slots are taken (3=Dexed pool,
// 2=drum voice, 1=tone/metronome). One free slot, N engines -> sub-mix first.
//
// Build flags (set by the env, e.g. teensy41_dexed2_opll2_drums):
//   -D TDSP_HETERO=1          ; enable the heterogeneous-inventory path (main.cpp)
//   -D TDSP_DEXED_VOICES=2    ; number of Dexed pool windows (tracks 0..N-1)
//   -D TDSP_OPLL_ENGINES=2    ; number of melodic OPLL voices appended after the Dexed ones
//
// Included by main.cpp AFTER the Dexed pool backend (so outL/outR + g_pool already exist)
// and gated on TDSP_HETERO. Separate g_hoOpll instances (not the OPLL backend's g_opll,
// which isn't compiled here — the main backend is the Dexed pool) — mirrors DrumVoice.h.
#pragma once
#if TDSP_HETERO
#include <AudioSynthYmfmOPLL.h>
#include "OpllSink.h"

#ifndef TDSP_OPLL_ENGINES
#define TDSP_OPLL_ENGINES 1
#endif
// The per-voice song-player STATE arrays in main.cpp (g_buf3/g_song3* for voice 2, g_buf4/
// g_song4* for voice 3) cover up to two extra melodic voices, so up to TWO OPLL engines are
// wired here. Supporting N>2 needs more of those extra-voice state globals; lift this then.
static_assert(TDSP_OPLL_ENGINES >= 1 && TDSP_OPLL_ENGINES <= 2,
              "HeteroOpll.h wires one or two melodic OPLL voices (see the extra-voice state in main.cpp)");

// The free mix slot the summed melodic-OPLL bus lands on. On the nobt board slot 0 (BT) is
// unused and slot 2 (S/PDIF-in) is freed by TDSP_NO_SPDIF_IN; the Dexed pool owns slot 3,
// the test tone slot 1, the drum voice slot 2. Default to slot 0. Override -D TDSP_HO_SLOT=n.
#ifndef TDSP_HO_SLOT
#define TDSP_HO_SLOT 0
#endif

AudioSynthYmfmOPLL    g_hoOpll[TDSP_OPLL_ENGINES];       // int16 mono (out 0 == out 1), melodic
AudioConvert_I16toF32 g_hoOpllToF32[TDSP_OPLL_ENGINES];  // int16 -> F32 bridge, one per engine
AudioEffectGain_F32   g_hoOpllTrim[TDSP_OPLL_ENGINES];   // per-voice user level (0..1.5)
AudioMixer4_F32       g_hoMixL, g_hoMixR;                // sum the melodic OPLLs into one mix slot

// Per-engine connections (explicit — AudioConnection objects can't be array-constructed).
AudioConnection       c_ho_conv0(g_hoOpll[0], 0, g_hoOpllToF32[0], 0);
AudioConnection_F32   c_ho_trim0(g_hoOpllToF32[0], 0, g_hoOpllTrim[0], 0);
AudioConnection_F32   c_ho_mixL0(g_hoOpllTrim[0], 0, g_hoMixL, 0);   // mono -> both sub-mix channels
AudioConnection_F32   c_ho_mixR0(g_hoOpllTrim[0], 0, g_hoMixR, 0);
OpllSink              g_hoOpllSink0(&g_hoOpll[0]);
#if TDSP_OPLL_ENGINES >= 2
AudioConnection       c_ho_conv1(g_hoOpll[1], 0, g_hoOpllToF32[1], 0);
AudioConnection_F32   c_ho_trim1(g_hoOpllToF32[1], 0, g_hoOpllTrim[1], 0);
AudioConnection_F32   c_ho_mixL1(g_hoOpllTrim[1], 0, g_hoMixL, 1);
AudioConnection_F32   c_ho_mixR1(g_hoOpllTrim[1], 0, g_hoMixR, 1);
OpllSink              g_hoOpllSink1(&g_hoOpll[1]);
#endif
AudioConnection_F32   c_ho_outL(g_hoMixL, 0, outL, TDSP_HO_SLOT);    // summed melodic OPLLs -> mix slot
AudioConnection_F32   c_ho_outR(g_hoMixR, 0, outR, TDSP_HO_SLOT);

// Slot registry (Thread D): the melodic OPLL sinks, indexed 0..TDSP_OPLL_ENGINES-1. main.cpp
// binds track (kDexedVoices + k) to g_hoOpllVoiceSink[k].
tdsp::MidiSink       *g_hoOpllVoiceSink[TDSP_OPLL_ENGINES] = {
    &g_hoOpllSink0,
#if TDSP_OPLL_ENGINES >= 2
    &g_hoOpllSink1,
#endif
};

static const int kHoNumRom = 15;                         // OPLL built-in ROM instruments 1..15
static int       g_hoInstrument[TDSP_OPLL_ENGINES] = { 0 };   // current melodic voice per engine (0..14 -> ROM 1..15)

static int         heteroOpllNumInstruments()      { return kHoNumRom; }
static const char *heteroOpllInstrumentName(int idx) {   // ROM name (engine-independent table)
    static char buf[40];
    if (idx < 0 || idx >= kHoNumRom) return "";
    snprintf(buf, sizeof(buf), "OPLL: %s", g_hoOpll[0].instrumentName(idx + 1));   // "OPLL: " -> app groups it
    return buf;
}
static int heteroOpllInstrument(int eng) {
    if (eng < 0 || eng >= TDSP_OPLL_ENGINES) eng = 0;
    return g_hoInstrument[eng];
}

// Force one ROM voice on every melodic channel of ONE engine (mirrors synthSetInstrument).
// A song's own Program Change events later re-diversify per channel; ch10 is left for drums.
static void heteroOpllSetInstrument(int eng, int idx) {
    if (eng < 0 || eng >= TDSP_OPLL_ENGINES) return;
    if (idx < 0) idx = 0;
    if (idx >= kHoNumRom) idx = kHoNumRom - 1;
    for (uint8_t ch = 1; ch <= 16; ch++)
        if (ch != 10) g_hoOpll[eng].setInstrumentOverride(ch, idx + 1);   // ROM 1..15
    g_hoInstrument[eng] = idx;
    Serial.printf("[hetero-opll] engine %d all channels -> %s\n", eng, heteroOpllInstrumentName(idx));
}

// Per-engine user level (0..150 %), like synthSetVoice2Vol for a Dexed voice. Rides the F32
// trim node so it's independent of the fixed slot make-up.
static int  g_hoVolPct[TDSP_OPLL_ENGINES] = { 0 };   // seeded to 100 in heteroOpllBegin()
static void heteroOpllSetVol(int eng, int pct) {
    if (eng < 0 || eng >= TDSP_OPLL_ENGINES) return;
    if (pct < 0) pct = 0; if (pct > 150) pct = 150;
    g_hoVolPct[eng] = pct;
    g_hoOpllTrim[eng].setGain(pct / 100.0f);
}
// Fixed-engine wrappers for Track::setLevel (a void(int) fn ptr, one per bound track).
static void heteroOpllSetVol0(int pct) { heteroOpllSetVol(0, pct); }
#if TDSP_OPLL_ENGINES >= 2
static void heteroOpllSetVol1(int pct) { heteroOpllSetVol(1, pct); }
#endif

// Bring up the melodic OPLLs and open their shared mix slot. Called from setup() AFTER
// synthBegin() (mirrors drumVoiceBegin()). Returns true (OPLL needs no font -> always ok).
static bool heteroOpllBegin() {
    for (int k = 0; k < TDSP_OPLL_ENGINES; k++) {
        g_hoOpll[k].begin();
        g_hoOpll[k].setGain(5.5f);                              // OPLL's 9-bit DAC runs quiet (match the OPLL backend)
        g_hoVolPct[k] = 100;
        g_hoOpllTrim[k].setGain(1.0f);                          // user level (default unity)
        g_hoMixL.gain(k, 1.0f); g_hoMixR.gain(k, 1.0f);         // sub-mix passthrough (per engine)
        heteroOpllSetInstrument(k, g_hoInstrument[k]);          // a default melodic voice on every channel
    }
    outL.gain(TDSP_HO_SLOT, TDSP_DEFAULT_SYNTH_MAKEUP);         // bus make-up (matches the synth slot)
    outR.gain(TDSP_HO_SLOT, TDSP_DEFAULT_SYNTH_MAKEUP);
    Serial.printf("[hetero-opll] %d melodic OPLL voice(s) ready -> mix slot %d\n", TDSP_OPLL_ENGINES, TDSP_HO_SLOT);
    return true;
}
#endif  // TDSP_HETERO
