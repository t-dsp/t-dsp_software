// MidiSmfFile.h — SD-card convenience loader for MidiSmfParser.
//
// Kept separate from MidiSmfParser.h so the core parser stays free of any SD
// dependency (parseSmf() is pure and unit-testable off-target). Include this
// header only from a translation unit that has SD available; it pulls in
// <SD.h> itself, so include order does not matter.
#pragma once
#include <SD.h>
#include "MidiSmfParser.h"

namespace tdsp {
namespace smf {

// Load /path off the SD card and parse it into out[0..maxOut). Returns the
// event count, or -1 on error (open/size/read/parse failure). Runs from the
// main loop only (heap alloc + SD I/O; never the audio ISR).
// `outBpm` (optional) receives the file's initial tempo in BPM — used to lock a
// drum groove to a playing song. Left untouched on failure.
static int loadSmfFile(const char *path, MidiFileEvent *out, int maxOut, float *outBpm = nullptr) {
    File f = SD.open(path, FILE_READ);
    if (!f) return -1;
    size_t len = f.size();
    if (len < 14 || len > 512UL * 1024) { f.close(); return -1; }   // sanity cap 512 KB

    // Read the whole file into RAM for parseSmf() (it needs random access across
    // tracks). PREFER reusing the caller's `out` buffer as the read scratch: it's
    // idle until parse's final emit, and parseSmf() only READS the file bytes in
    // its early passes — by the time it WRITES `out` the bytes are spent, so the
    // aliasing is safe. This avoids a second large heap block: on a no-PSRAM board
    // the file malloc + parseSmf's event scratch together exhausted the OCRAM heap
    // and every /songs load failed (the tiny /drums files still fit). initialBpm()
    // must run BEFORE parseSmf() overwrites the bytes. Fall back to malloc for a
    // file too big for `out` (e.g. a small drum buffer) so no caller regresses.
    const size_t cap = (size_t)maxOut * sizeof(MidiFileEvent);
    bool owned = false;
    uint8_t *buf;
    if (len <= cap) {
        buf = (uint8_t *)out;                       // reuse output buffer — no heap
    } else {
        buf = (uint8_t *)malloc(len);               // file bigger than out: heap it
        if (!buf) { f.close(); return -1; }
        owned = true;
    }
    size_t got = f.read(buf, len);
    f.close();
    if (got != len) { if (owned) free(buf); return -1; }
    if (outBpm) *outBpm = initialBpm(buf, len);     // before parse reuses the bytes
    int n = parseSmf(buf, len, out, maxOut);
    if (owned) free(buf);
    return n;
}

} // namespace smf
} // namespace tdsp
