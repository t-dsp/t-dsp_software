// SynthBackendRings.h — Mutable-Rings-style modal/string resonator backend.
//
// DaisySP's ModalVoice + StringVoice (Electrosmith, MIT; lib/TDspRings) — a
// physical-modelling resonator with full 3-axis MPE (per-note bend, pressure ->
// ring length, CC#74 -> brightness). The picker selects Modal vs String mode.
//
// CPU note: the 24-mode modal resonator is ~32% PER VOICE (no cheap idle path
// beyond our gate), so this backend runs only 2 voices — the mix-kit's BT +
// S/PDIF resamplers already take a big slice. AudioSynthRings' idle gate means
// released notes free their CPU as they ring out, so 2 simultaneous is the cap.
//
// Engine is mono int16 -> F32 bridge -> mix slot 3. Included by main.cpp after
// outL/outR + g_player + g_sdReady.
#pragma once
#include <TDspRings.h>             // AudioSynthRings + RingsSink
#include <AudioEffectGain_F32.h>
#include "ReplayGain.h"
#include "RingsVoiceTrim.h"

static constexpr int kRingsVoices = 2;   // ~32% CPU/voice — 2 leaves room for BT+S/PDIF

AudioSynthRings     g_rVoice[kRingsVoices];               // one resonator each (mono int16)
AudioMixer4         g_rMix;                               // sum the voices to mono
AudioConnection     c_rv0(g_rVoice[0], 0, g_rMix, 0);
AudioConnection     c_rv1(g_rVoice[1], 0, g_rMix, 1);
AudioConvert_I16toF32   g_synthToF32;                     // int16 -> F32 (mono)
AudioConnection     c_rMix(g_rMix, 0, g_synthToF32, 0);
tdsp::LoudnessProbe_F32 g_rProbe;                         // K-weighted meter on the RAW (pre-trim) sum
AudioConnection_F32 c_rProbe(g_synthToF32, 0, g_rProbe, 0);
AudioEffectGain_F32 g_rTrim;                              // Tier-1 audition bus gain
AudioConnection_F32 c_rTrimIn(g_synthToF32, 0, g_rTrim, 0);
AudioConnection_F32 c_synthL(g_rTrim, 0, outL, 3);        // mono -> both mix channels
AudioConnection_F32 c_synthR(g_rTrim, 0, outR, 3);

RingsSink::VoicePorts g_rPorts[kRingsVoices] = {
    { &g_rVoice[0] }, { &g_rVoice[1] },
};
RingsSink       g_ringsSink(g_rPorts, kRingsVoices);
tdsp::MidiSink *g_synthSink = &g_ringsSink;

static int g_synthInstrument = 0;   // app-picker mode: 0 Modal, 1 String

static const char *synthName()        { return "Rings (Modal/String MPE)"; }
static const char *synthDescription() { return "DaisySP modal + sympathetic-string physical-modelling resonator — full 3-axis MPE (per-note bend + pressure + brightness)."; }
static bool        synthIsGM()         { return false; }

static void synthSetMpeMode(bool mpe) {
    g_ringsSink.onAllNotesOff(0);
    g_ringsSink.setMasterChannel(mpe ? 1 : 0);
}

// Picker "instruments" = the two resonator modes.
static int         synthNumInstruments()      { return AudioSynthRings::kNumModes; }
static const char *synthInstrumentName(int i) { return AudioSynthRings::modeName((uint8_t)i); }
static int         synthInstrument()          { return g_synthInstrument; }

static void synthSetInstrument(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= AudioSynthRings::kNumModes) idx = AudioSynthRings::kNumModes - 1;
    g_ringsSink.setMode((uint8_t)idx);
    g_synthInstrument = idx;
    g_rTrim.setGain(tdsp::auditionTrim(ringsVoiceTrim(idx)));
    Serial.printf("[synth] mode -> %d \"%s\"\n", idx, AudioSynthRings::modeName((uint8_t)idx));
}

// --- ReplayGain hooks (Tier-1 only; single-timbre engine) --------------------
#define TDSP_HAS_REPLAYGAIN 1
static tdsp::ILoudnessMeter *synthLoudness()     { return &g_rProbe; }
static AudioEffectGain_F32  *synthAuditionTrim() { return &g_rTrim; }
static float                 synthVoiceTrim(int idx) { return ringsVoiceTrim(idx); }
static const char           *synthTrimSymbol()   { return "kRingsVoiceTrim"; }

static void synthBegin() {
    // Init the DaisySP voices now (NOT in their ctors) — after AudioMemory, with
    // the audio ISR quiesced, so DaisySP Init can't race the update ISR.
    AudioNoInterrupts();
    for (int i = 0; i < kRingsVoices; ++i) g_rVoice[i].begin();
    AudioInterrupts();

    g_ringsSink.setMode(0);          // Modal
    g_ringsSink.setStructure(0.5f);
    g_ringsSink.setBrightness(0.5f);
    g_ringsSink.setDamping(0.7f);    // longer default ring
    g_ringsSink.setBrightnessDepth(1.0f);
    g_ringsSink.setMasterChannel(0);
    g_rTrim.setGain(tdsp::auditionTrim(ringsVoiceTrim(0)));

    g_rMix.gain(0, 0.7f); g_rMix.gain(1, 0.7f);

    g_player.setChannelMask(tdsp::MidiFilePlayer::kMaskNoDrums);
    Serial.printf("[synth] Rings ready: %d voices, Modal/String, 3-axis MPE\n", kRingsVoices);
}
