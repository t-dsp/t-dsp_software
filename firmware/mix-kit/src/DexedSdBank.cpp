#include "DexedSdBank.h"

#include "tdsp_hw_config.h"

#include <Arduino.h>    // FLASHMEM
#include <string.h>

// All of this runs from loop/handlers (scan at boot, voice load on select) — never
// the audio ISR — so it lives in FLASH, not ITCM, to keep the RAM-tight pool build
// under the RAM1 ceiling. FLASHMEM tags are applied per function below.

#if TDSP_HAS_SDCARD
#include <SD.h>
#endif

namespace tdsp {
namespace dexed {

#if TDSP_HAS_SDCARD

namespace {

// One cached SD cartridge: its path (to re-read voices on demand), a display
// name, the payload offset, and the 32 voice names (cached at scan time).
struct SdBank {
    char     path[80];
    char     name[24];
    uint16_t payloadOffset;                          // 6 (SysEx) or 0 (raw)
    char     names[kVoicesPerBank][kVoiceNameBufBytes];
};

// Metadata cache + a single file scratch buffer, both in OCRAM (RAM2) to keep
// the ~30 KB off the tight DTCM budget.
DMAMEM SdBank  g_banks[kMaxSdBanks];
DMAMEM uint8_t g_scratch[4096];
int            g_numBanks = 0;

constexpr int kCartVoices   = 32;
constexpr int kVmemBytes    = 128;
constexpr int kPayloadBytes = kCartVoices * kVmemBytes;   // 4096
constexpr int kSysexTotal   = 6 + kPayloadBytes + 2;      // 4104
constexpr int kVmemNameOff  = 118;                        // name = VMEM bytes 118..127

FLASHMEM bool endsWithSyx(const char *s) {
    size_t n = s ? strlen(s) : 0;
    return n > 4 && strcasecmp(s + n - 4, ".syx") == 0;
}

FLASHMEM void voiceNameFromVmem(const uint8_t *vmem, char *out) {
    for (int i = 0; i < kVoiceNameLen; ++i) out[i] = (char)vmem[kVmemNameOff + i];
    out[kVoiceNameLen] = 0;
    for (int i = kVoiceNameLen - 1; i >= 0; --i) {   // trim trailing spaces
        if (out[i] == ' ') out[i] = 0; else break;
    }
}

// Recognizable 32-voice cart? Returns payload offset, or -1.
FLASHMEM int cartPayloadOffset(const uint8_t *hdr, uint32_t size) {
    if (size >= (uint32_t)kSysexTotal && hdr[0] == 0xF0 && hdr[1] == 0x43 && hdr[3] == 0x09)
        return 6;                                    // standard 32-voice bulk SysEx
    if (size == (uint32_t)kPayloadBytes)
        return 0;                                    // headerless raw dump
    return -1;
}

FLASHMEM void baseName(const char *path, char *out, int outLen) {
    const char *b = path;
    for (const char *p = path; *p; ++p) if (*p == '/' || *p == '\\') b = p + 1;
    int i = 0;
    for (; b[i] && i < outLen - 1; ++i) out[i] = b[i];
    out[i] = 0;
    int n = (int)strlen(out);
    if (n > 4 && strcasecmp(out + n - 4, ".syx") == 0) out[n - 4] = 0;
}

// Cache a cart from an already-open File + its full path. Reads the whole
// payload once to extract the 32 voice names. Returns true if added.
FLASHMEM bool addBank(File &f, const char *path) {
    if (g_numBanks >= kMaxSdBanks) return false;
    uint32_t size = f.size();
    uint8_t hdr[8];
    if (f.read(hdr, 8) != 8) return false;
    int off = cartPayloadOffset(hdr, size);
    if (off < 0) return false;
    f.seek(off);
    if (f.read(g_scratch, kPayloadBytes) < kPayloadBytes) return false;

    SdBank &bank = g_banks[g_numBanks];
    strncpy(bank.path, path, sizeof(bank.path) - 1); bank.path[sizeof(bank.path) - 1] = 0;
    baseName(path, bank.name, sizeof(bank.name));
    bank.payloadOffset = (uint16_t)off;
    for (int v = 0; v < kCartVoices; ++v)
        voiceNameFromVmem(g_scratch + v * kVmemBytes, bank.names[v]);
    g_numBanks++;
    return true;
}

} // namespace

FLASHMEM int scanSdBanks() {
    g_numBanks = 0;
    File d = SD.open("/dexed");
    if (!d || !d.isDirectory()) { if (d) d.close(); return 0; }
    for (File f = d.openNextFile(); f && g_numBanks < kMaxSdBanks; f = d.openNextFile()) {
        const char *nm = f.name();
        if (!f.isDirectory() && endsWithSyx(nm)) {
            char full[80];
            snprintf(full, sizeof(full), "/dexed/%s", nm ? nm : "");
            addBank(f, full);
        }
        f.close();
    }
    d.close();
    return g_numBanks;
}

FLASHMEM int  numSdBanks()  { return g_numBanks; }
FLASHMEM int  numSdVoices() { return g_numBanks * kVoicesPerBank; }

FLASHMEM const char *sdBankName(int bank) {
    if (bank < 0 || bank >= g_numBanks) return "?";
    return g_banks[bank].name;
}

FLASHMEM bool copySdVoiceName(int bank, int voice, char *out, int outLen) {
    if (bank < 0 || bank >= g_numBanks || voice < 0 || voice >= kVoicesPerBank
        || !out || outLen < kVoiceNameBufBytes) return false;
    strncpy(out, g_banks[bank].names[voice], outLen - 1);
    out[outLen - 1] = 0;
    return true;
}

FLASHMEM bool loadSdVoice(AudioSynthDexed &engine, int bank, int voice) {
    if (bank < 0 || bank >= g_numBanks || voice < 0 || voice >= kVoicesPerBank) return false;
    File f = SD.open(g_banks[bank].path);
    if (!f) return false;
    uint8_t packed[kVmemBytes];
    f.seek(g_banks[bank].payloadOffset + voice * kVmemBytes);
    int got = f.read(packed, kVmemBytes);
    f.close();
    if (got < kVmemBytes) return false;

    uint8_t decoded[156];
    if (!engine.decodeVoice(decoded, packed)) return false;
    engine.loadVoiceParameters(decoded);
    return true;
}

// --- Lazy directory browser --------------------------------------------------

namespace {
// Build "/dexed" or "/dexed/<rel>" into `buf`. Rejects "..", "//" and leading '/'
// in rel so a client can't escape /dexed.
FLASHMEM bool buildRelPath(const char *rel, char *buf, int bufLen) {
    if (rel && (strstr(rel, "..") || rel[0] == '/')) return false;
    if (rel && rel[0]) snprintf(buf, bufLen, "/dexed/%s", rel);
    else               snprintf(buf, bufLen, "/dexed");
    return true;
}
FLASHMEM bool isBrowsableCart(const char *nm, uint32_t size) {
    return nm && endsWithSyx(nm) && (size == 4104 || size == 4096);
}
} // namespace

FLASHMEM int sdListDir(const char *rel, int page, int pageSize, SdDirEntry *out, int *total) {
    if (total) *total = 0;
    char path[220];
    if (!buildRelPath(rel, path, sizeof(path))) return 0;
    File d = SD.open(path);
    if (!d || !d.isDirectory()) { if (d) d.close(); return 0; }

    const int startAt = page * pageSize;
    int matchIdx = 0, filled = 0;
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
        const char *nm = f.name();
        const bool  isDir = f.isDirectory();
        bool accept = false;
        if (isDir) {
            accept = nm && nm[0] != '.' && strcasecmp(nm, "System Volume Information") != 0;
        } else {
            accept = isBrowsableCart(nm, f.size());
        }
        if (accept) {
            if (matchIdx >= startAt && filled < pageSize) {
                strncpy(out[filled].name, nm ? nm : "", sizeof(out[filled].name) - 1);
                out[filled].name[sizeof(out[filled].name) - 1] = 0;
                out[filled].isDir = isDir;
                ++filled;
            }
            ++matchIdx;
        }
        f.close();
    }
    d.close();
    if (total) *total = matchIdx;
    return filled;
}

FLASHMEM int sdCartVoiceNames(const char *relCart, char names[kVoicesPerBank][kVoiceNameBufBytes]) {
    char path[220];
    if (!buildRelPath(relCart, path, sizeof(path))) return 0;
    File f = SD.open(path);
    if (!f || f.isDirectory()) { if (f) f.close(); return 0; }
    uint8_t hdr[8];
    if (f.read(hdr, 8) != 8) { f.close(); return 0; }
    int off = cartPayloadOffset(hdr, f.size());
    if (off < 0) { f.close(); return 0; }
    f.seek(off);
    int got = f.read(g_scratch, kPayloadBytes);
    f.close();
    if (got < kPayloadBytes) return 0;
    for (int v = 0; v < kCartVoices; ++v)
        voiceNameFromVmem(g_scratch + v * kVmemBytes, names[v]);
    return kCartVoices;
}

FLASHMEM bool sdLoadCartVoice(AudioSynthDexed &engine, const char *relCart, int voice) {
    if (voice < 0 || voice >= kCartVoices) return false;
    char path[220];
    if (!buildRelPath(relCart, path, sizeof(path))) return false;
    File f = SD.open(path);
    if (!f || f.isDirectory()) { if (f) f.close(); return false; }
    uint8_t hdr[8];
    if (f.read(hdr, 8) != 8) { f.close(); return false; }
    int off = cartPayloadOffset(hdr, f.size());
    if (off < 0) { f.close(); return false; }
    uint8_t packed[kVmemBytes];
    f.seek(off + voice * kVmemBytes);
    int got = f.read(packed, kVmemBytes);
    f.close();
    if (got < kVmemBytes) return false;

    uint8_t decoded[156];
    if (!engine.decodeVoice(decoded, packed)) return false;
    engine.loadVoiceParameters(decoded);
    return true;
}

#else  // no SD card on this board — stubs

int  scanSdBanks() { return 0; }
int  numSdBanks()  { return 0; }
int  numSdVoices() { return 0; }
const char *sdBankName(int) { return "?"; }
bool copySdVoiceName(int, int, char *, int) { return false; }
bool loadSdVoice(AudioSynthDexed &, int, int) { return false; }
int  sdListDir(const char *, int, int, SdDirEntry *, int *total) { if (total) *total = 0; return 0; }
int  sdCartVoiceNames(const char *, char[kVoicesPerBank][kVoiceNameBufBytes]) { return 0; }
bool sdLoadCartVoice(AudioSynthDexed &, const char *, int) { return false; }

#endif

} // namespace dexed
} // namespace tdsp
