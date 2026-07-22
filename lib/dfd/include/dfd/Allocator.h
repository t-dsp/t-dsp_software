// dfd/Allocator.h — the memory-injection interface (see DESIGN §2.1 / §7).
//
// Buffers (resident heads, body rings, stutter slices) are allocated ONCE at load time
// through an Allocator; the audio path never allocates. `preferFast` asks for the fastest
// pool available (PSRAM on Teensy); an implementation falls back to normal RAM when fast
// RAM is unavailable or preferFast is false. This single seam is what lets the same core
// scale from a no-PSRAM heap to a 64 MB PSRAM part (DESIGN §7): PSRAM buys latency-hiding,
// not residency — only heads live there, never whole samples.
#pragma once
#include <cstddef>
#include <cstdint>

namespace dfd {

struct Allocator {
    virtual ~Allocator() = default;

    // Allocate `samples` int16s. `preferFast` requests the fast pool; implementations MUST
    // fall back to normal RAM (rather than fail) when fast RAM is unavailable. Returns
    // nullptr only on genuine out-of-memory.
    virtual int16_t* alloc(size_t samples, bool preferFast) = 0;

    // Free a buffer previously returned by alloc(). nullptr is a no-op.
    virtual void free(int16_t* p) = 0;
};

} // namespace dfd
