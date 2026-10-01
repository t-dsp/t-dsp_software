// HeteroTsf.h — a MELODIC SoundFont (TinySoundFont) track for a heterogeneous engine inventory.
//
// "Synth F": a second TSF instance (the first renders the sampled drums, DrumTsf.h). It is the box's
// general SoundFont synth: it plays ANY .sf2 on the card. The voice catalog has two folders:
//
//   "<Font>: <preset>"   the presets of the font that is loaded right now (bank 0 first; a drum-only
//                        font lists its kits) -- pick one and it plays
//   "Fonts: <name>"      every /sf2/*.sf2 on the card -- pick one and the track UNLOADS the current
//                        font, loads that one into PSRAM (a few seconds; audio keeps running), selects
//                        its first preset and re-pushes the voice list so the app re-lists. Fonts that
//                        won't fit the free PSRAM are tagged "(too big)" and refused.
//
// Boot font = TDSP_TSF_FONT_PATH (the sampled handpan, tools/fetch_handpan.py). RING mode (ignore
// note-off, let a struck note decay to the sample end) switches on automatically for fonts named
// hang/handpan and is exposed as @TRK<i>.RING=0|1. MPE via TsfSink (bend ±48, pressure -> volume,
// CC74 -> lowpass). Output sums into the Plaits sub-mix (all four main mix slots are taken).
//
// Build flags (env): -D TDSP_TSF_ENGINES=1, -D TDSP_TSF_FONT_PATH="/sf2/handpan.sf2", -D TDSP_TSF_FONT_LABEL="Handpan".
#pragma once
#if TDSP_HETERO && TDSP_TSF_ENGINES >= 1
#include <Audio.h>
#include <AudioSynthTsf.h>
#include <smalloc.h>
#include "TsfSink.h"

extern "C" { extern struct smalloc_pool extmem_smalloc_pool; }

static_assert(TDSP_TSF_ENGINES == 1, "HeteroTsf.h wires exactly one melodic SoundFont track");
#ifndef TDSP_TSF_FONT_PATH
#define TDSP_TSF_FONT_PATH "/sf2/handpan.sf2"
#endif
#ifndef TDSP_TSF_FONT_LABEL
#define TDSP_TSF_FONT_LABEL "Handpan"
#endif
#ifndef TDSP_TSF_MAX_VOICES
#define TDSP_TSF_MAX_VOICES 24              // ringing handpan notes stack up; TSF steals past this
#endif
#define HT_TRACK_INDEX (kDexedVoices + kOpllVoices + TDSP_PLAITS_ENGINES)

AudioSynthTsf         g_htTsf;                          // stereo int16: 0=L, 1=R
AudioConvert_I16toF32 g_htToF32L, g_htToF32R;
AudioEffectGain_F32   g_htTrim;                         // per-track user level (0..1.5)
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
static int   g_htInstrument = 0;             // catalog index of the current preset
static int   g_htVolPct     = 100;
static bool  g_htReady      = false;
static bool  g_htRing       = false;
static char  g_htFontPath[40] = TDSP_TSF_FONT_PATH;
static char  g_htFontDisp[22] = TDSP_TSF_FONT_LABEL;
static uint32_t g_htFontBytes = 0;

// Fonts on the card (scanned at boot; display names from /sf2/fonts.tsv when listed there).
struct HtFont { char path[40]; char disp[22]; uint32_t bytes; };
static const int kHtMaxFonts = 96;
static HtFont *g_htFonts = nullptr;          // PSRAM
static int     g_htFontCount = 0;
// Catalog index -> TSF preset index for the loaded font (bank-0 presets first, else all).
static uint8_t g_htPresetMap[256];
static int     g_htPresetCount = 0;
static volatile bool g_htCatalogDirty = false;
static char   *g_htLine = nullptr;           // PSRAM scratch for the pushed @TRK.INSTRS line
static Print  *g_htPush = nullptr;           // the control broadcast (`ctrl`, defined later in main.cpp; set in setup())

