// SynthBackendOpllPool.h — OPLL (YM2413) *pool* backend: true 3-axis MPE.
//
// Unlike SynthBackendOpll.h (one chip, 9 voices, 2-axis MPE), this runs a POOL of N
// YM2413 chips (one note each) so every note gets independent bend + pressure + TIMBRE
// (CC#74 -> that chip's modulator level). The one axis a single OPLL can't do.
//
// Same two-bank picker as the single-chip backend (15 ROM + 100 PSS-140), but every
// voice is loaded as an 8-byte USER voice (ROM voices via OpllRomVoices.h) so it's
// brightness-modulatable. MPE polyphony = pool size; normal-mode poly = pool size too
// (one note per chip). No rhythm section (drums are masked off upstream) — this backend
// is for melodic MPE performance.
//
// Engine is mono int16 -> F32 bridge -> mix slot 3. Included by main.cpp after outL/outR.
#pragma once
#include <AudioSynthYmfmOPLLPool.h>
#include <OpllRomVoices.h>
#include "OpllPoolSink.h"
#include "Pss140Patches.h"   // baked 100 PSS-140 user voices (study-only; see NOTICE.md)

static constexpr int kOpllPoolSlots = 6;   // MPE polyphony = independent-timbre notes

// The pool is 8 ym2413 chips (~8 KB each) — ~64 KB that RAM1 (DTCM) can't spare on this
// build. Park it in DMAMEM (RAM2/OCRAM) like the Plaits voices: its constructor explicitly
// inits every member (and each Slot's ymfm ctor runs), so uninitialized .dmabss is safe, and
// begin() resets the chips before use. Frees the ~62 KB that was overflowing RAM1.
DMAMEM AudioSynthYmfmOPLLPool g_opllPool;                // mono int16 (out 0 == out 1)
AudioConvert_I16toF32  g_synthToF32;                     // int16 -> F32 bridge
AudioConnection     c_opllPool(g_opllPool, 0, g_synthToF32, 0);
AudioConnection_F32 c_synthL(g_synthToF32, 0, outL, 3);  // mono -> both mix channels
AudioConnection_F32 c_synthR(g_synthToF32, 0, outR, 3);
OpllPoolSink    g_opllSink(&g_opllPool, kOpllPoolSlots,
                           kOpllRomVoices, kOpllRomCount, kPss140Patches, kPss140Count);
tdsp::MidiSink *g_synthSink = &g_opllSink;

static int g_synthInstrument = 0;

static const char *synthName()        { return "OPLL Pool (YM2413 MPE)"; }
static const char *synthDescription() { return "Yamaha YM2413 OPLL chip pool — full 3-axis MPE (per-note bend + pressure + timbre). 15 ROM + 100 PSS-140 voices."; }
static bool        synthIsGM()         { return false; }
static void        synthSetMpeMode(bool mpe) { g_opllSink.setMpeMode(mpe); }

static const int kNumRom = kOpllRomCount;
static int synthNumInstruments() { return kNumRom + kPss140Count; }
static const char *synthInstrumentName(int i) {
    static char buf[40];
    if (i < 0 || i >= kNumRom + kPss140Count) return "";
    if (i < kNumRom) snprintf(buf, sizeof(buf), "ROM: %s", kOpllRomNames[i]);
    else             snprintf(buf, sizeof(buf), "PSS-140: %s", kPss140Names[i - kNumRom]);
    return buf;
}
static int synthInstrument() { return g_synthInstrument; }

static void synthSetInstrument(int idx) {
    const int total = kNumRom + kPss140Count;
    if (idx < 0) idx = 0;
    if (idx >= total) idx = total - 1;
    g_opllSink.setVoiceAll(idx);
    g_synthInstrument = idx;
    Serial.printf("[synth] pool voice -> %s\n", synthInstrumentName(idx));
}

static void synthBegin() {
    g_opllPool.begin();
    g_opllPool.setNumSlots(kOpllPoolSlots);
    g_opllPool.setGain(2.4f);            // N summed notes; leave headroom for chords
    // No OPLL rhythm section here — drop the drum channel upstream (melodic MPE engine).
    g_player.setChannelMask(tdsp::MidiFilePlayer::kMaskNoDrums);
    Serial.printf("[synth] OPLL Pool ready: %d slots, 3-axis MPE, %d voices (%d ROM + %d PSS-140)\n",
                  kOpllPoolSlots, kNumRom + kPss140Count, kNumRom, kPss140Count);
}
