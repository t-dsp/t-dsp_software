// SynthBackendPlaits.h — AUTHENTIC Mutable Instruments Plaits backend for the mix-kit.
//
// The real Plaits macro-oscillator (Emilie Gillet, MIT; lib/TDspPlaits2), NOT the
// stock-primitive approximation that lib/TDspPlaits used to be. A pool of N real
// Plaits Voices gives full 3-axis MPE: per-note pitch bend + pressure (-> LEVEL/VCA)
// + timbre (CC#74 -> the voice's TIMBRE). The app's picker selects one of the 16
// synthesis models (VA, waveshaping, FM, granular, additive, wavetable, chord,
// speech, swarm, noise, particle, string, modal, + 3 drum models).
//
// Plaits renders at the native 48 kHz (no resampling) but has NO idle gate — every
// voice renders every block — so voice count is a CPU/polyphony trade (4 voices'
// worst-case engine ~= 39% on its own; the mix-kit adds BT + S/PDIF on top).
//
// Engine is mono int16 -> F32 bridge -> mix slot 3. Included by main.cpp after
// outL/outR + g_player + g_sdReady.
#pragma once
#include <TDspPlaits2.h>            // AudioSynthPlaits + Plaits2Sink
#include <AudioEffectGain_F32.h>
#include "ReplayGain.h"            // shared K-weighted meter + ILoudnessMeter (Tier-1 sweep)
#include "PlaitsVoiceTrim.h"       // Tier-1 per-engine audition trims (unity until swept)

// 4 voices fit one AudioMixer4 exactly and match the spike-proven CPU budget
// (4 voices of the heaviest engine = 39%), leaving headroom for BT + S/PDIF.
static constexpr int kPlaitsVoices = 4;

// Voice state (~20 KB each, incl. a 16 KB engine buffer) lives in RAM2/OCRAM —
// the mix-kit's DTCM is already full of BT + S/PDIF resampler filters.
DMAMEM AudioSynthPlaits g_plVoice[kPlaitsVoices];          // one real Plaits Voice each (mono int16)
AudioMixer4         g_plMix;                               // sum the voices to mono
AudioConnection     c_plv0(g_plVoice[0], 0, g_plMix, 0);
AudioConnection     c_plv1(g_plVoice[1], 0, g_plMix, 1);
AudioConnection     c_plv2(g_plVoice[2], 0, g_plMix, 2);
AudioConnection     c_plv3(g_plVoice[3], 0, g_plMix, 3);
AudioConvert_I16toF32   g_synthToF32;                     // int16 sub-mix -> F32 (mono)
AudioConnection     c_plMix(g_plMix, 0, g_synthToF32, 0);
tdsp::LoudnessProbe_F32 g_plProbe;                        // K-weighted meter on the RAW (pre-trim) sum
AudioConnection_F32 c_plProbe(g_synthToF32, 0, g_plProbe, 0);
AudioEffectGain_F32 g_plTrim;                             // Tier-1 audition bus gain
AudioConnection_F32 c_plTrimIn(g_synthToF32, 0, g_plTrim, 0);
AudioConnection_F32 c_synthL(g_plTrim, 0, outL, 3);       // mono -> both mix channels
AudioConnection_F32 c_synthR(g_plTrim, 0, outR, 3);

Plaits2Sink::VoicePorts g_plPorts[kPlaitsVoices] = {
    { &g_plVoice[0] }, { &g_plVoice[1] }, { &g_plVoice[2] }, { &g_plVoice[3] },
};
Plaits2Sink     g_plaitsSink(g_plPorts, kPlaitsVoices);
tdsp::MidiSink *g_synthSink = &g_plaitsSink;

static int g_synthInstrument = 0;   // app-picker engine 0..15

static const char *synthName()        { return "Plaits (Mutable MPE)"; }
static const char *synthDescription() { return "Authentic Mutable Instruments Plaits macro-oscillator — 16 synthesis models, full 3-axis MPE (per-note bend + pressure + timbre)."; }
static bool        synthIsGM()         { return false; }  // 16-engine list streamed to the app

// MPE: master ch 1 (member channels 2..16 each own a voice); 0 = plain poly (omni).
static void synthSetMpeMode(bool mpe) {
    g_plaitsSink.onAllNotesOff(0);
    g_plaitsSink.setMasterChannel(mpe ? 1 : 0);
}

// The picker's "instruments" ARE the 16 Plaits synthesis engines.
static int         synthNumInstruments()      { return AudioSynthPlaits::kNumEngines; }
static const char *synthInstrumentName(int i) { return AudioSynthPlaits::engineName(i); }
static int         synthInstrument()          { return g_synthInstrument; }

static void synthSetInstrument(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= AudioSynthPlaits::kNumEngines) idx = AudioSynthPlaits::kNumEngines - 1;
    g_plaitsSink.setEngine((uint8_t)idx);
    g_synthInstrument = idx;
    // Tier-1 audition trim: all voices now play this one engine, so a single bus
    // gain normalizes exactly what's sounding (unity until PlaitsVoiceTrim.h swept).
    g_plTrim.setGain(tdsp::auditionTrim(plaitsVoiceTrim(idx)));
    Serial.printf("[synth] engine -> %d \"%s\" (trim=%.3f)\n",
                  idx, AudioSynthPlaits::engineName(idx), (double)plaitsVoiceTrim(idx));
}

// --- ReplayGain hooks (Tier-1 only; single-timbre engine) --------------------
#define TDSP_HAS_REPLAYGAIN 1
static tdsp::ILoudnessMeter *synthLoudness()     { return &g_plProbe; }
static AudioEffectGain_F32  *synthAuditionTrim() { return &g_plTrim; }
static float                 synthVoiceTrim(int idx) { return plaitsVoiceTrim(idx); }
static const char           *synthTrimSymbol()   { return "kPlaitsVoiceTrim"; }

static void synthBegin() {
    // Pleasant default patch: VA engine, mid macros, a little sustain for keys.
    g_plaitsSink.setEngine(0);
    g_plaitsSink.setHarmonics(0.5f);
    g_plaitsSink.setTimbre(0.5f);
    g_plaitsSink.setMorph(0.5f);
    g_plaitsSink.setDecay(0.6f);        // longer LPG tail -> playable sustain
    g_plaitsSink.setLpgColour(0.5f);
    g_plaitsSink.setTimbreDepth(1.0f);
    g_plaitsSink.setMasterChannel(0);   // start in plain poly (applyMidiMode may switch)
    g_plTrim.setGain(tdsp::auditionTrim(plaitsVoiceTrim(0)));

    // 4 voices summed can pass full-scale on hot engines; keep per-voice headroom.
    const float g = 0.5f;
    g_plMix.gain(0, g); g_plMix.gain(1, g); g_plMix.gain(2, g); g_plMix.gain(3, g);

    // Melodic engine — drop the drum channel upstream (Plaits' own drum models are
    // pickable, but a GM song's ch10 shouldn't force them).
    g_player.setChannelMask(tdsp::MidiFilePlayer::kMaskNoDrums);
    Serial.printf("[synth] Plaits ready: %d voices, 16 engines, 3-axis MPE\n", kPlaitsVoices);
}
