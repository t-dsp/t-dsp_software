// HeteroPlaits.h — a MELODIC PLAITS track for a HETEROGENEOUS engine inventory.
//
// The Plaits sibling of HeteroOpll.h: it adds ONE authentic Mutable Instruments Plaits
// track (a small voice POOL behind a Plaits2Sink) ALONGSIDE whatever primary engine the
// build uses, bound to a Track via a tdsp::MidiSink — so a build can be e.g. "OPLL + Plaits"
// or "Dexed + Plaits". Unlike OPLL (one chip == one voice), Plaits is polyphonic per-note:
// ONE Plaits TRACK owns kHpVoices real Plaits Voices summed into one trim/slot, and the
// track's player/arp/live-MIDI drive the pool's allocator (Plaits2Sink) for that one track.
//
//   g_playerV[v]/router[v] -> arp[v] -> g_hpSink -> Plaits2Sink alloc -> g_hpVoice[0..N] (int16)
//                                    -> g_hpMix -> I16toF32 -> g_hpTrim --+
//                                                                         +-> outL/outR[HP slot]
//
// Cost: Plaits has NO idle gate (every voice renders every block) and each voice is ~20 KB
// (16 KB engine buffer). kHpVoices=3 keeps a no-PSRAM budget; voices live in DMAMEM (RAM2)
// like the Plaits primary backend. See planning/multi-engine-tracks/DESIGN.md.
//
// Build flags (set by the env):
//   -D TDSP_HETERO_PLAITS=1   ; compile + wire this Plaits track
//   -D TDSP_HP_SLOT=n         ; the output mix slot for the Plaits track (default 0)
//   -D TDSP_HP_VOICES=n       ; pool size (default 3)
//
// Included by main.cpp AFTER outL/outR exist and gated on TDSP_HETERO_PLAITS. Uses its OWN
// Plaits instances (never the SynthBackendPlaits primary's g_plVoice/g_plaitsSink), so it
// composes with any primary engine — mirrors HeteroOpll's separate g_hoOpll instances.
#pragma once
#if TDSP_HETERO_PLAITS
#include <TDspPlaits2.h>            // AudioSynthPlaits + Plaits2Sink
#include <AudioEffectGain_F32.h>

#ifndef TDSP_HP_VOICES
#define TDSP_HP_VOICES 3
#endif
#ifndef TDSP_HP_SLOT
#define TDSP_HP_SLOT 0
#endif
#ifndef TDSP_PLAITS_ENGINES
#define TDSP_PLAITS_ENGINES 1
#endif
#ifndef TDSP_HP2_SLOT
#define TDSP_HP2_SLOT 2          // second Plaits track (Synth E) -> mix slot 2 (free when TDSP_NO_SPDIF_IN)
#endif
static constexpr int kHpVoices = TDSP_HP_VOICES;
static_assert(kHpVoices >= 1 && kHpVoices <= 4,
              "HeteroPlaits pool sums through one AudioMixer4 (<=4 voices)");

DMAMEM AudioSynthPlaits g_hpVoice[kHpVoices];        // real Plaits Voices (mono int16), RAM2
AudioMixer4           g_hpMix;                        // sum the pool to mono int16
AudioConvert_I16toF32 g_hpToF32;                      // int16 sub-mix -> F32
AudioEffectGain_F32   g_hpTrim;                       // per-track user level (rides the trim node = FX-send tap point)

// Explicit per-voice connections (AudioConnection can't be array-constructed).
AudioConnection     c_hp_v0(g_hpVoice[0], 0, g_hpMix, 0);
#if TDSP_HP_VOICES >= 2
AudioConnection     c_hp_v1(g_hpVoice[1], 0, g_hpMix, 1);
#endif
#if TDSP_HP_VOICES >= 3
AudioConnection     c_hp_v2(g_hpVoice[2], 0, g_hpMix, 2);
#endif
#if TDSP_HP_VOICES >= 4
AudioConnection     c_hp_v3(g_hpVoice[3], 0, g_hpMix, 3);
#endif
AudioConnection     c_hp_mix(g_hpMix, 0, g_hpToF32, 0);
AudioConnection_F32 c_hp_trim(g_hpToF32, 0, g_hpTrim, 0);
#if TDSP_PLAITS_ENGINES >= 2
// Two Plaits tracks (Synth D + E) SUM through one sub-mixer into a single shared output slot, because
// all four main mix slots are occupied (OPLL/Plaits/drums/Dexed). Mirrors HeteroOpll's 2-engine sub-mix.
AudioMixer4_F32     g_hpSubMix;
AudioConnection_F32 c_hp_sub(g_hpTrim, 0, g_hpSubMix, 0);
AudioConnection_F32 c_hp_outL(g_hpSubMix, 0, outL, TDSP_HP_SLOT);
AudioConnection_F32 c_hp_outR(g_hpSubMix, 0, outR, TDSP_HP_SLOT);
#else
AudioConnection_F32 c_hp_outL(g_hpTrim, 0, outL, TDSP_HP_SLOT);   // mono -> both mix channels
AudioConnection_F32 c_hp_outR(g_hpTrim, 0, outR, TDSP_HP_SLOT);
#endif

