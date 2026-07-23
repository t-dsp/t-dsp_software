// DrumTsf.h — a small, dedicated TinySoundFont instance that renders GM drums on
// channel 10, so a MELODIC engine (Dexed pool, Plaits, …) can have drum grooves under it.
//
// The mix-kit builds ONE synth engine and the drum-groove player feeds that engine's
// sink — fine for a GM engine (TSF/SF2/OPL3/OPLL), but a melodic engine has no GM drum
// map, so drums are gated off (see drumEngineOk()). This adds a SECOND, drum-only TSF on
// the FREE output mix slot 2 (S/PDIF-in is dropped in the pool build via TDSP_NO_SPDIF_IN),
// fed by the groove player. The resident drum font needs PSRAM — hence a PSRAM board.
//
//   AudioSynthTsf(g_drumTsf, stereo int16) -> I16toF32 L/R -> outL/outR slot 2
//
// Enabled with -D TDSP_DRUM_TSF on a melodic build. Included by main.cpp AFTER outL/outR,
// g_sdReady, and the melodic backend exist. Loads DRUM_TSF_FONT_PATH (default a full GM
// font; only channel 10 / bank 128 is used — a drum-only SF2 works too and saves PSRAM).
#pragma once
#include <Audio.h>
#include <AudioSynthTsf.h>
#include "TsfSink.h"

#ifndef DRUM_TSF_FONT_PATH
#define DRUM_TSF_FONT_PATH "/sf2/gm_tsf.sf2"   // any GM font with a bank-128 kit (TimGM6mb baseline)
#endif

extern "C" uint8_t external_psram_size;        // Teensy core: detected PSRAM MB (0/8/16)

AudioSynthTsf         g_drumTsf;                        // stereo int16: 0=L, 1=R
AudioConvert_I16toF32 g_drumTsfToF32L, g_drumTsfToF32R;
AudioConnection       c_drumTsfBrL(g_drumTsf, 0, g_drumTsfToF32L, 0);
AudioConnection       c_drumTsfBrR(g_drumTsf, 1, g_drumTsfToF32R, 0);
AudioConnection_F32   c_drumTsfOutL(g_drumTsfToF32L, 0, outL, 2);   // slot 2 = free (no S/PDIF-in here)
AudioConnection_F32   c_drumTsfOutR(g_drumTsfToF32R, 0, outR, 2);

tsf            *g_drumTsfHandle = nullptr;
TsfSink         g_drumTsfSink(&g_drumTsfHandle);        // groove player's ch10 is routed here in setup()

// True once a font loaded; g_drumFontIsKits reports whether it was the acoustic
// /sf2/drumkits.sf2 (fetch_drumkits.py) rather than the GM baseline. setup() uses this to
// swap the Drums menu to the acoustic kit list (loadDrumKitsTsv) — the kits are bank-128
// presets by program, exactly like GM kits, so ch10 program-change selection is unchanged.
bool g_drumFontIsKits = false;
#define DRUM_KITS_FONT_PATH "/sf2/drumkits.sf2"        // acoustic multi-kit font (fetch_drumkits.py)

// Resident drum-font identity (for @STATE.drumfont + the app's @DRUMFONT picker). Set by
// drumTsfBegin() at boot and drumTsfReload() on a runtime swap.
char g_drumFontPath[72]    = "";
char g_drumFontDisplay[40] = "";