// Free PSRAM. EXPENSIVE: sm_malloc_stats_pool() steps through the free space one 16-byte header at a
// time (CRC-checking each) — with ~6 MB free that is ~400k uncached PSRAM reads = hundreds of ms per
// call. Never call it per catalog entry (that stalled the main loop for a minute while listing 59
// fonts and chopped the USB reply); refresh the cached figure only around a font load/unload.
static size_t g_htPsramFreeCached = 0;
static size_t htPsramFree() {
    size_t total = 0, used = 0, fr = 0; int blocks = 0;
    if (external_psram_size == 0) return 0;
    sm_malloc_stats_pool(&extmem_smalloc_pool, &total, &used, &fr, &blocks);
    g_htPsramFreeCached = fr;
    return fr;
}
static bool htNameHasRing(const char *path) {   // *hang* / *handpan* -> ring by default
    char low[48]; size_t n = 0;
    for (const char *p = path; *p && n + 1 < sizeof(low); p++) low[n++] = (char)tolower((unsigned char)*p);
    low[n] = 0;
    return strstr(low, "hang") || strstr(low, "handpan");
}
FLASHMEM static void htDisplayFor(const char *path, char *out, int outLen) {
    out[0] = 0;
    File f = SD.open("/sf2/fonts.tsv");
    if (f) {
        while (f.available()) {
            String ln = f.readStringUntil('\n'); ln.trim();
            if (!ln.length() || ln[0] == '#') continue;
            int t1 = ln.indexOf('\t'); if (t1 < 0) continue;
            if (ln.substring(0, t1) == path) {
                int t2 = ln.indexOf('\t', t1 + 1);
                String d = (t2 > t1) ? ln.substring(t1 + 1, t2) : ln.substring(t1 + 1);
                d.trim(); snprintf(out, outLen, "%s", d.c_str()); break;
            }
        }
        f.close();
    }
    if (!out[0]) {
        const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
        snprintf(out, outLen, "%s", b);
        char *dot = strrchr(out, '.'); if (dot) *dot = 0;
        if (!strcasecmp(out, "handpan")) snprintf(out, outLen, "%s", TDSP_TSF_FONT_LABEL);
    }
}
FLASHMEM static void htScanFonts() {
    if (!g_htFonts) g_htFonts = (HtFont *)extmem_malloc(sizeof(HtFont) * kHtMaxFonts);
    if (!g_htFonts) return;
    g_htFontCount = 0;
    File d = SD.open("/sf2");
    if (!d || !d.isDirectory()) return;
    for (;;) {
        File e = d.openNextFile();
        if (!e) break;
        if (!e.isDirectory()) {
            const char *nm = e.name(); const char *b = strrchr(nm, '/'); b = b ? b + 1 : nm;
            size_t L = strlen(b);
            if (L > 4 && !strcasecmp(b + L - 4, ".sf2") && L + 5 < sizeof(g_htFonts[0].path) && g_htFontCount < kHtMaxFonts) {
                HtFont F; snprintf(F.path, sizeof(F.path), "/sf2/%s", b); F.bytes = e.size();
                htDisplayFor(F.path, F.disp, sizeof(F.disp));
                int i = g_htFontCount++;                    // insertion sort by display name
                while (i > 0 && strcasecmp(g_htFonts[i - 1].disp, F.disp) > 0) { g_htFonts[i] = g_htFonts[i - 1]; i--; }
                g_htFonts[i] = F;
            }
        }
        e.close();
    }
    d.close();
}
FLASHMEM static void htBuildPresetMap() {
    g_htPresetCount = 0;
    if (!g_htHandle) return;
    for (int prog = 0; prog < 128 && g_htPresetCount < 256; prog++) {      // bank 0 (melodic) first
        int pi = tsf_get_presetindex(g_htHandle, 0, prog);
        if (pi >= 0) g_htPresetMap[g_htPresetCount++] = (uint8_t)pi;
    }
    if (g_htPresetCount == 0) {                                             // drum-only font: list everything
        int n = tsf_get_presetcount(g_htHandle);
        for (int i = 0; i < n && g_htPresetCount < 256; i++) g_htPresetMap[g_htPresetCount++] = (uint8_t)i;
    }
}

