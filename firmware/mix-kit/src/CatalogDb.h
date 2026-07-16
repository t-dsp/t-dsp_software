// CatalogDb.h — on-demand catalog database, built on the Teensy, written to /tdsp/.
//
// One command (@REINDEX) walks the SD content dirs and emits per-type NDJSON files
// under /tdsp/ so a client (control.html / the app) fetches them ONCE (via the @READ
// transport) and drives its whole UI locally — no more @INSTR/@DXLS runtime bursts.
// The DB is the source of truth; runtime serial then carries only ACTIONS (pick a
// voice, play a groove).
//
//   /tdsp/dexed.ndjson       one line per .syx cart, voice names INLINE
//   /tdsp/grooves.ndjson     one line per /drums/*.mid
//   /tdsp/songs.ndjson       one line per /songs/*.mid
//   /tdsp/soundfonts.ndjson  one line per /sf2/*.sf2  (+ bytes)
//   /tdsp/samples.ndjson     one line per /samples/<bank> subfolder
//   /tdsp/index.ndjson       manifest: version, build time, engine, per-type counts
//   /tdsp/.sig               internal upsert cache (per-source {count,bytes})
//
// Bundled (compile-time) lists — the engine's built-in voices and the GM drum kits —
// are written by the caller (main.cpp has them); see writeBundled* declared as hooks.
//
// Upsert: a cheap per-source signature (recursive file count + total bytes, no content
// reads) is cached in /tdsp/.sig. @REINDEX rebuilds only the sources whose signature
// changed (so re-reading 4000 carts is skipped when /dexed is untouched). Auto-builds
// at boot only if /tdsp/index.ndjson is missing.
#pragma once
#include <Arduino.h>
#include <SD.h>
// The /dexed cart walk (with inline voice names) is Dexed-only — DexedSdBank.cpp is
// excluded from non-Dexed builds, so guard the include + the walk. Every other source
// (grooves/songs/soundfonts/samples) is engine-agnostic.
#if defined(TDSP_SYNTH_DEXED) || defined(TDSP_SYNTH_DEXED_POOL)
  #define TDSP_CATDB_DEXED 1
  #include "DexedSdBank.h"
  #include "DexedVoiceBank.h"   // kVoicesPerBank / kVoiceNameBufBytes
#endif

extern bool g_sdReady;   // defined in main.cpp (SD mounted?)

namespace tdsp { namespace catdb {

static const char *kRoot = "/tdsp";
static const int   kMaxDepth = 5;          // /dexed nesting guard

// ---- small helpers ---------------------------------------------------------
static void jsonStr(Print &o, const char *s) {
    o.write('"');
    for (; s && *s; ++s) {
        uint8_t c = (uint8_t)*s;
        if (c == '"' || c == '\\') { o.write('\\'); o.write((char)c); }
        else if (c == '\n') o.print("\\n");
        else if (c == '\r') o.print("\\r");
        else if (c == '\t') o.print("\\t");
        else if (c < 0x20)  { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o.print(b); }
        else o.write((char)c);
    }
    o.write('"');
}

static bool endsWithCI(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    if (lf > ls) return false;
    s += ls - lf;
    for (size_t i = 0; i < lf; ++i)
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)suf[i])) return false;
    return true;
}

// strip a trailing extension (".mid"/".sf2"/".syx") into `out`
static void stripExt(char *out, int n, const char *name) {
    snprintf(out, n, "%s", name);
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
}

static void ensureRoot() { if (!SD.exists(kRoot)) SD.mkdir(kRoot); }

// ---- per-source signature (upsert) -----------------------------------------
struct Sig { uint32_t count; uint32_t bytes; };

// Recursive {matching-file count, total bytes} under `dir`. `ext`==nullptr counts
// every file; else only files ending in `ext`. No content is read.
static void sigOf(const char *dir, const char *ext, int depth, Sig &s) {
    if (depth > kMaxDepth) return;
    File d = SD.open(dir);
    if (!d) return;
    if (!d.isDirectory()) { d.close(); return; }
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
        const char *nm = f.name();
        if (f.isDirectory()) {
            char sub[160]; snprintf(sub, sizeof sub, "%s/%s", dir, nm);
            f.close();
            sigOf(sub, ext, depth + 1, s);
        } else {
            if (!ext || endsWithCI(nm, ext)) { s.count++; s.bytes += (uint32_t)f.size(); }
            f.close();
        }
    }
    d.close();
}

