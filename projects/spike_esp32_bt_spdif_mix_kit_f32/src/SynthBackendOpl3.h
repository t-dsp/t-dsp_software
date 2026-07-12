// SynthBackendOpl3.h — OPL3 (YMF262) + DMXOPL General-MIDI backend for the mix-kit.
//
// The third selectable synth backend (Dexed / ymfm OPM / OPL3), same interface as
// SynthBackendDexed.h. Unlike the OPM multitimbral backend (4 banks, hand-mapped
// arcade patches), OPL3 is a self-contained GM synth: one chip, 18 channels, the
// DMXOPL bank giving all 128 GM programs + real drums. So a MIDI song plays the
// RIGHT instrument per part with no per-bank routing here.
//
// Engine is stereo int16 -> bridge each channel to F32 into mix slot 3 (the
// mix-kit bus is F32). Included by main.cpp AFTER outL/outR + g_player + g_sdReady.
//
// PENDING: needs AudioSynthYmfmOPL3 in lib/TDspYmfm — see
// lib/TDspYmfm/OPL3_DMXOPL_SPEC.md. Until that lands, `-D TDSP_SYNTH_OPL3` will
// not compile (by design); the default envs (Dexed, ymfm) are unaffected.
#pragma once
#include <AudioSynthYmfmOPL3.h>
#include "Opl3Sink.h"

AudioSynthYmfmOPL3 g_opl3;                                // stereo int16: out 0=L, 1=R
AudioConvert_I16toF32 g_synthToF32L, g_synthToF32R;       // int16 -> F32 bridges (L, R)
AudioConnection     c_oplL(g_opl3, 0, g_synthToF32L, 0);
AudioConnection     c_oplR(g_opl3, 1, g_synthToF32R, 0);
AudioConnection_F32 c_synthL(g_synthToF32L, 0, outL, 3);
AudioConnection_F32 c_synthR(g_synthToF32R, 0, outR, 3);
Opl3Sink        g_oplSink(&g_opl3);
tdsp::MidiSink *g_synthSink = &g_oplSink;

static int g_synthInstrument = 0;   // app-picker "audition" program (0..127)

static const char *synthName()        { return "OPL3 (DMXOPL)"; }
static const char *synthDescription() { return "YMF262 OPL3 + DMXOPL General MIDI: multitimbral, a patch per channel, with drums."; }
static bool        synthIsGM()         { return true; }   // 128 standard GM programs -> app renders names locally
static int         synthNumInstruments()      { return g_opl3.numMelodic(); }             // 128 GM
static const char *synthInstrumentName(int i) { return g_opl3.melodicName(i); }
static int         synthInstrument()          { return g_synthInstrument; }

// The app's single picker "auditions" one GM program on every channel; a song's
// own Program Change events re-diversify per channel as it plays.
static void synthSetInstrument(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= g_opl3.numMelodic()) idx = g_opl3.numMelodic() - 1;
    for (uint8_t ch = 1; ch <= 16; ch++)
        if (ch != 10) g_opl3.programChange(ch, (uint8_t)idx);   // leave the drum channel alone
    g_synthInstrument = idx;
    Serial.printf("[synth] all channels -> GM %d = %s\n", idx, g_opl3.melodicName(idx));
}

// NOTE: the SD /opl/*.wopl bank override is deferred until AudioSynthYmfmOPL3::
// loadBankWopl() is implemented (it's currently a stub). A large DMAMEM read
// buffer here would starve the SD MIDI parser's malloc heap and break song
// loading — the baked DMXOPL bank is used until WOPL loading lands.

static void synthBegin() {
    g_opl3.begin();                    // reset chip + resampler, load baked DMXOPL bank
    g_opl3.setGain(3.2f);              // DMXOPL runs quiet; lift ~1.6x over the 2.0 default
                                       // (dense material still peaks well under clip)
    // OPL3 handles GM drums on channel 10 itself, so let the player pass every
    // channel through (the Dexed/OPM backends leave the default kMaskNoDrums).
    g_player.setChannelMask(tdsp::MidiFilePlayer::kMaskAll);
    Serial.printf("[synth] OPL3 (DMXOPL) ready: %d GM instruments + drums\n", g_opl3.numMelodic());
}
