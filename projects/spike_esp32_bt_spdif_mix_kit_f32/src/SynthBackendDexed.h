// SynthBackendDexed.h — Dexed (synth_dexed, 6-op DX7 FM) backend for the mix-kit.
//
// One of two interchangeable synth backends (see SynthBackendYmfm.h). main.cpp
// includes exactly one, chosen by the TDSP_SYNTH_* build flag, AFTER the mixers
// outL/outR are declared (this header wires the engine into mix slot 3).
//
// Both backends expose the same interface the mix-kit's player, SD catalog, and
// app control use, so nothing outside these headers is engine-specific:
//   tdsp::MidiSink *g_synthSink;   synthBegin(); synthSetInstrument(int);
//   synthNumInstruments(); synthInstrumentName(int); synthInstrument();
//   synthName(); synthDescription();
//
// Dexed is a MONO int16 engine: bridge to F32 and fan into both mix channels.
#pragma once
#include <synth_dexed.h>
#include "DexedSink.h"
#include "DexedVoiceBank.h"

AudioSynthDexed       g_dexed(16, AUDIO_SAMPLE_RATE_EXACT);   // 16-voice 6-op FM (int16 out)
AudioConvert_I16toF32 g_synthToF32;                           // int16 -> F32 bridge
AudioConnection       c_synthConv(g_dexed,      0, g_synthToF32, 0);
AudioConnection_F32   c_synthL   (g_synthToF32, 0, outL, 3);  // mono -> both channels
AudioConnection_F32   c_synthR   (g_synthToF32, 0, outR, 3);
DexedSink             g_dexedSink(&g_dexed);
tdsp::MidiSink       *g_synthSink = &g_dexedSink;

// Expose ALL bundled DX7 voices, browsable by bank: a global instrument index
// (sent by the app as @DXVOICE=<i>) maps to (bank, voice) across the 10 banks x
// 32 voices in dexed_banks_data.h — index = bank * kVoicesPerBank + voice, 320
// total. Names are streamed as "<bankName>: <voiceName>" so the app/control page
// can group the flat list back into per-bank sections by splitting on ": ".
static const int kNumInstruments = tdsp::dexed::kNumBanks * tdsp::dexed::kVoicesPerBank;  // 320
static int g_synthInstrument = 0;

static const char *synthName()        { return "Dexed"; }
static const char *synthDescription() { return "6-op FM (DX7) synth, played by the MIDI IN port and the songs below."; }
static bool        synthIsGM()         { return false; }  // full DX7 voice set -> names streamed to the app
static void        synthSetMpeMode(bool /*mpe*/) {}       // MPE not wired for this backend (router still bends)
static int         synthNumInstruments()        { return kNumInstruments; }
static int         synthInstrument()            { return g_synthInstrument; }

// "<bankName>: <voiceName>" for global index i. Returns a pointer to a shared
// static buffer — valid only until the next call, which is fine for the catalog
// stream (each name is printed before the next is fetched) and for logging.
static const char *synthInstrumentName(int i) {
    static char buf[32];
    int bank  = i / tdsp::dexed::kVoicesPerBank;
    int voice = i % tdsp::dexed::kVoicesPerBank;
    char vname[tdsp::dexed::kVoiceNameBufBytes];
    if (!tdsp::dexed::copyVoiceName(bank, voice, vname, sizeof(vname))) vname[0] = 0;
    snprintf(buf, sizeof(buf), "%s: %s", tdsp::dexed::bankName(bank), vname);
    return buf;
}

// Load a voice by global index (runs from loop/handlers, never the audio ISR).
static void synthSetInstrument(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= kNumInstruments) idx = kNumInstruments - 1;
    int bank  = idx / tdsp::dexed::kVoicesPerBank;
    int voice = idx % tdsp::dexed::kVoicesPerBank;
    g_dexed.panic();
    if (tdsp::dexed::loadVoice(g_dexed, bank, voice)) {
        g_synthInstrument = idx;
        Serial.printf("[synth] instrument %d = %s (bank %d voice %d)\n", idx, synthInstrumentName(idx), bank, voice);
    }
}

static void synthBegin() {
    // Dexed renders in float then saturates at the float->q15 rail; pull the
    // internal gain below unity so punchy notes don't flat-top (the 0.62 mixer
    // make-up in setup restores the level in the F32 domain).
    g_dexed.setGain(0.8f);
    g_dexedSink.setListenChannel(0);   // omni: one patch plays every channel
    g_dexed.setPitchbendRange(2);
    g_dexed.setPitchbend((int16_t)0);
    g_dexed.setModWheel(0);
    g_dexed.setSustain(false);
    synthSetInstrument(g_synthInstrument);
}