static int heteroTsfNumInstruments() { return g_htPresetCount + g_htFontCount; }
static const char *heteroTsfInstrumentName(int idx) {
    static char buf[64];
    if (idx < 0) return "";
    if (idx < g_htPresetCount) {
        const char *pn = g_htHandle ? tsf_get_presetname(g_htHandle, g_htPresetMap[idx]) : nullptr;
        snprintf(buf, sizeof(buf), "%s: %s", g_htFontDisp, (pn && pn[0]) ? pn : "preset");
        return buf;
    }
    const int fi = idx - g_htPresetCount;
    if (fi >= g_htFontCount) return "";
    const HtFont &F = g_htFonts[fi];
    const bool cur  = g_htReady && !strcmp(F.path, g_htFontPath);
    const bool fits = cur || F.bytes <= g_htPsramFreeCached + g_htFontBytes;   // the current font is freed first (cached: see htPsramFree)
    snprintf(buf, sizeof(buf), "Fonts: %s%s", F.disp, cur ? " (loaded)" : fits ? "" : " (too big)");
    return buf;
}
static int heteroTsfInstrument() { return g_htInstrument; }

static void heteroTsfSetRing(bool r) { g_htRing = r; g_htSink.setRing(r); }
static void heteroTsfSetVol(int pct) {
    if (pct < 0) pct = 0; if (pct > 150) pct = 150;
    g_htVolPct = pct;
    g_htTrim.setGain(pct / 100.0f);
}
static void heteroTsfSetMpe(bool mpe) { g_htSink.setMpe(mpe); }

// Put catalog preset idx (< g_htPresetCount) on every channel (mono-timbral track).
FLASHMEM static void htSelectPreset(int idx) {
    if (!g_htHandle || idx < 0 || idx >= g_htPresetCount) return;
    AudioNoInterrupts();
    for (int ch = 0; ch < 16; ch++) tsf_channel_set_presetindex(g_htHandle, ch, g_htPresetMap[idx]);
    AudioInterrupts();
    g_htInstrument = idx;
    Serial.printf("[hetero-tsf] all channels -> %s\n", heteroTsfInstrumentName(idx));
}
// Attach a freshly loaded font to the renderer/sink.
FLASHMEM static void htAttach(tsf *nf, const char *path) {
    tsf_set_output(nf, TSF_STEREO_UNWEAVED, (int)AUDIO_SAMPLE_RATE_EXACT, -3.0f);
    tsf_set_max_voices(nf, TDSP_TSF_MAX_VOICES);
    for (int ch = 0; ch < 16; ch++) {
        tsf_channel_set_presetindex(nf, ch, 0);
        tsf_channel_set_pitchrange(nf, ch, 48.0f);
        tsf_channel_midi_control(nf, ch, 74, 127);
    }
    AudioNoInterrupts();
    g_htHandle = nf;
    g_htTsf.begin(nf);
    AudioInterrupts();
    strncpy(g_htFontPath, path, sizeof(g_htFontPath) - 1); g_htFontPath[sizeof(g_htFontPath) - 1] = 0;
    htDisplayFor(path, g_htFontDisp, sizeof(g_htFontDisp));
    g_htReady = true;
    htBuildPresetMap();
    heteroTsfSetRing(htNameHasRing(path));
    htSelectPreset(0);
    g_htCatalogDirty = true;
}
// Swap fonts: free the current one FIRST (PSRAM holds one font + the drum font), then load. If the new
// font fails (too big / unreadable) reload the previous one so the track never goes silent by accident.
FLASHMEM static bool heteroTsfLoadFont(const char *path) {
    if (!g_sdReady || !path || !path[0]) return false;
    char prev[40]; strncpy(prev, g_htFontPath, sizeof(prev) - 1); prev[sizeof(prev) - 1] = 0;
    const bool hadPrev = g_htReady;
    tsf *old = g_htHandle;
    AudioNoInterrupts();
    g_htHandle = nullptr; g_htTsf.begin(nullptr);   // renderer idles (null-safe) while we work
    AudioInterrupts();
    g_htReady = false; g_htPresetCount = 0;
    if (old) tsf_close(old);
    Serial.printf("[hetero-tsf] loading %s (%lu KB PSRAM free)...\n", path, (unsigned long)(htPsramFree() / 1024));
    uint32_t t0 = millis();
    tsf *nf = tsfLoadFromSD(path);
    if (nf) {
        File f = SD.open(path); g_htFontBytes = f ? (uint32_t)f.size() : 0; if (f) f.close();
        htAttach(nf, path);
        const uint32_t t1 = millis(); htPsramFree();   // refresh the cached free figure (one slow walk, logged)
        Serial.printf("[hetero-tsf] ready: %s, %d preset(s) in %lu ms, ring=%d (%lu KB PSRAM free, stats %lu ms)\n", g_htFontDisp,
                      tsf_get_presetcount(nf), (unsigned long)(t1 - t0), g_htRing ? 1 : 0,
                      (unsigned long)(g_htPsramFreeCached / 1024), (unsigned long)(millis() - t1));
        return true;
    }
    Serial.printf("[hetero-tsf] load FAILED for %s (too big for PSRAM, or unreadable)\n", path);
    if (hadPrev && strcmp(prev, path) != 0) {
        tsf *back = tsfLoadFromSD(prev);
        if (back) { htAttach(back, prev); Serial.printf("[hetero-tsf] kept previous font %s\n", g_htFontDisp); }
    }
    htPsramFree();   // refresh the cache (nothing / the previous font is resident now)
    g_htCatalogDirty = true;
    return false;
}

