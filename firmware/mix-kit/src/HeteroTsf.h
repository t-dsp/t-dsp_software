// HeteroTsf.h — a MELODIC SoundFont (TinySoundFont) track for a heterogeneous engine inventory.
//
// "Synth F": a second TSF instance (the first renders the sampled drums, DrumTsf.h) that loads ONE
// melodic font from the card and plays it as a full Track peer — own player / arp / router, live MIDI
// and MPE (TsfSink: per-channel bend over ±48, pressure -> channel volume, CC74 -> lowpass). Built
// for the sampled HANDPAN (tools/fetch_handpan.py -> /sf2/handpan.sf2), but any SF2 works: every
// preset in the font is a catalog entry ("<Font>: <preset>").
//
//   g_playerV[v]/router[v] -> arp[v] -> g_htSink -> g_htTsf (stereo int16) -> I16toF32 L/R
//     -> g_htTrim (per-track level) -> g_hpSubMix in 2/3 (the Plaits tracks' sub-mix) -> mix slot
//
// Mix slot: all four main slots are taken (OPLL / Plaits / drums / Dexed), so this track sums into
// the Plaits sub-mix (inputs 2+3, L+R at 0.5 each -> that bus is mono). The font's samples live in
// PSRAM (the T-DSP TSF patch keeps them int16): budget it against the drum font (fetch_handpan.py
// --rate/--seconds/--vel-layers trims to a few MB).
//
// Build flags (env): -D TDSP_TSF_ENGINES=1 (count; only 1 supported), -D TDSP_TSF_FONT_PATH="/sf2/handpan.sf2".
// Track index: after Dexed + OPLL + Plaits (kDexedVoices + kOpllVoices + TDSP_PLAITS_ENGINES).
#pragma once
#if TDSP_HETERO && TDSP_TSF_ENGINES >= 1
#include <Audio.h>
#include <AudioSynthTsf.h>
#include "TsfSink.h"

static_assert(TDSP_TSF_ENGINES == 1, "HeteroTsf.h wires exactly one melodic SoundFont track");
#ifndef TDSP_TSF_FONT_PATH
#define TDSP_TSF_FONT_PATH "/sf2/handpan.sf2"
#endif
#ifndef TDSP_TSF_FONT_LABEL
#define TDSP_TSF_FONT_LABEL "Handpan"       // "<label>: <preset>" in the voice browser (one folder)
#endif
#ifndef TDSP_TSF_MAX_VOICES
#define TDSP_TSF_MAX_VOICES 16              // a handpan rarely needs more than ~10 ringing notes
#endif

AudioSynthTsf         g_htTsf;                          // stereo int16: 0=L, 1=R
AudioConvert_I16toF32 g_htToF32L, g_htToF32R;
AudioEffectGain_F32   g_htTrim;                         // per-track user level (0..1.5); mono sum of L+R
AudioMixer4_F32       g_htSum;                          // L+R -> mono (0.5 each) before the trim
AudioConnection       c_ht_brL (g_htTsf, 0, g_htToF32L, 0);
AudioConnection       c_ht_brR (g_htTsf, 1, g_htToF32R, 0);
AudioConnection_F32   c_ht_sumL(g_htToF32L, 0, g_htSum, 0);
AudioConnection_F32   c_ht_sumR(g_htToF32R, 0, g_htSum, 1);
AudioConnection_F32   c_ht_trim(g_htSum, 0, g_htTrim, 0);
#if TDSP_HETERO_PLAITS && TDSP_PLAITS_ENGINES >= 2
AudioConnection_F32   c_ht_out (g_htTrim, 0, g_hpSubMix, 2);   // joins the Plaits sub-mix -> shared slot
#else
#error "HeteroTsf.h expects the Plaits sub-mix (TDSP_PLAITS_ENGINES >= 2) to sum into; add a slot route for other inventories"
#endif

tsf     *g_htHandle = nullptr;
TsfSink  g_htSink(&g_htHandle);
static int  g_htInstrument = 0;              // current preset index in the font
static int  g_htVolPct     = 100;
static bool g_htReady      = false;
static char g_htFontDisp[24] = TDSP_TSF_FONT_LABEL;

