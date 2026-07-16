// MidiSmfFile.h — SD-card convenience loader for MidiSmfParser.
//
// Kept separate from MidiSmfParser.h so the core parser stays free of any SD
// dependency (parseSmf() is pure and unit-testable off-target). Include this
// header only from a translation unit that has SD available; it pulls in
// <SD.h> itself, so include order does not matter.
#pragma once
#include <SD.h>
#include "MidiSmfParser.h"

#if defined(__IMXRT1062__)
// Teensy 4.1 core: PSRAM (EXTMEM) allocator + the MB detected at boot. Used to keep
// the whole-file song scratch OUT of the small OCRAM heap the audio engine shares.
extern "C" uint8_t external_psram_size;
extern "C" void *extmem_malloc(size_t);
extern "C" void  extmem_free(void *);
// OCRAM (RAM2) malloc-heap bounds, for a pre-alloc safety check on no-PSRAM boards.
extern "C" unsigned long _heap_start;
extern "C" unsigned long _heap_end;
extern "C" char *__brkval;
#endif

namespace tdsp {
namespace smf {

// Bytes still allocatable from the OCRAM heap (current sbrk break -> heap top). This
// heap is SHARED with the audio engine, so loading a whole song file into it without
// margin can clobber audio memory and hang the main loop. 0 off-target (host: plenty).
static inline size_t ocramHeapFree() {
#if defined(__IMXRT1062__)
    char *brk = __brkval ? __brkval : (char *)&_heap_start;
    char *top = (char *)&_heap_end;
    return (top > brk) ? (size_t)(top - brk) : 0;
#else
    return (size_t)-1;   // host: effectively unlimited
#endif
}

// Load /path off the SD card and parse it into out[0..maxOut). Returns the
// event count, or -1 on error (open/size/read/parse failure). Runs from the
// main loop only (heap alloc + SD I/O; never the audio ISR).
// `outBpm` (optional) receives the file's initial tempo in BPM — used to lock a
// drum groove to a playing song. `outBpb` (optional) receives the initial time
// signature as quarter-note beats per bar (see initialBeatsPerBar) so the master
// clock's bar/downbeat matches non-4/4 content. Both left untouched on failure.
static int loadSmfFile(const char *path, MidiFileEvent *out, int maxOut, float *outBpm = nullptr, uint8_t *outBpb = nullptr) {
    File f = SD.open(path, FILE_READ);
    if (!f) return -1;
    size_t len = f.size();
    if (len < 14 || len > 512UL * 1024) { f.close(); return -1; }   // sanity cap 512 KB

    // Read the whole file into a heap buffer for parseSmf() (it needs random access
    // across tracks). This MUST be a separate buffer from `out`: parseSmf() now
    // streams a k-way merge that writes into out[] as it reads the file bytes, so
    // aliasing the two would corrupt not-yet-read tracks. That's affordable now
    // precisely because the parser no longer allocates its own big event scratch —
    // the old ~121 KB TickEv block was what forced the out-buffer aliasing hack and
    // still overflowed the ~80 KB OCRAM heap on no-PSRAM boards. A typical song file
    // (tens of KB) fits the heap easily; a file too big to malloc returns -1 cleanly.
    // On a Teensy 4.1 WITH PSRAM, put the whole-file scratch in PSRAM (extmem): the
    // OCRAM malloc heap is only ~tens of KB and is shared with the audio engine, so a
    // big song (e.g. a dense 30 KB+ piano piece) either fails or — worse — faults the
    // main loop against it. PSRAM has megabytes free. Falls back to the OCRAM heap on
    // no-PSRAM boards and off-target (host) builds.
    uint8_t *buf = nullptr; bool ext = false; (void)ext;
#if defined(__IMXRT1062__)
    if (external_psram_size > 0) { buf = (uint8_t *)extmem_malloc(len); ext = (buf != nullptr); }
#endif
    // OCRAM-heap fallback (no PSRAM, or PSRAM full): plain malloc, as it always was.
    // On no-PSRAM boards this loads full songs fine (malloc returns NULL on genuine OOM
    // -> graceful -1, no hang). The whole-file-in-OCRAM hang was specific to a board
    // WITH PSRAM present (the extmem path above avoids it there) — so no size cap here.
    if (!buf) buf = (uint8_t *)malloc(len);
    if (!buf) { f.close(); return -1; }
    size_t got = f.read(buf, len);
    f.close();
    auto release = [&](uint8_t *p) {
#if defined(__IMXRT1062__)
        if (ext) { extmem_free(p); return; }
#endif
        free(p);
    };
    if (got != len) { release(buf); return -1; }
    if (outBpm) *outBpm = initialBpm(buf, len);
    if (outBpb) *outBpb = initialBeatsPerBar(buf, len);
    int n = parseSmf(buf, len, out, maxOut);
    release(buf);
    return n;
}

} // namespace smf
} // namespace tdsp
