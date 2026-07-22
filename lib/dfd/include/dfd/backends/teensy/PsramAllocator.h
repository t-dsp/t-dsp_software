// dfd/backends/teensy/PsramAllocator.h — a dfd::Allocator that puts fast buffers in PSRAM when
// the board has it, and falls back to the normal heap when it doesn't (DESIGN §7). One knob —
// external_psram_size, read once here — picks the pool; the dfd core is identical either way.
//
// PSRAM present: heads (and, later, stutter slices) go to extmem_malloc(), so H can scale up
// for stall margin and huge libraries fit (only heads live there, not whole samples).
// No PSRAM: everything comes from the OCRAM heap — keep H * voiceCount within budget (DESIGN §7).
//
// Teensy backend: free to include Arduino/extmem. `external_psram_size` is provided by the core.
#pragma once
#include <Arduino.h>
#include <cstdint>
#include <cstddef>
#include "dfd/Allocator.h"

extern "C" uint8_t external_psram_size;      // MB of PSRAM the core detected (0 = none)
extern "C" void* extmem_malloc(size_t size);
extern "C" void  extmem_free(void* ptr);

namespace dfd {

class PsramAllocator : public Allocator {
public:
    // hasPsram is latched once at construction from external_psram_size.
    PsramAllocator() : _hasPsram(external_psram_size > 0) {}

    bool hasPsram() const { return _hasPsram; }

    int16_t* alloc(size_t samples, bool preferFast) override {
        size_t bytes = samples * sizeof(int16_t);
        if (preferFast && _hasPsram) {
            void* p = extmem_malloc(bytes);
            if (p) return (int16_t*)p;
            // fall through to heap if PSRAM is momentarily exhausted (contract: never fail on
            // fast-unavailable; DESIGN §7 / Allocator.h)
        }
        return (int16_t*)malloc(bytes);
    }

    void free(int16_t* p) override {
        if (!p) return;
        // Route the free to the pool the pointer came from. On Teensy 4.x PSRAM lives at
        // 0x70000000+; the OCRAM/heap does not, so an address test is sufficient and avoids
        // tracking every allocation.
        if (_hasPsram && ((uintptr_t)p >> 24) == 0x70) extmem_free(p);
        else ::free(p);
    }

private:
    bool _hasPsram;
};

} // namespace dfd
