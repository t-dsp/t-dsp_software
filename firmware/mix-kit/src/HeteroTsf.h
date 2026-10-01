// HeteroTsf.h — a MELODIC SoundFont (TinySoundFont) track for a heterogeneous engine inventory.
//
// "Synth F": a second TSF instance (the first renders the sampled drums, DrumTsf.h). It is the box's
// general SoundFont synth: it plays any MELODIC .sf2 on the card (drum-only fonts -- every preset in
// the SF2 percussion bank 128 -- belong to the drum track and are hidden here). The voice catalog is
// one folder per font ("<Font>: ..."), so the app's picker shows the fonts as folders:
//
//   "<Font>: <preset>"              the presets of the font that is LOADED right now (bank 0 first)
//                                   -- pick one and it plays
//   "<Font>: Load font (N.N MB)"    every other melodic font on the card -- pick it and the track
//                                   UNLOADS the current font, loads that one into PSRAM (~0.3 s/MB;
//                                   audio keeps running), selects its first preset and re-pushes the
//                                   voice list so the folder fills with its presets
//   "Too big for this device: <Font> (N.N MB)"   the fonts that won't fit the free PSRAM, in one folder (refused)
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
struct HtFont { char path[40]; char disp[22]; uint32_t bytes; uint16_t presets; bool melodic; };
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
// Peek an SF2's preset headers WITHOUT loading it: walk the RIFF chunks (seeking over the sample data),
// count presets, and flag the font melodic when any preset sits outside the percussion banks. A few SD
// reads per font. Returns false for something that isn't a readable SF2.
FLASHMEM static bool htPeekFont(const char *path, uint16_t &presets, bool &melodic) {
    presets = 0; melodic = false;
    File f = SD.open(path);
    if (!f) return false;
    uint8_t h[12];
    if (f.read(h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "sfbk", 4)) { f.close(); return false; }
    auto rd32 = [](const uint8_t *q) { return (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24); };
    const uint32_t end = f.size();
    uint32_t pos = 12;
    bool ok = false;
    while (pos + 8 <= end) {
        f.seek(pos);
        uint8_t ch[12];
        if (f.read(ch, 8) != 8) break;
        const uint32_t sz = rd32(ch + 4);
        if (!memcmp(ch, "LIST", 4) && f.read(ch + 8, 4) == 4 && !memcmp(ch + 8, "pdta", 4)) {
            uint32_t sp = pos + 12; const uint32_t send = pos + 8 + sz;
            while (sp + 8 <= send) {
                f.seek(sp);
                uint8_t sh[8];
                if (f.read(sh, 8) != 8) break;
                const uint32_t ssz = rd32(sh + 4);
                if (!memcmp(sh, "phdr", 4)) {
                    const uint32_t n = ssz / 38;                 // incl. the terminal EOP record
                    for (uint32_t i = 0; i + 1 < n; i++) {
                        uint8_t rec[38];
                        f.seek(sp + 8 + i * 38);
                        if (f.read(rec, 38) != 38) break;
                        const uint16_t bank = (uint16_t)(rec[22] | (rec[23] << 8));
                        presets++;
                        if (bank < 120) melodic = true;         // 128 (and 127 in some fonts) = percussion
                    }
                    ok = true;
                    break;
                }
                sp += 8 + ssz + (ssz & 1);
            }
            break;
        }
        pos += 8 + sz + (sz & 1);
    }
    f.close();
    return ok;
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
                if (!htPeekFont(F.path, F.presets, F.melodic) || !F.melodic) { e.close(); continue; }   // drum-only / unreadable: not ours
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

// The loaded font is excluded from the "Load font" entries (its presets ARE its folder).
static bool htIsLoaded(const HtFont &F) { return g_htReady && !strcmp(F.path, g_htFontPath); }
static int  htOtherFontCount() { int n = 0; for (int i = 0; i < g_htFontCount; i++) if (!htIsLoaded(g_htFonts[i])) n++; return n; }
static bool htFits(const HtFont &F) { return F.bytes <= g_htPsramFreeCached + g_htFontBytes; }   // the current font is freed first (cached: see htPsramFree)
static const HtFont *htOtherFont(int k) {   // k-th font that is NOT the loaded one: the ones that FIT first, then the too-big ones
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < g_htFontCount; i++) {
            if (htIsLoaded(g_htFonts[i]) || htFits(g_htFonts[i]) != (pass == 0)) continue;
            if (k-- == 0) return &g_htFonts[i];
        }
    return nullptr;
}
static int heteroTsfNumInstruments() { return g_htPresetCount + htOtherFontCount(); }
static const char *heteroTsfInstrumentName(int idx) {
    static char buf[96];   // "<disp>: Too big for PSRAM (11.7 MB, 263 presets)" needs > 64;
    if (idx < 0) return "";
    if (idx < g_htPresetCount) {
        const char *pn = g_htHandle ? tsf_get_presetname(g_htHandle, g_htPresetMap[idx]) : nullptr;
        snprintf(buf, sizeof(buf), "%s: %s", g_htFontDisp, (pn && pn[0]) ? pn : "preset");
        return buf;
    }
    const HtFont *F = htOtherFont(idx - g_htPresetCount);
    if (!F) return "";
    // A font that fits is its own folder ("<Font>: Load font …"); the ones this device can't hold are
    // gathered under ONE folder so they don't clutter the picker but are still visible.
    const unsigned long mb = F->bytes / 1000000UL, tenth = (F->bytes / 100000UL) % 10;
    if (htFits(*F)) snprintf(buf, sizeof(buf), "%s: Load font (%lu.%lu MB, %u presets)", F->disp, mb, tenth, (unsigned)F->presets);
    else            snprintf(buf, sizeof(buf), "Too big for this device: %s (%lu.%lu MB)", F->disp, mb, tenth);
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
    const HtFont *Fp = htOtherFont(idx - g_htPresetCount);
    if (!Fp) return;
    const HtFont &F = *Fp;
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
    htPsramFree();   // refresh the cached free figure (one ~280 ms walk, only here) so the "Load font" /
                     // "Too big" tags reflect what has been allocated since boot (song buffers, rings …)
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
// Two-step bring-up. heteroTsfBegin() wires the mix + scans the card + grabs this track's PSRAM scratch;
// heteroTsfLoadBootFont() loads the boot font and must be setup()'s LAST PSRAM allocation: a font swap
// frees the resident font and the new one needs ONE contiguous block, so the resident font has to sit
// right below the free top of the PSRAM heap. (With the 1.7 MB drum font allocated after it, freeing the
// 1.4 MB handpan left two holes and a 4.1 MB GM font failed with 4.8 MB "free" -- hardware, 2026-10-01.)
FLASHMEM static bool heteroTsfBegin() {
    g_htSum.gain(0, 0.5f); g_htSum.gain(1, 0.5f);
    g_htTrim.setGain(1.0f);
    g_hpSubMix.gain(2, 1.0f);
    if (!g_sdReady) { Serial.println("[hetero-tsf] no SD card -> SoundFont track idle"); return false; }
    htScanFonts();
    if (!g_htLine) g_htLine = (char *)extmem_malloc(12288);   // the pushed voice-list line: allocate BEFORE any font
    Serial.printf("[hetero-tsf] %d melodic font(s) on the card (drum-only fonts hidden)\n", g_htFontCount);
    return g_htFontCount > 0;
}
FLASHMEM static bool heteroTsfLoadBootFont() {
    if (!g_sdReady) return false;
    if (!SD.exists((char *)TDSP_TSF_FONT_PATH)) {
        Serial.println("[hetero-tsf] " TDSP_TSF_FONT_PATH " not on the card -> pick a font from the voice list (run tools/fetch_handpan.py for the handpan)");
        htPsramFree();             // one walk, so the font entries can be tagged
        g_htCatalogDirty = true;   // the "Load font" entries are still pickable
        return false;
    }
    return heteroTsfLoadFont(TDSP_TSF_FONT_PATH);
}
#endif  // TDSP_HETERO && TDSP_TSF_ENGINES
