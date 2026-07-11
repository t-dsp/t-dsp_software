// SynthBackendYmfm.h — ymfm OPM (YM2151, 8-voice 4-op FM) backend for the mix-kit.
//
// The BSD-3-Clause counterpart to SynthBackendDexed.h (same interface; see that
// file's header). main.cpp includes exactly one, chosen by the build flag, AFTER
// the mixers outL/outR are declared.
//
// OPM is a genuinely STEREO int16 engine (out 0=L, 1=R): bridge each channel to
// F32 separately into the stereo mix slot 3 (not fanned like mono Dexed).
#pragma once
#include <AudioSynthYmfmOPM.h>
#include "YmfmSink.h"

AudioSynthYmfmOPM     g_opm;                                  // stereo int16: out 0=L, 1=R
AudioConvert_I16toF32 g_synthToF32L, g_synthToF32R;           // int16 -> F32 bridges (L, R)
AudioConnection       c_synthConvL(g_opm,         0, g_synthToF32L, 0);
AudioConnection       c_synthConvR(g_opm,         1, g_synthToF32R, 0);
AudioConnection_F32   c_synthL    (g_synthToF32L, 0, outL, 3);
AudioConnection_F32   c_synthR    (g_synthToF32R, 0, outR, 3);
YmfmSink              g_ymfmSink(&g_opm);
tdsp::MidiSink       *g_synthSink = &g_ymfmSink;

// OPM patch bank (from lib/TDspYmfm). Index maps to @DXVOICE / 'V'.
static const tdsp::ymfmopm::OpmVoice *kVoices[] = {
    &tdsp::ymfmopm::kAdditiveOrgan,
    &tdsp::ymfmopm::kElectricPiano,
};
static const int kNumVoices = sizeof(kVoices) / sizeof(kVoices[0]);
static int g_synthInstrument = 0;

static const char *synthName()        { return "ymfm OPM"; }
static const char *synthDescription() { return "YM2151 (OPM) 4-op FM synth, played by the MIDI IN port and the songs below."; }
static int         synthNumInstruments()        { return kNumVoices; }
static const char *synthInstrumentName(int i)   { return kVoices[i]->name; }
static int         synthInstrument()            { return g_synthInstrument; }

static void synthSetInstrument(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= kNumVoices) idx = kNumVoices - 1;
    g_opm.allNotesOff();
    g_opm.setVoice(*kVoices[idx]);
    g_synthInstrument = idx;
    Serial.printf("[synth] voice %d = %s\n", idx, kVoices[idx]->name);
}

static void synthBegin() {
    g_opm.begin();                     // reset chip, size resampler, load default voice
    g_ymfmSink.setListenChannel(0);    // omni: one patch plays every channel
    synthSetInstrument(g_synthInstrument);
}
