// dfd/Source.h — the storage-injection interface (see DESIGN §2.1).
//
// A random-access PCM source. Offsets/counts are in int16 SAMPLES, CHANNEL-INTERLEAVED
// (i.e. a stereo frame is two samples: L then R). read() returns the number of samples
// actually written (< count at EOF). It is called ONLY from a non-realtime context (the
// streamer service(), DESIGN §6) — NEVER from the audio callback.
//
// This is one of the two whole-platform touchpoints of the dfd core. A consumer on any
// platform implements Source (+ Allocator) and gets the entire streaming engine. The core
// (include/dfd/*.h) includes nothing but <cstdint>/<cstddef> and these two interfaces.
#pragma once
#include <cstdint>

namespace dfd {

struct Source {
    virtual ~Source() = default;

    // Length of the sample DATA (excluding any container header), in interleaved int16 samples.
    virtual uint32_t totalSamples() const = 0;

    // 1 (mono) or 2 (stereo).
    virtual uint8_t channels() const = 0;

    // Read `count` interleaved int16 samples starting at `offsetSamples` into `dst`.
    // Returns the count actually read (< count only at/after EOF). NON-realtime only.
    //
    // NOTE: DESIGN §2.1 sketched `uint16_t* dst`; PCM is signed, so the core (and every
    // backend) uses int16_t* — the byte layout is identical, the type is just honest.
    virtual uint32_t read(uint32_t offsetSamples, int16_t* dst, uint32_t count) = 0;
};

} // namespace dfd