// Read/lookup a stored source signature from /tdsp/.sig ("<key> <count> <bytes>\n").
static bool readStoredSig(const char *key, Sig &out) {
    File f = SD.open("/tdsp/.sig");
    if (!f) return false;
    char line[80]; bool found = false;
    while (f.available()) {
        int n = f.readBytesUntil('\n', line, sizeof(line) - 1); line[n] = 0;
        char k[32]; unsigned long c = 0, b = 0;
        if (sscanf(line, "%31s %lu %lu", k, &c, &b) == 3 && strcmp(k, key) == 0) {
            out.count = (uint32_t)c; out.bytes = (uint32_t)b; found = true; break;
        }
    }
    f.close();
    return found;
}

// ---- dexed walk: one line per cart, voice names inline ---------------------
#ifdef TDSP_CATDB_DEXED
[[maybe_unused]] static void walkDexed(Print &out, const char *absDir, const char *relDir, int depth, uint32_t &nCarts) {
    if (depth > kMaxDepth) return;
    File d = SD.open(absDir);
    if (!d || !d.isDirectory()) { if (d) d.close(); return; }
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
        char nm[64]; snprintf(nm, sizeof nm, "%s", f.name());
        bool isDir = f.isDirectory();
        uint32_t sz = (uint32_t)f.size();
        f.close();                                         // close before recursing / opening the cart
        char rel[192];
        if (relDir[0]) snprintf(rel, sizeof rel, "%s/%s", relDir, nm);
        else           snprintf(rel, sizeof rel, "%s", nm);
        if (isDir) {
            char sub[200]; snprintf(sub, sizeof sub, "%s/%s", absDir, nm);
            walkDexed(out, sub, rel, depth + 1, nCarts);
        } else if (endsWithCI(nm, ".syx") && (sz == 4104 || sz == 4096)) {
            static char names[tdsp::dexed::kVoicesPerBank][tdsp::dexed::kVoiceNameBufBytes];
            int nv = tdsp::dexed::sdCartVoiceNames(rel, names);
            if (nv <= 0) continue;
            char base[64]; stripExt(base, sizeof base, nm);
            out.print("{\"type\":\"cart\",\"path\":\"/dexed/"); out.print(rel);   // rel has no leading slash
            out.print("\",\"folder\":"); jsonStr(out, relDir);
            out.print(",\"name\":"); jsonStr(out, base);
            out.print(",\"voices\":[");
            for (int i = 0; i < nv; ++i) { if (i) out.write(','); jsonStr(out, names[i]); }
            out.print("]}\n");
            nCarts++;
        }
    }
    d.close();
}
#endif // TDSP_CATDB_DEXED

// ---- flat scan: one line per matching file in `dir` ------------------------
// kind: emits {"type":kind,"path":...,"name":...[,"bytes":n]}
static uint32_t flatScan(Print &out, const char *dir, const char *ext, const char *kind, bool withBytes) {
    File d = SD.open(dir);
    if (!d || !d.isDirectory()) { if (d) d.close(); return 0; }
    uint32_t n = 0;
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
        const char *nm = f.name();
        if (!f.isDirectory() && endsWithCI(nm, ext)) {
            char base[80]; stripExt(base, sizeof base, nm);
            out.print("{\"type\":"); jsonStr(out, kind);
            out.print(",\"path\":\""); out.print(dir); out.write('/'); out.print(nm);
            out.print("\",\"name\":"); jsonStr(out, base);
            if (withBytes) { out.print(",\"bytes\":"); out.print((uint32_t)f.size()); }
            out.print("}\n"); n++;
        }
        f.close();
    }
    d.close();
    return n;
}

// ---- sample banks: one line per /samples/<bank> subfolder ------------------
static uint32_t scanSampleBanks(Print &out) {
    File d = SD.open("/samples");
    if (!d || !d.isDirectory()) { if (d) d.close(); return 0; }
    uint32_t n = 0;
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
        if (f.isDirectory()) {
            const char *nm = f.name();
            out.print("{\"type\":\"samplebank\",\"path\":\"/samples/"); out.print(nm);
            out.print("\",\"name\":"); jsonStr(out, nm); out.print("}\n"); n++;
        }
        f.close();
    }
    d.close();
    return n;
}

