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
    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf) { f.close(); return -1; }
    size_t got = f.read(buf, len);
    f.close();
    int n = (got == len) ? parseSmf(buf, len, out, maxOut) : -1;
    if (n > 0 && outBpm) *outBpm = initialBpm(buf, len);
    free(buf);
    return n;
}

} // namespace smf
} // namespace tdsp
