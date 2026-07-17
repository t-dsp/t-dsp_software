// tsf_impl.cpp — the single translation unit that compiles TinySoundFont's
// implementation, configured for Teensy 4.1:
//   * all allocations go to PSRAM (extmem_*), since the whole font's samples are
//     resident (as float) — 8 MB now, 16 MB after the second APS6404L.
//   * no stdio (no FILE on the MCU); the font is loaded via a tsf_stream backed by
//     the SD card (see AudioSynthTsf.h).
//
// TinySoundFont (schellingb/TinySoundFont) is MIT. It is a COMPLETE SF2 renderer —
// velocity layers, region layering, all generators, filters, envelopes, GM drums —
// so unlike the AudioSynthWavetable path it needs no per-generator glue.
#include <Arduino.h>

extern "C" {
    void *extmem_malloc(size_t size);
    void *extmem_realloc(void *ptr, size_t size);
    void  extmem_free(void *ptr);
}

#define TSF_NO_STDIO
#define TSF_MALLOC   extmem_malloc
#define TSF_REALLOC  extmem_realloc
#define TSF_FREE     extmem_free

#define TSF_IMPLEMENTATION
#include "tsf.h"
