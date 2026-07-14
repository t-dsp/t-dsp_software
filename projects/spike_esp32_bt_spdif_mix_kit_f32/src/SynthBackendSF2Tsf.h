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
#include "ReplayGain.h"      // shared K-weighted meter + ILoudnessMeter (Tier-1 sweep)
#include "SF2TsfGmTrim.h"    // per-GM-program trims (serves both tiers on this GM backend)

AudioSynthTsf        g_tsfSynth;                        // stereo int16: 0=L, 1=R
AudioConvert_I16toF32 g_tsfToF32L, g_tsfToF32R;
AudioConnection      c_tsfL(g_tsfSynth, 0, g_tsfToF32L, 0);
AudioConnection      c_tsfR(g_tsfSynth, 1, g_tsfToF32R, 0);
// Tier-1 audition trim: one bus gain (L+R set together) between the raw F32 and mix slot 3.
// The loudness probe (g_tsfLoud) taps the RAW L pre-trim so the 'N' sweep measures
// unnormalized program loudness. See REPLAYGAIN.md.
AudioEffectGain_F32  g_tsfTrimL, g_tsfTrimR;
AudioConnection_F32  c_tsfTrimInL(g_tsfToF32L, 0, g_tsfTrimL, 0);
AudioConnection_F32  c_tsfTrimInR(g_tsfToF32R, 0, g_tsfTrimR, 0);
AudioConnection_F32  c_tsfFL(g_tsfTrimL, 0, outL, 3);
AudioConnection_F32  c_tsfFR(g_tsfTrimR, 0, outR, 3);
tdsp::LoudnessProbe_F32 g_tsfLoud;                      // K-weighted meter on the RAW sum
AudioConnection_F32  c_tsfLoud(g_tsfToF32L, 0, g_tsfLoud, 0);

// --- MPE axis-proof capture probe -------------------------------------------
// Taps the TSF synth sum (left, pre-mix) so the shared runAxisProof (@PROOF) can
// capture a note and let the PC measure that an MPE axis really modulates the
// waveform — for the timbre axis, that CC#74 opens/closes the per-voice lowpass.
// Onset-capture only; the pool's full ClipProbe_F32 (K-weighting / clip / click
// detection) isn't needed for GM. Named ClipProbe_F32 so runAxisProof compiles
// against ClipProbe_F32::kCapN in both backends (only one is ever built). Buffer
// in DMAMEM (RAM2) — a 32 KB array in RAM1 starves the stack (see pool backend).
DMAMEM static float g_tsfCapBuf[8192];
class ClipProbe_F32 : public AudioStream_F32 {
public:
    static const int kCapN = 8192;                     // ~171 ms @ 48 kHz
    ClipProbe_F32(void) : AudioStream_F32(1, inputQueueArray) {}
    void update(void) override {
        audio_block_f32_t *b = receiveReadOnly_f32(0);
        if (!b) return;
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
            float s = b->data[i];
            if (m_arm && m_idx < kCapN) { g_tsfCapBuf[m_idx++] = s; if (m_idx >= kCapN) m_arm = false; }
        }
        AudioStream_F32::release(b);
    }
    void         armCapture(void)        { __disable_irq(); m_idx = 0; m_arm = true; __enable_irq(); }
    bool         captureDone(void) const { return !m_arm; }
    int          captureCount(void) const { return m_idx; }
    const float *capture(void)     const { return g_tsfCapBuf; }
private:
    audio_block_f32_t *inputQueueArray[1];
    volatile bool m_arm = false;
    volatile int  m_idx = 0;
};
ClipProbe_F32       dxpClip;
AudioConnection_F32 c_tsfClip(g_tsfToF32L, 0, dxpClip, 0);

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

// --- ReplayGain hooks (see REPLAYGAIN.md) ------------------------------------
// TSF is multitimbral during song playback, so it uses both tiers. Being General MIDI, the
// audition trim (index = GM program) and the song-norm trim (channel's GM program) read the
// SAME per-program loudness table (SF2TsfGmTrim.h) — one measurement serves both.
#define TDSP_HAS_REPLAYGAIN 1
#define TDSP_REPLAYGAIN_MULTITIMBRAL 1     // songStart neutralizes the audition trim
static tdsp::ILoudnessMeter *synthLoudness()     { return &g_tsfLoud; }
static AudioEffectGain_F32  *synthAuditionTrim() { return &g_tsfTrimL; }   // R set alongside in synthSetInstrument
static float                 synthVoiceTrim(int idx) { return sf2TsfGmTrim(idx); }
static const char           *synthTrimSymbol()   { return "kSf2TsfGmTrim"; }

// The app's single picker "auditions" one GM program on every melodic channel; a
// song's own Program Change events re-diversify per channel as it plays.
static void synthSetInstrument(int idx) {
    if (idx < 0) idx = 0;
    if (idx > 127) idx = 127;
    if (g_tsf) {
        AudioNoInterrupts();
        for (int ch = 0; ch < 16; ch++) {
            if (ch != 9) tsf_channel_set_presetnumber(g_tsf, ch, idx, 0);  // leave drum ch (9) alone
            tsf_channel_set_volume(g_tsf, ch, 1.0f);  // clear leftover MPE-pressure attenuation
        }
        AudioInterrupts();
    }
    g_synthInstrument = idx;
    // Tier-1 audition trim: all channels play this one GM program, so a single bus gain
    // (L+R together) normalizes it. Channel volume above stays 1.0 so it doesn't fight this.
    g_tsfTrimL.setGain(sf2TsfGmTrim(idx));
    g_tsfTrimR.setGain(sf2TsfGmTrim(idx));
    Serial.printf("[synth] all channels -> GM %d = %s (trim=%.3f)\n", idx, synthInstrumentName(idx), (double)sf2TsfGmTrim(idx));
}

// MPE mode hook (called by main.cpp applyMidiMode). MPE is single-timbre: every member
// channel (1..16, including 10) plays the ONE currently-selected instrument, so we point
// them all at g_synthInstrument. Normal MIDI restores GM drums on channel 10 and leaves
// the other channels' programs alone. Per-note bend range is handled by the router.
static void synthSetMpeMode(bool mpe) {
    if (!g_tsf) return;
    g_tsfSink.setMpe(mpe);   // gate CC74-as-cutoff: MPE = timbre axis, GM = ignore file CC74
    AudioNoInterrupts();
    for (int ch = 0; ch < 16; ch++) tsf_channel_midi_control(g_tsf, ch, 74, 127);  // timbre neutral (patch-open)
    if (mpe) {
        for (int ch = 0; ch < 16; ch++) tsf_channel_set_presetnumber(g_tsf, ch, g_synthInstrument, 0);
    } else {
        tsf_channel_set_presetnumber(g_tsf, 9, 0, 1);   // GM drums back on ch10
    }
    AudioInterrupts();
    Serial.printf("[synth] MPE %s\n", mpe ? "ON: all channels play the selected instrument"
                                         : "off: channel 10 = GM drums");
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
    g_tsfSink.setGmSongTrim(kSf2TsfGmTrim);   // Tier-2 per-GM-program song norm (unity until swept)

    // TSF renders GM drums on channel 10 itself, so pass every channel through.
    g_player.setChannelMask(tdsp::MidiFilePlayer::kMaskAll);
    Serial.printf("[synth] SF2 GM (TSF) ready: %d presets, 32 voices\n", tsf_get_presetcount(g_tsf));
}