static int heteroTsfNumInstruments() { return g_htHandle ? tsf_get_presetcount(g_htHandle) : 0; }
static const char *heteroTsfInstrumentName(int idx) {
    static char buf[64];
    if (!g_htHandle || idx < 0 || idx >= tsf_get_presetcount(g_htHandle)) return "";
    const char *pn = tsf_get_presetname(g_htHandle, idx);
    snprintf(buf, sizeof(buf), "%s: %s", g_htFontDisp, (pn && pn[0]) ? pn : "preset");
    return buf;
}
static int heteroTsfInstrument() { return g_htInstrument; }
// Put one preset on every channel (the track is mono-timbral; a song's Program Change still
// re-diversifies per channel through TsfSink::onProgramChange).
FLASHMEM static void heteroTsfSetInstrument(int idx) {
    if (!g_htHandle) return;
    const int n = tsf_get_presetcount(g_htHandle);
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    AudioNoInterrupts();
    for (int ch = 0; ch < 16; ch++) tsf_channel_set_presetindex(g_htHandle, ch, idx);
    AudioInterrupts();
    g_htInstrument = idx;
    Serial.printf("[hetero-tsf] all channels -> %s\n", heteroTsfInstrumentName(idx));
}
static void heteroTsfSetVol(int pct) {
    if (pct < 0) pct = 0; if (pct > 150) pct = 150;
    g_htVolPct = pct;
    g_htTrim.setGain(pct / 100.0f);
}
static void heteroTsfSetMpe(bool mpe) { g_htSink.setMpe(mpe); }

// Load the font (whole SF2 into PSRAM) and bring the track up. Called from setup() after the SD card
// is mounted and the Plaits tracks exist (their sub-mix is our output). Returns true when the font
// loaded; without it the track stays silent and the app greys the card (unavail).
FLASHMEM static bool heteroTsfBegin() {
    g_htSum.gain(0, 0.5f); g_htSum.gain(1, 0.5f);
    g_htTrim.setGain(1.0f);
    g_hpSubMix.gain(2, 1.0f);
    if (!g_sdReady) { Serial.println("[hetero-tsf] no SD card -> " TDSP_TSF_FONT_LABEL " track idle"); return false; }
    if (!SD.exists((char *)TDSP_TSF_FONT_PATH)) {
        Serial.println("[hetero-tsf] " TDSP_TSF_FONT_PATH " not on the card -> track idle (run tools/fetch_handpan.py, push, reboot)");
        return false;
    }
    Serial.println("[hetero-tsf] loading " TDSP_TSF_FONT_PATH " into PSRAM...");
    g_htHandle = tsfLoadFromSD(TDSP_TSF_FONT_PATH);
    if (!g_htHandle) { Serial.println("[hetero-tsf] load FAILED (too big for the remaining PSRAM?) -> track idle"); return false; }
    tsf_set_output(g_htHandle, TSF_STEREO_UNWEAVED, (int)AUDIO_SAMPLE_RATE_EXACT, -3.0f);
    tsf_set_max_voices(g_htHandle, TDSP_TSF_MAX_VOICES);   // pre-alloc so note-on never reallocs under the lock
    for (int ch = 0; ch < 16; ch++) {
        tsf_channel_set_presetindex(g_htHandle, ch, 0);
        tsf_channel_set_pitchrange(g_htHandle, ch, 48.0f);  // TsfSink maps bend over ±48 (covers MPE)
        tsf_channel_midi_control(g_htHandle, ch, 74, 127);   // timbre neutral (filter open) until MPE drives it
    }
    g_htTsf.begin(g_htHandle);
    g_htTsf.setGain(1.0f);
    g_htReady = true;
    heteroTsfSetInstrument(0);
    Serial.printf("[hetero-tsf] ready: %d preset(s) from " TDSP_TSF_FONT_PATH " -> Plaits sub-mix\n", tsf_get_presetcount(g_htHandle));
    return true;
}
#endif  // TDSP_HETERO && TDSP_TSF_ENGINES