Plaits2Sink::VoicePorts g_hpPorts[kHpVoices] = {
    { &g_hpVoice[0] },
#if TDSP_HP_VOICES >= 2
    { &g_hpVoice[1] },
#endif
#if TDSP_HP_VOICES >= 3
    { &g_hpVoice[2] },
#endif
#if TDSP_HP_VOICES >= 4
    { &g_hpVoice[3] },
#endif
};
Plaits2Sink     g_hpPlaitsSink(g_hpPorts, kHpVoices);

#if TDSP_PLAITS_ENGINES >= 2
// Second INDEPENDENT Plaits track (Synth E): its own pool + sub-mix -> a different output slot, so
// track (kDexed+kOpll+1) plays a Plaits voice fully separate from the first (own model/macros/notes).
DMAMEM AudioSynthPlaits g_hp2Voice[kHpVoices];
AudioMixer4           g_hp2Mix;
AudioConvert_I16toF32 g_hp2ToF32;
AudioEffectGain_F32   g_hp2Trim;
AudioConnection     c_hp2_v0(g_hp2Voice[0], 0, g_hp2Mix, 0);
#if TDSP_HP_VOICES >= 2
AudioConnection     c_hp2_v1(g_hp2Voice[1], 0, g_hp2Mix, 1);
#endif
#if TDSP_HP_VOICES >= 3
AudioConnection     c_hp2_v2(g_hp2Voice[2], 0, g_hp2Mix, 2);
#endif
#if TDSP_HP_VOICES >= 4
AudioConnection     c_hp2_v3(g_hp2Voice[3], 0, g_hp2Mix, 3);
#endif
AudioConnection     c_hp2_mix(g_hp2Mix, 0, g_hp2ToF32, 0);
AudioConnection_F32 c_hp2_trim(g_hp2ToF32, 0, g_hp2Trim, 0);
AudioConnection_F32 c_hp2_sub(g_hp2Trim, 0, g_hpSubMix, 1);   // Synth E sums with D into the shared Plaits slot
Plaits2Sink::VoicePorts g_hp2Ports[kHpVoices] = {
    { &g_hp2Voice[0] },
#if TDSP_HP_VOICES >= 2
    { &g_hp2Voice[1] },
#endif
#if TDSP_HP_VOICES >= 3
    { &g_hp2Voice[2] },
#endif
#if TDSP_HP_VOICES >= 4
    { &g_hp2Voice[3] },
#endif
};
Plaits2Sink     g_hp2PlaitsSink(g_hp2Ports, kHpVoices);
#endif

// Slot registry: main.cpp binds Plaits track k's Track.sink to g_hpVoiceSink[k] (one per Plaits voice).
tdsp::MidiSink *g_hpVoiceSink[TDSP_PLAITS_ENGINES] = {
    &g_hpPlaitsSink,
#if TDSP_PLAITS_ENGINES >= 2
    &g_hp2PlaitsSink,
#endif
};
// Per-instance sink accessor so the macro/model setters can target a specific Plaits track.
static inline Plaits2Sink &hpSink(int k) {
#if TDSP_PLAITS_ENGINES >= 2
    return k == 1 ? g_hp2PlaitsSink : g_hpPlaitsSink;
#else
    (void)k; return g_hpPlaitsSink;
#endif
}
static inline AudioEffectGain_F32 &hpTrimNode(int k) {
#if TDSP_PLAITS_ENGINES >= 2
    return k == 1 ? g_hp2Trim : g_hpTrim;
#else
    (void)k; return g_hpTrim;
#endif
}

// Per-Plaits-track state (index k = 0..TDSP_PLAITS_ENGINES-1). Each track owns its own model + macros
// + level so Synth D and Synth E are fully independent.
static int g_hpInstrument[TDSP_PLAITS_ENGINES];   // current Plaits synthesis engine 0..15, per track

static int         heteroPlaitsNumInstruments()      { return AudioSynthPlaits::kNumEngines; }
static const char *heteroPlaitsInstrumentName(int idx) {
    static char buf[40];
    if (idx < 0 || idx >= AudioSynthPlaits::kNumEngines) return "";
    snprintf(buf, sizeof(buf), "Plaits: %s", AudioSynthPlaits::engineName(idx));   // "Plaits: " -> app groups it
    return buf;
}
static int heteroPlaitsInstrument(int k) { return g_hpInstrument[k]; }

// Pick one of the 16 Plaits synthesis models for Plaits track k's pool.
static void heteroPlaitsSetInstrument(int k, int idx) {
    if (idx < 0) idx = 0;
    if (idx >= AudioSynthPlaits::kNumEngines) idx = AudioSynthPlaits::kNumEngines - 1;
    hpSink(k).setEngine((uint8_t)idx);
    g_hpInstrument[k] = idx;
    Serial.printf("[hetero-plaits] k%d engine -> %d \"%s\"\n", k, idx, AudioSynthPlaits::engineName(idx));
}

