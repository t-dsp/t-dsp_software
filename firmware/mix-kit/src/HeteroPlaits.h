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
AudioConnection_F32 c_hp_outL(g_hpTrim, 0, outL, TDSP_HP_SLOT);   // mono -> both mix channels
AudioConnection_F32 c_hp_outR(g_hpTrim, 0, outR, TDSP_HP_SLOT);

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

// Slot registry: main.cpp binds this Plaits track's Track.sink to g_hpVoiceSink (one track).
tdsp::MidiSink *g_hpVoiceSink = &g_hpPlaitsSink;

static int g_hpInstrument = 0;   // current Plaits synthesis engine 0..15

static int         heteroPlaitsNumInstruments()      { return AudioSynthPlaits::kNumEngines; }
static const char *heteroPlaitsInstrumentName(int idx) {
    static char buf[40];
    if (idx < 0 || idx >= AudioSynthPlaits::kNumEngines) return "";
    snprintf(buf, sizeof(buf), "Plaits: %s", AudioSynthPlaits::engineName(idx));   // "Plaits: " -> app groups it
    return buf;
}
static int heteroPlaitsInstrument() { return g_hpInstrument; }

// Pick one of the 16 Plaits synthesis models for the whole pool (mirrors synthSetInstrument).
static void heteroPlaitsSetInstrument(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= AudioSynthPlaits::kNumEngines) idx = AudioSynthPlaits::kNumEngines - 1;
    g_hpPlaitsSink.setEngine((uint8_t)idx);
    g_hpInstrument = idx;
    Serial.printf("[hetero-plaits] engine -> %d \"%s\"\n", idx, AudioSynthPlaits::engineName(idx));
}

// Timbre-shaping macros — the Plaits faceplate's HARMONICS/TIMBRE/MORPH + the LPG (decay/colour).
// Held as int permille (0..1000) so the wire + @STATE stay integer (matches the app's slider/knob
// convention); the sink takes 0..1 floats. These edit the VOICE, not per-note MIDI — pitch still
// comes from the played note. Seeded in heteroPlaitsBegin() to match the sink's boot defaults.
static int g_hpHarm = 500, g_hpTimbre = 500, g_hpMorph = 500, g_hpDecay = 600, g_hpColor = 500;
static int clampPermille(int v) { return v < 0 ? 0 : (v > 1000 ? 1000 : v); }
static void heteroPlaitsSetHarm(int v)  { g_hpHarm  = clampPermille(v); g_hpPlaitsSink.setHarmonics(g_hpHarm  / 1000.0f); }
static void heteroPlaitsSetTimbre(int v){ g_hpTimbre= clampPermille(v); g_hpPlaitsSink.setTimbre(g_hpTimbre / 1000.0f); }
static void heteroPlaitsSetMorph(int v) { g_hpMorph = clampPermille(v); g_hpPlaitsSink.setMorph(g_hpMorph  / 1000.0f); }
static void heteroPlaitsSetDecay(int v) { g_hpDecay = clampPermille(v); g_hpPlaitsSink.setDecay(g_hpDecay  / 1000.0f); }
static void heteroPlaitsSetColor(int v) { g_hpColor = clampPermille(v); g_hpPlaitsSink.setLpgColour(g_hpColor / 1000.0f); }

// Per-track user level (0..150 %) on the trim node — independent of the fixed slot make-up.
static int  g_hpVolPct = 0;   // seeded to 100 in heteroPlaitsBegin()
static void heteroPlaitsSetVol(int pct) {
    if (pct < 0) pct = 0; if (pct > 150) pct = 150;
    g_hpVolPct = pct;
    g_hpTrim.setGain(pct / 100.0f);
}

// Bring up the Plaits track + open its mix slot. Called from setup() after the primary
// synthBegin() (mirrors heteroOpllBegin()). Returns true (Plaits needs no font -> always ok).
static bool heteroPlaitsBegin() {
    g_hpPlaitsSink.setEngine(0);            // VA default
    heteroPlaitsSetHarm(g_hpHarm);          // seed macros through the setters so cache == sink (0.5/0.5/0.5/0.6/0.5)
    heteroPlaitsSetTimbre(g_hpTimbre);
    heteroPlaitsSetMorph(g_hpMorph);
    heteroPlaitsSetDecay(g_hpDecay);
    heteroPlaitsSetColor(g_hpColor);
    g_hpPlaitsSink.setTimbreDepth(1.0f);
    g_hpPlaitsSink.setMasterChannel(0);     // plain poly (applyMidiMode may switch)
    const float g = 0.5f;                   // per-voice headroom (a hot engine can pass full-scale)
    for (int i = 0; i < kHpVoices; i++) g_hpMix.gain(i, g);
    g_hpVolPct = 100;
    g_hpTrim.setGain(1.0f);
    outL.gain(TDSP_HP_SLOT, TDSP_DEFAULT_SYNTH_MAKEUP);   // bus make-up (matches the synth slot)
    outR.gain(TDSP_HP_SLOT, TDSP_DEFAULT_SYNTH_MAKEUP);
    heteroPlaitsSetInstrument(g_hpInstrument);
    Serial.printf("[hetero-plaits] Plaits track ready: %d voices -> mix slot %d\n", kHpVoices, TDSP_HP_SLOT);
    return true;
}
#endif  // TDSP_HETERO_PLAITS
