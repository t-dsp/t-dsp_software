// SynthBackendSF2Tsf.h — full-fidelity SF2 General-MIDI backend via TinySoundFont.
//
// The full-featured counterpart to SynthBackendSF2.h (sf22aswt/AudioSynthWavetable).
// TSF is a COMPLETE SF2 renderer — velocity layers, region layering, all generators,
// filters, envelopes, GM drums — so there is no per-generator glue here; we just load
// the font and forward MIDI channels (see TsfSink). Trade-off: the whole font is
// resident in PSRAM (samples as float), so it needs a COMPACT font that fits — put it
// on the SD as /sf2/gm_tsf.sf2 (GeneralUser's 30 MB will NOT fit; see the plan).
//
//   AudioSynthTsf (stereo int16) --> AudioConvert_I16toF32 --> mix slot 3 (L, R)
//
// Included by main.cpp AFTER outL/outR + g_player + g_sdReady.
#pragma once
#include <Audio.h>
#include <AudioSynthTsf.h>
#include "TsfSink.h"

AudioSynthTsf        g_tsfSynth;                        // stereo int16: 0=L, 1=R
AudioConvert_I16toF32 g_tsfToF32L, g_tsfToF32R;
AudioConnection      c_tsfL(g_tsfSynth, 0, g_tsfToF32L, 0);
AudioConnection      c_tsfR(g_tsfSynth, 1, g_tsfToF32R, 0);
AudioConnection_F32  c_tsfFL(g_tsfToF32L, 0, outL, 3);
AudioConnection_F32  c_tsfFR(g_tsfToF32R, 0, outR, 3);

tsf            *g_tsf = nullptr;                        // loaded in synthBegin
TsfSink         g_tsfSink(&g_tsf);
tdsp::MidiSink *g_synthSink = &g_tsfSink;

static int g_synthInstrument = 0;   // app-picker "audition" program (0..127)

static const char *synthName()        { return "SF2 GM (TSF)"; }
static const char *synthDescription() { return "Full-fidelity General MIDI via TinySoundFont: velocity layers, region layering, filters, GM drums (compact SF2 resident in PSRAM)."; }
static bool        synthIsGM()         { return true; }   // 128 standard GM programs -> app renders names locally
static int         synthNumInstruments()      { return 128; }   // GM melodic (bank 0)
static const char *synthInstrumentName(int i) {
    const char *n = (g_tsf ? tsf_bank_get_presetname(g_tsf, 0, i) : nullptr);
    return (n && *n) ? n : "-";
}
static int         synthInstrument()          { return g_synthInstrument; }

// The app's single picker "auditions" one GM program on every melodic channel; a
// song's own Program Change events re-diversify per channel as it plays.
static void synthSetInstrument(int idx) {
    if (idx < 0) idx = 0;
    if (idx > 127) idx = 127;
    if (g_tsf) {
        AudioNoInterrupts();
        for (int ch = 0; ch < 16; ch++)
            if (ch != 9) tsf_channel_set_presetnumber(g_tsf, ch, idx, 0);  // leave drum ch (9) alone
        AudioInterrupts();
    }
    g_synthInstrument = idx;
    Serial.printf("[synth] all channels -> GM %d = %s\n", idx, synthInstrumentName(idx));
}

// MPE mode hook (called by main.cpp applyMidiMode). MPE puts one note per member
// channel 2..16, so channel 10 must be melodic, not the GM drum kit; normal MIDI
// keeps ch10 as drums. Per-note bend range is handled by the router (48 semis).
static void synthSetMpeMode(bool mpe) {
    if (!g_tsf) return;
    AudioNoInterrupts();
    tsf_channel_set_presetnumber(g_tsf, 9, mpe ? g_synthInstrument : 0, mpe ? 0 : 1);
    AudioInterrupts();
    Serial.printf("[synth] MPE %s: channel 10 -> %s\n", mpe ? "ON" : "off", mpe ? "melodic" : "drums");
}

static void synthBegin() {
    if (!g_sdReady) {
        Serial.println("[synth] TSF: no SD card -> engine idle (need /sf2/gm_tsf.sf2)");
        return;
    }
    Serial.println("[synth] TSF: loading /sf2/gm_tsf.sf2 (compact SF2 into PSRAM)...");
    g_tsf = tsfLoadFromSD("/sf2/gm_tsf.sf2");
    if (!g_tsf) {
        Serial.println("[synth] TSF: load FAILED (missing, or too big for PSRAM) -> engine idle");
        return;
    }
    tsf_set_output(g_tsf, TSF_STEREO_UNWEAVED, (int)AUDIO_SAMPLE_RATE_EXACT, -4.0f);
    tsf_set_max_voices(g_tsf, 32);                     // pre-alloc so note-on never reallocs
    // Pre-create all 16 channels; channel 9 (MIDI 10) is drums (bank 128).
    for (int ch = 0; ch < 16; ch++) {
        tsf_channel_set_presetnumber(g_tsf, ch, 0, ch == 9 ? 1 : 0);
        tsf_channel_set_pitchrange(g_tsf, ch, 48.0f); // TsfSink maps bend over +-48 (covers MPE)
    }
    g_tsfSynth.begin(g_tsf);
    g_tsfSynth.setGain(1.0f);

    // TSF renders GM drums on channel 10 itself, so pass every channel through.
    g_player.setChannelMask(tdsp::MidiFilePlayer::kMaskAll);
    Serial.printf("[synth] SF2 GM (TSF) ready: %d presets, 32 voices\n", tsf_get_presetcount(g_tsf));
}