// Timbre-shaping macros — HARMONICS/TIMBRE/MORPH + LPG decay/colour, per Plaits track (permille 0..1000).
// Seeded in heteroPlaitsBegin() to match the sink's boot defaults (0.5/0.5/0.5/0.6/0.5).
static int g_hpHarm[TDSP_PLAITS_ENGINES], g_hpTimbre[TDSP_PLAITS_ENGINES], g_hpMorph[TDSP_PLAITS_ENGINES],
           g_hpDecay[TDSP_PLAITS_ENGINES], g_hpColor[TDSP_PLAITS_ENGINES];
static int clampPermille(int v) { return v < 0 ? 0 : (v > 1000 ? 1000 : v); }
static void heteroPlaitsSetHarm(int k, int v)  { g_hpHarm[k]  = clampPermille(v); hpSink(k).setHarmonics(g_hpHarm[k]  / 1000.0f); }
static void heteroPlaitsSetTimbre(int k, int v){ g_hpTimbre[k]= clampPermille(v); hpSink(k).setTimbre(g_hpTimbre[k] / 1000.0f); }
static void heteroPlaitsSetMorph(int k, int v) { g_hpMorph[k] = clampPermille(v); hpSink(k).setMorph(g_hpMorph[k]  / 1000.0f); }
static void heteroPlaitsSetDecay(int k, int v) { g_hpDecay[k] = clampPermille(v); hpSink(k).setDecay(g_hpDecay[k]  / 1000.0f); }
static void heteroPlaitsSetColor(int k, int v) { g_hpColor[k] = clampPermille(v); hpSink(k).setLpgColour(g_hpColor[k] / 1000.0f); }

// Per-track user level (0..150 %) on the trim node — independent of the fixed slot make-up.
static int  g_hpVolPct[TDSP_PLAITS_ENGINES];
static void heteroPlaitsSetVol(int k, int pct) {
    if (pct < 0) pct = 0; if (pct > 150) pct = 150;
    g_hpVolPct[k] = pct;
    hpTrimNode(k).setGain(pct / 100.0f);
}
// Track.setLevel is a void(*)(int) with no index -> one thin hook per Plaits track.
static void heteroPlaitsSetVol0(int pct) { heteroPlaitsSetVol(0, pct); }
#if TDSP_PLAITS_ENGINES >= 2
static void heteroPlaitsSetVol1(int pct) { heteroPlaitsSetVol(1, pct); }
#endif

// Per-pool voice-headroom gains on each track's summing AudioMixer4.
static void hpMixGains(int k) {
    const float g = 0.5f;   // a hot engine can pass full-scale
#if TDSP_PLAITS_ENGINES >= 2
    AudioMixer4 &mx = (k == 1) ? g_hp2Mix : g_hpMix;
#else
    (void)k; AudioMixer4 &mx = g_hpMix;
#endif
    for (int i = 0; i < kHpVoices; i++) mx.gain(i, g);
}

// Bring up Plaits track k + open its mix slot. Called once per Plaits track from setup() after the
// primary synthBegin() (mirrors heteroOpllBegin()). Returns true (Plaits needs no font -> always ok).
static bool heteroPlaitsBegin(int k) {
#if TDSP_PLAITS_ENGINES >= 2
    const int slot = TDSP_HP_SLOT;          // Synth D + E share one output slot via g_hpSubMix
    g_hpSubMix.gain(k, 1.0f);               // unity sum: each track's own trim sets its level
#else
    const int slot = TDSP_HP_SLOT;
#endif
    hpSink(k).setEngine(0);                 // VA default
    g_hpHarm[k] = 500; g_hpTimbre[k] = 500; g_hpMorph[k] = 500; g_hpDecay[k] = 600; g_hpColor[k] = 500;
    heteroPlaitsSetHarm(k, g_hpHarm[k]);    // seed macros through the setters so cache == sink
    heteroPlaitsSetTimbre(k, g_hpTimbre[k]);
    heteroPlaitsSetMorph(k, g_hpMorph[k]);
    heteroPlaitsSetDecay(k, g_hpDecay[k]);
    heteroPlaitsSetColor(k, g_hpColor[k]);
    hpSink(k).setTimbreDepth(1.0f);
    hpSink(k).setMasterChannel(0);          // plain poly (applyMidiMode may switch)
    hpMixGains(k);
    g_hpVolPct[k] = 100;
    hpTrimNode(k).setGain(1.0f);
    outL.gain(slot, TDSP_DEFAULT_SYNTH_MAKEUP);   // bus make-up (matches the synth slot)
    outR.gain(slot, TDSP_DEFAULT_SYNTH_MAKEUP);
    g_hpInstrument[k] = 0;
    heteroPlaitsSetInstrument(k, g_hpInstrument[k]);
    Serial.printf("[hetero-plaits] k%d ready: %d voices -> mix slot %d\n", k, kHpVoices, slot);
    return true;
}
#endif  // TDSP_HETERO_PLAITS