// ---- one source -> one /tdsp/<file>.ndjson (skips if signature unchanged) ---
// build() runs the writer only when the source changed; returns the line count
// (from the writer) or -1 if skipped. Writes the new signature to `newSig`.
template <typename WriterFn>
static long buildSource(const char *key, const char *outPath, const char *srcDir,
                        const char *sigExt, Print &sigOut, WriterFn writer) {
    Sig cur{0, 0};
    if (SD.exists(srcDir)) sigOf(srcDir, sigExt, 0, cur);
    Sig old{0, 0};
    bool have = readStoredSig(key, old);
    sigOut.print(key); sigOut.write(' '); sigOut.print(cur.count); sigOut.write(' '); sigOut.print(cur.bytes); sigOut.write('\n');
    if (have && old.count == cur.count && old.bytes == cur.bytes && SD.exists(outPath)) {
        Serial.printf("[catdb] %-11s unchanged (%lu files) -> keep\n", key, (unsigned long)cur.count);
        return -1;   // skipped
    }
    SD.remove(outPath);
    File o = SD.open(outPath, FILE_WRITE);
    if (!o) { Serial.printf("[catdb] %s: open FAILED\n", outPath); return 0; }
    long lines = writer(o);
    o.close();
    Serial.printf("[catdb] %-11s rebuilt: %ld rows\n", key, lines);
    return lines;
}

// Catalog schema/builder version. BUMP when the layout or content the builder writes
// changes (e.g. GM engines now ship instrument names) so setup()'s auto-reindex rebuilds
// a stale catalog even when the engine name is unchanged. v2: GM instrument names written.
// v3: meta gains "sf"/"dexed" capability flags (app fetches only relevant catalogs).
static const int kCatalogVersion = 3;

// Read the engine name ("engine":"...") and schema version ("v":N) stored in
// /tdsp/index.ndjson. `out` is set empty and *outVer is set 0 if the file/field is
// absent. Lets setup() detect an engine change OR a builder-version change across a
// reflash and auto-rebuild, so the app is never stuck on a stale catalog. Returns true
// if the engine name was read.
static bool readStoredEngine(char *out, size_t n, int *outVer = nullptr) {
    if (out && n) out[0] = 0;
    if (outVer) *outVer = 0;
    if (!::g_sdReady || !out || n == 0) return false;
    File f = SD.open("/tdsp/index.ndjson");
    if (!f) return false;
    char buf[256];
    int got = f.read(buf, sizeof(buf) - 1);   // v + engine are near the front of the one-line manifest
    f.close();
    if (got <= 0) return false;
    buf[got] = 0;
    if (outVer) { const char *v = strstr(buf, "\"v\":"); if (v) *outVer = atoi(v + 4); }
    const char *key = "\"engine\":\"";
    const char *p = strstr(buf, key);
    if (!p) return false;
    p += strlen(key);
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) out[i++] = *p++;
    out[i] = 0;
    return true;
}

// ---- top-level build -------------------------------------------------------
// EngineCaps: which SD catalogs THIS engine can actually use. Written to index.ndjson
// meta so the app fetches only what's needed (not everything on the card) and hides the
// UI for the rest. buildBundled: optional callback that writes /tdsp/instruments.ndjson
// + /tdsp/drumkits.ndjson (compile-time lists the caller owns). Returns true on success.
struct EngineCaps {
    const char *engineName;
    bool        hasDrums;         // engine renders ch10 drums -> grooves + drum kits relevant
    const char *drumEngine;       // parallel drum-voice label ("OPLL"/"TSF"), "" if the synth does its own
    bool        hasBt;            // ESP32 Bluetooth-audio receiver present -> app shows the BT card
    bool        hasSoundfonts;    // engine can browse+load /sf2 fonts as the MAIN synth (SF2/TSF)
    bool        hasDexedLibrary;  // engine uses the /dexed DX7 cart library (live-browsed)
};
typedef void (*BundledFn)();

