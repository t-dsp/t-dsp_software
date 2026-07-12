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

// Curated instrument list: index (sent by the app as @DXVOICE=<i>) -> a bundled
// DX7 patch (bank, voice) from dexed_banks_data.h. Bank 2 = the rom1a factory
// cartridge.
struct DxInstrument { uint8_t bank, voice; const char *name; };
static const DxInstrument kInstruments[] = {
    // Keys                                        (bank, voice-index)
    {2, 10, "E.Piano"},     {2,  7, "Grand Piano"}, {0,  0, "FM Rhodes"},
    {3,  2, "E.Piano 2"},   {2, 18, "Harpsichord"}, {2, 19, "Clav"},
    {3,  6, "Celeste"},
    // Organs
    {2, 16, "Organ"},       {2, 17, "Pipe Organ"},
    // Strings / ensemble
    {2,  3, "Strings"},     {6,  2, "String Ens"},  {2,  6, "Orchestra"},
    {8, 17, "Pizzicato"},
    // Brass
    {2,  0, "Brass"},       {6,  5, "Trumpet"},     {8, 11, "Synth Brass"},
    // Winds
    {2, 23, "Flute"},       {8,  5, "Pan Flute"},   {4,  2, "Oboe"},
    {4,  3, "Clarinet"},    {4,  4, "Sax"},         {4, 17, "Harmonica"},
    // Guitar / plucked
    {2, 11, "Guitar"},      {6, 14, "Jazz Guitar"}, {3, 21, "Sitar"},
    {3, 28, "Harp"},
    // Bass
    {2, 14, "Bass"},        {6, 11, "E.Bass"},      {6, 17, "Fretless"},
    // Synth / lead
    {2, 13, "Syn Lead"},    {0, 20, "Mini Moog"},   {0, 12, "Jupiter 8"},
    {0,  7, "Synclavier"},
    // Mallets / bells / perc
    {2, 20, "Vibes"},       {2, 21, "Marimba"},     {4, 23, "Xylophone"},
    {2, 25, "Tub Bells"},   {4, 21, "Glockenspiel"},{2, 26, "Steel Drum"},
    {2, 27, "Timpani"},
    // Voice
    {2, 29, "Voice"},       {1, 29, "Choir"},
};
static const int kNumInstruments = sizeof(kInstruments) / sizeof(kInstruments[0]);
static int g_synthInstrument = 0;

static const char *synthName()        { return "Dexed"; }
static const char *synthDescription() { return "6-op FM (DX7) synth, played by the MIDI IN port and the songs below."; }
static bool        synthIsGM()         { return false; }  // curated Dexed patch list -> names streamed to the app
static int         synthNumInstruments()        { return kNumInstruments; }
static const char *synthInstrumentName(int i)   { return kInstruments[i].name; }
static int         synthInstrument()            { return g_synthInstrument; }

// Load a curated instrument (runs from loop/handlers, never the audio ISR).
static void synthSetInstrument(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= kNumInstruments) idx = kNumInstruments - 1;
    const DxInstrument &in = kInstruments[idx];
    g_dexed.panic();
    if (tdsp::dexed::loadVoice(g_dexed, in.bank, in.voice)) {
        g_synthInstrument = idx;
        Serial.printf("[synth] instrument %d = %s (bank %d voice %d)\n", idx, in.name, in.bank, in.voice);
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