// Load the drum font, set channel 10 as GM drums, and open mix slot 2. Returns true if
// the font loaded (samples resident in PSRAM). Call from setup() after the melodic
// synthBegin() ran and g_sdReady is known.
static bool drumTsfBegin() {
    if (!g_sdReady) {
        Serial.println("[drumtsf] no SD card -> drums idle (need " DRUM_TSF_FONT_PATH ")");
        return false;
    }
    // Prefer the acoustic multi-kit font if it's on the card; else the GM baseline. The
    // acoustic kits are GM-note-layout, so the DrumNoteMapper's GmReduce still rescues the
    // Roland 22/26 hi-hats onto the real closed/open hat samples.
    const char *fontPath = DRUM_TSF_FONT_PATH;
    if (SD.exists((char *)DRUM_KITS_FONT_PATH)) { fontPath = DRUM_KITS_FONT_PATH; g_drumFontIsKits = true; }
    Serial.printf("[drumtsf] loading %s for ch10 drums (%d MB PSRAM installed)...\n",
                  fontPath, (int)external_psram_size);
    g_drumTsfHandle = tsfLoadFromSD(fontPath);
    if (!g_drumTsfHandle) {
        Serial.println("[drumtsf] load FAILED (missing font or too big for PSRAM) -> drums idle");
        g_drumFontIsKits = false;
        return false;
    }
    // Global TSF render gain (dB). Default -4 for normalized GM fonts; quiet fonts (e.g. the Mars
    // drum SF2, whose samples land ~-20 dBFS) can override with -D TDSP_DRUM_TSF_DB=<n> to boost.
#ifndef TDSP_DRUM_TSF_DB
#define TDSP_DRUM_TSF_DB -4.0f
#endif
    tsf_set_output(g_drumTsfHandle, TSF_STEREO_UNWEAVED, (int)AUDIO_SAMPLE_RATE_EXACT, (float)TDSP_DRUM_TSF_DB);
    tsf_set_max_voices(g_drumTsfHandle, 24);            // drums need fewer voices than a full melodic engine
    for (int ch = 0; ch < 16; ch++) {
        tsf_channel_set_presetnumber(g_drumTsfHandle, ch, 0, ch == 9 ? 1 : 0);  // tsf ch9 (MIDI 10) = drum bank 128
        tsf_channel_set_pitchrange(g_drumTsfHandle, ch, 48.0f);
    }
    g_drumTsf.begin(g_drumTsfHandle);
    g_drumTsf.setGain(1.0f);
    outL.gain(2, 0.62f); outR.gain(2, 0.62f);           // drum bus make-up (mirrors the synth slot); setMix leaves slot 2 alone under TDSP_DRUM_TSF
    strncpy(g_drumFontPath, fontPath, sizeof(g_drumFontPath) - 1); g_drumFontPath[sizeof(g_drumFontPath) - 1] = 0;
    strncpy(g_drumFontDisplay, g_drumFontIsKits ? "Drum Kits" : "GM Drums", sizeof(g_drumFontDisplay) - 1);
    Serial.printf("[drumtsf] ready: %d presets, ch10 GM drums -> mix slot 2\n",
                  tsf_get_presetcount(g_drumTsfHandle));
    return true;
}

// Runtime drum-font swap (@DRUMFONT): make a DIFFERENT SF2 the resident ch10 drum font. Loads
// the NEW font BEFORE freeing the old one (peak PSRAM = old+new; the Mars packs are small enough
// to overlap on 8 MB), configures it identically to drumTsfBegin(), then swaps the renderer + the
// groove player's sink atomically under AudioNoInterrupts() so the audio ISR never renders a
// half-updated or freed handle. Returns true on success (old font freed); false leaves the current
// font running untouched. The CALLER reloads the kit list + updates g_drumFontDisplay.
static bool drumTsfReload(const char *path) {
    if (!g_sdReady || !path || !path[0]) return false;
    tsf *nf = tsfLoadFromSD(path);
    if (!nf) { Serial.printf("[drumtsf] swap FAILED to load %s (missing or too big for PSRAM) -> keeping current\n", path); return false; }
    tsf_set_output(nf, TSF_STEREO_UNWEAVED, (int)AUDIO_SAMPLE_RATE_EXACT, (float)TDSP_DRUM_TSF_DB);
    tsf_set_max_voices(nf, 24);
    for (int ch = 0; ch < 16; ch++) {
        tsf_channel_set_presetnumber(nf, ch, 0, ch == 9 ? 1 : 0);
        tsf_channel_set_pitchrange(nf, ch, 48.0f);
    }
    tsf *old = g_drumTsfHandle;
    AudioNoInterrupts();
    g_drumTsfHandle = nf;          // g_drumTsfSink sends notes via &g_drumTsfHandle
    g_drumTsf.begin(nf);           // renderer now pulls from the new font
    AudioInterrupts();
    if (old && old != nf) tsf_close(old);
    strncpy(g_drumFontPath, path, sizeof(g_drumFontPath) - 1); g_drumFontPath[sizeof(g_drumFontPath) - 1] = 0;
    Serial.printf("[drumtsf] swapped -> %s (%d presets)\n", path, tsf_get_presetcount(nf));
    return true;
}