// Catalog pick: a preset of the loaded font, or a font to load.
FLASHMEM static void heteroTsfSetInstrument(int idx) {
    if (idx < 0) idx = 0;
    if (idx < g_htPresetCount) { htSelectPreset(idx); return; }
    const int fi = idx - g_htPresetCount;
    if (fi >= g_htFontCount) return;
    const HtFont &F = g_htFonts[fi];
    if (g_htReady && !strcmp(F.path, g_htFontPath)) { htSelectPreset(0); return; }   // already loaded
    if (F.bytes > htPsramFree() + g_htFontBytes) {
        Serial.printf("[hetero-tsf] %s is %lu KB; only %lu KB PSRAM would be free -> not loading\n", F.disp,
                      (unsigned long)(F.bytes / 1024), (unsigned long)((htPsramFree() + g_htFontBytes) / 1024));
        g_htCatalogDirty = true;   // re-push so the app's selection snaps back
        return;
    }
    heteroTsfLoadFont(F.path);     // blocking (seconds); the app shows the delay, audio keeps running
}

// loop(): after a font swap push the refreshed voice list + state on every lane (one write per lane so a
// lane either gets the whole line or nothing — see CtrlBroadcast).
FLASHMEM static void heteroTsfService() {
    if (!g_htCatalogDirty) return;
    g_htCatalogDirty = false;
    if (!g_htLine) g_htLine = (char *)extmem_malloc(12288);
    if (!g_htLine) return;
    int n = snprintf(g_htLine, 12288, "@TRK%d.INSTRS=", (int)HT_TRACK_INDEX);
    const int total = heteroTsfNumInstruments();
    for (int k = 0; k < total && n < 12288 - 80; k++) {
        if (k) g_htLine[n++] = '\x1f';
        n += snprintf(g_htLine + n, 12288 - n, "%s", heteroTsfInstrumentName(k));
    }
    if (n < 12288 - 1) g_htLine[n++] = '\n';
    if (!g_htPush) return;
    g_htPush->write((const uint8_t *)g_htLine, n);
    g_htPush->printf("@TRK%d.INSTR=%d\n", (int)HT_TRACK_INDEX, g_htInstrument);
}

// setup(): scan the card's fonts and load the boot font.
FLASHMEM static bool heteroTsfBegin() {
    g_htSum.gain(0, 0.5f); g_htSum.gain(1, 0.5f);
    g_htTrim.setGain(1.0f);
    g_hpSubMix.gain(2, 1.0f);
    if (!g_sdReady) { Serial.println("[hetero-tsf] no SD card -> SoundFont track idle"); return false; }
    htScanFonts();
    Serial.printf("[hetero-tsf] %d font(s) on the card\n", g_htFontCount);
    if (!SD.exists((char *)TDSP_TSF_FONT_PATH)) {
        Serial.println("[hetero-tsf] " TDSP_TSF_FONT_PATH " not on the card -> pick a font from the voice list (run tools/fetch_handpan.py for the handpan)");
        htPsramFree();             // one walk, so the Fonts: entries can be tagged (too big)
        g_htCatalogDirty = true;   // the Fonts: entries are still pickable
        return false;
    }
    return heteroTsfLoadFont(TDSP_TSF_FONT_PATH);
}
#endif  // TDSP_HETERO && TDSP_TSF_ENGINES
