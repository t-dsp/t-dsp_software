// SynthBackendDaisyVa.h — DaisySP virtual-analog backend for the mix-kit.
//
// Two PolyBLEP oscillators -> Moog ladder filter -> ADSR, per voice (Electrosmith
// DaisySP, MIT; lib/TDspDaisyVa). NOT ElectroTechnique's GPL TSynth. Full 3-axis
// MPE: per-note bend + pressure (-> level) + CC#74 (-> filter cutoff). The picker
// selects a preset (Saw Lead / Detuned Saws / Square Reed / Soft Tri / Acid Bass).
//
// VA voices are cheap (~6% CPU each, idle-gated to ~0% when released), so this
// runs 8 voices. Engine is mono int16 -> F32 bridge -> mix slot 3. Included by
// main.cpp after outL/outR + g_player + g_sdReady.
#pragma once
#include <TDspDaisyVa.h>          // AudioSynthDaisyVa + DaisyVaSink
#include <AudioEffectGain_F32.h>
#include "ReplayGain.h"
#include "VaVoiceTrim.h"

static constexpr int kVaVoices = 8;

AudioSynthDaisyVa   g_vaVoice[kVaVoices];
AudioMixer4         g_vaMixA, g_vaMixB, g_vaMixSum;       // 8 voices -> 2 mixers -> sum
AudioConnection     c_va0(g_vaVoice[0], 0, g_vaMixA, 0);
AudioConnection     c_va1(g_vaVoice[1], 0, g_vaMixA, 1);
AudioConnection     c_va2(g_vaVoice[2], 0, g_vaMixA, 2);
AudioConnection     c_va3(g_vaVoice[3], 0, g_vaMixA, 3);
AudioConnection     c_va4(g_vaVoice[4], 0, g_vaMixB, 0);
AudioConnection     c_va5(g_vaVoice[5], 0, g_vaMixB, 1);
AudioConnection     c_va6(g_vaVoice[6], 0, g_vaMixB, 2);
AudioConnection     c_va7(g_vaVoice[7], 0, g_vaMixB, 3);
AudioConnection     c_vaMA(g_vaMixA, 0, g_vaMixSum, 0);
AudioConnection     c_vaMB(g_vaMixB, 0, g_vaMixSum, 1);
AudioConvert_I16toF32   g_synthToF32;                     // int16 -> F32 (mono)
AudioConnection     c_vaMix(g_vaMixSum, 0, g_synthToF32, 0);
tdsp::LoudnessProbe_F32 g_vaProbe;                        // K-weighted meter on the RAW (pre-trim) sum
AudioConnection_F32 c_vaProbe(g_synthToF32, 0, g_vaProbe, 0);
AudioEffectGain_F32 g_vaTrim;                             // Tier-1 audition bus gain
AudioConnection_F32 c_vaTrimIn(g_synthToF32, 0, g_vaTrim, 0);
AudioConnection_F32 c_synthL(g_vaTrim, 0, outL, 3);       // mono -> both mix channels
AudioConnection_F32 c_synthR(g_vaTrim, 0, outR, 3);

DaisyVaSink::VoicePorts g_vaPorts[kVaVoices] = {
    {&g_vaVoice[0]},{&g_vaVoice[1]},{&g_vaVoice[2]},{&g_vaVoice[3]},
    {&g_vaVoice[4]},{&g_vaVoice[5]},{&g_vaVoice[6]},{&g_vaVoice[7]},
};
DaisyVaSink     g_vaSink(g_vaPorts, kVaVoices);
tdsp::MidiSink *g_synthSink = &g_vaSink;

static int g_synthInstrument = 0;   // app-picker preset

static const char *synthName()        { return "Virtual Analog (DaisySP MPE)"; }
static const char *synthDescription() { return "DaisySP virtual-analog: 2 band-limited oscillators -> Moog ladder filter -> ADSR, 8-voice, full 3-axis MPE."; }
static bool        synthIsGM()         { return false; }

static void synthSetMpeMode(bool mpe) {
    g_vaSink.onAllNotesOff(0);
    g_vaSink.setMasterChannel(mpe ? 1 : 0);
}

static int         synthNumInstruments()      { return DaisyVaSink::numPresets(); }
static const char *synthInstrumentName(int i) { return DaisyVaSink::presetName(i); }
static int         synthInstrument()          { return g_synthInstrument; }

static void synthSetInstrument(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= DaisyVaSink::numPresets()) idx = DaisyVaSink::numPresets() - 1;
    g_vaSink.setPreset(idx);
    g_synthInstrument = idx;
    g_vaTrim.setGain(tdsp::auditionTrim(vaVoiceTrim(idx)));
    Serial.printf("[synth] preset -> %d \"%s\"\n", idx, DaisyVaSink::presetName(idx));
}

// --- ReplayGain hooks (Tier-1 only; single-timbre engine) --------------------
#define TDSP_HAS_REPLAYGAIN 1
static tdsp::ILoudnessMeter *synthLoudness()     { return &g_vaProbe; }
static AudioEffectGain_F32  *synthAuditionTrim() { return &g_vaTrim; }
static float                 synthVoiceTrim(int idx) { return vaVoiceTrim(idx); }
static const char           *synthTrimSymbol()   { return "kVaVoiceTrim"; }

static void synthBegin() {
    // Init the DaisySP voices now (NOT in their ctors) — after AudioMemory, ISR quiesced.
    AudioNoInterrupts();
    for (int i = 0; i < kVaVoices; ++i) g_vaVoice[i].begin();
    AudioInterrupts();

    g_vaSink.setPreset(0);
    g_vaSink.setMasterChannel(0);
    g_vaTrim.setGain(tdsp::auditionTrim(vaVoiceTrim(0)));

    const float g = 0.4f;   // 8 voices summed -> keep headroom
    g_vaMixA.gain(0, g); g_vaMixA.gain(1, g); g_vaMixA.gain(2, g); g_vaMixA.gain(3, g);
    g_vaMixB.gain(0, g); g_vaMixB.gain(1, g); g_vaMixB.gain(2, g); g_vaMixB.gain(3, g);
    g_vaMixSum.gain(0, 1.0f); g_vaMixSum.gain(1, 1.0f);

    g_player.setChannelMask(tdsp::MidiFilePlayer::kMaskNoDrums);
    Serial.printf("[synth] Virtual Analog ready: %d voices, %d presets, 3-axis MPE\n",
                  kVaVoices, DaisyVaSink::numPresets());
}