static bool buildCatalog(const EngineCaps &caps, BundledFn buildBundled, uint32_t nowMs) {
    if (!::g_sdReady) { Serial.println("[catdb] no SD -> cannot build"); return false; }
    ensureRoot();
    Serial.println("[catdb] building /tdsp/ catalog...");
    uint32_t t0 = millis();

    // New signature file is accumulated as we build each source.
    SD.remove("/tdsp/.sig.tmp");
    File sig = SD.open("/tdsp/.sig.tmp", FILE_WRITE);
    if (!sig) { Serial.println("[catdb] .sig open FAILED"); return false; }

    // The /dexed cart library is browsed LIVE via @DXLS/@DXVL (lazy, paged, no RAM), never
    // bulk-shipped in the catalog: at ~11k carts the inline-voice-name NDJSON is ~6 MB — too
    // big for a client to @READ on every connect (it timed out and the library came back
    // empty). Drop any stale dexed.ndjson so clients don't fetch it; the manifest below then
    // reports it absent. (walkDexed is kept, unused, so this can be re-enabled if ever needed.)
    long dexed = -1;
    SD.remove("/tdsp/dexed.ndjson");
    long grooves = buildSource("grooves", "/tdsp/grooves.ndjson", "/drums", ".mid", sig,
        [](Print &o) -> long { return (long)flatScan(o, "/drums", ".mid", "groove", false); });
    // songs.ndjson is written by the bundled hook from the firmware's g_songs play registry
    // (built-ins + SD), so its row index == the @SONG=<i> the player must send. A bare
    // flatScan(/songs) would mis-index (extra built-ins, different order).
    long songs = -1;
    long sf2 = buildSource("soundfonts", "/tdsp/soundfonts.ndjson", "/sf2", ".sf2", sig,
        [](Print &o) -> long { return (long)flatScan(o, "/sf2", ".sf2", "soundfont", true); });
    long samples = buildSource("samples", "/tdsp/samples.ndjson", "/samples", nullptr, sig,
        [](Print &o) -> long { return (long)scanSampleBanks(o); });

    sig.close();
    SD.remove("/tdsp/.sig"); SD.rename("/tdsp/.sig.tmp", "/tdsp/.sig");

    // Bundled (compile-time) lists — the caller writes instruments.ndjson + drumkits.ndjson.
    if (buildBundled) buildBundled();

    // Manifest (always rewritten). Counts come from the freshly-computed signatures when a
    // source was skipped we still know its file exists; report -1 as "unchanged".
    SD.remove("/tdsp/index.ndjson");
    File ix = SD.open("/tdsp/index.ndjson", FILE_WRITE);
    if (ix) {
        ix.print("{\"v\":"); ix.print(kCatalogVersion); ix.print(",\"built\":"); ix.print(nowMs);
        ix.print(",\"engine\":"); jsonStr(ix, caps.engineName);
        ix.print(",\"drums\":"); ix.print(caps.hasDrums ? "true" : "false");
        if (caps.drumEngine && caps.drumEngine[0]) { ix.print(",\"drumEngine\":"); jsonStr(ix, caps.drumEngine); }
        // Capability flags -> the app fetches only the catalogs this engine can use and hides the
        // UI for the rest. Absent (old firmware) -> client assumes present (back-compat).
        ix.print(",\"bt\":"); ix.print(caps.hasBt ? "true" : "false");
        ix.print(",\"sf\":"); ix.print(caps.hasSoundfonts ? "true" : "false");
        ix.print(",\"dexed\":"); ix.print(caps.hasDexedLibrary ? "true" : "false");
        ix.print(",\"files\":[");
        const char *fs[] = {"dexed","grooves","songs","soundfonts","samples","instruments","drumkits"};
        for (int i = 0; i < 7; ++i) {
            if (i) ix.write(',');
            char p[40]; snprintf(p, sizeof p, "/tdsp/%s.ndjson", fs[i]);
            // Announce each file's byte size so a client can total "how much data" up
            // front (index.ndjson is fetched first) and show a real load-progress bar.
            uint32_t fb = 0; { File pf = SD.open(p); if (pf) { fb = (uint32_t)pf.size(); pf.close(); } }
            ix.print("{\"type\":"); jsonStr(ix, fs[i]);
            ix.print(",\"path\":"); jsonStr(ix, p);
            ix.print(",\"present\":"); ix.print(SD.exists(p) ? "true" : "false");
            ix.print(",\"bytes\":"); ix.print(fb); ix.print("}");
        }
        ix.print("]}\n");
        ix.close();
    }
    Serial.printf("[catdb] done in %lu ms (dexed=%ld grooves=%ld songs=%ld sf2=%ld samples=%ld)\n",
                  (unsigned long)(millis() - t0), dexed, grooves, songs, sf2, samples);
    return true;
}

}} // namespace tdsp::catdb
