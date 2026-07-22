// dfd/backends/host/StdAllocator.h — plain malloc/free Allocator for desktop tests and any
// platform without a fast pool. `preferFast` is ignored (there is only one pool). Optionally
// tracks live allocations so tests can assert no leaks and exercise the fallback path.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include "dfd/Allocator.h"

namespace dfd {

class StdAllocator : public Allocator {
public:
    int16_t* alloc(size_t samples, bool /*preferFast*/) override {
        int16_t* p = (int16_t*)std::malloc(samples * sizeof(int16_t));
        if (p) _live++;
        return p;
    }
    void free(int16_t* p) override {
        if (p) { std::free(p); _live--; }
    }
    long liveAllocations() const { return _live; }

private:
    long _live = 0;
};

// Models a platform with NO fast pool (like a no-PSRAM Teensy): a preferFast request is served
// from normal RAM instead of failing — the contract in Allocator.h. Records that a fallback
// happened so tests can prove the path is exercised and still yields a working Region.
class FallbackAllocator : public Allocator {
public:
    int16_t* alloc(size_t samples, bool preferFast) override {
        if (preferFast) _fallbacks++;                 // fast asked for, but we only have normal RAM
        return (int16_t*)std::malloc(samples * sizeof(int16_t));
    }
    void free(int16_t* p) override { if (p) std::free(p); }
    long fallbacks() const { return _fallbacks; }
private:
    long _fallbacks = 0;
};

// Always returns nullptr (genuine out-of-memory) — lets tests prove a Region reports !ok()
// and that play()/read() are safe no-ops rather than crashing.
class OomAllocator : public Allocator {
public:
    int16_t* alloc(size_t, bool) override { return nullptr; }
    void free(int16_t*) override {}
};

} // namespace dfd
