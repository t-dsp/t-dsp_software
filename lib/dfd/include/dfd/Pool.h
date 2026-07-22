// dfd/Pool.h — an N-voice pool with idle->oldest stealing (DESIGN §3, §12).
//
// Optional convenience: N Voices sharing one Allocator/config, with the voice-pick policy the
// drum sampler already tuned (retrigger reuses; else an idle voice; else steal the oldest).
// Region/Voice stay usable standalone (DESIGN §12 "ship it but keep Region/Voice standalone").
// A consumer that wants a different steal policy (e.g. the drum sampler's least-remaining-tail)
// can drive Voices directly and skip Pool.
//
// Portable core: Voice only. Voices are heap-allocated once at construction (off the audio
// path); the audio path only read()s them.
#pragma once
#include <cstdint>
#include <cstddef>
#include "Voice.h"

namespace dfd {

template <uint8_t N>
class Pool {
public:
    Pool(Allocator& alloc, const RegionConfig& cfg) {
        for (uint8_t i = 0; i < N; i++) _voices[i] = new Voice(alloc, cfg);
    }
    ~Pool() { for (uint8_t i = 0; i < N; i++) delete _voices[i]; }

    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    Voice& voice(uint8_t i) { return *_voices[i]; }
    static constexpr uint8_t size() { return N; }

    // Pick a voice for a fresh note: first an idle one, else the oldest-started. (No note-key
    // retrigger here — the consumer that has a key can check that first; see DrumSampler.)
    uint8_t pick() {
        for (uint8_t i = 0; i < N; i++) if (!_voices[i]->active()) return i;
        uint8_t oldest = 0; uint32_t bestSeq = 0xFFFFFFFFu;
        for (uint8_t i = 0; i < N; i++) {
            if (_voices[i]->startSeq() < bestSeq) { bestSeq = _voices[i]->startSeq(); oldest = i; }
        }
        return oldest;
    }

    // Refill every active voice's ring. Call regularly off the audio path.
    void service() { for (uint8_t i = 0; i < N; i++) if (_voices[i]->active()) _voices[i]->service(); }

    // Sum all voices into interleaved `dst` (frames * channels). `dst` must be pre-zeroed by the
    // caller if it wants a clean mix; this ADDS with saturation. Convenience for simple hosts.
    void mixInto(int16_t* dst, uint16_t frames, uint8_t channels, int16_t* scratch) {
        for (uint8_t i = 0; i < N; i++) {
            if (!_voices[i]->active()) continue;
            if (!_voices[i]->read(scratch, frames)) continue;
            uint32_t n = (uint32_t)frames * channels;
            for (uint32_t s = 0; s < n; s++) {
                int32_t m = (int32_t)dst[s] + (int32_t)scratch[s];
                dst[s] = (m > 32767) ? 32767 : (m < -32768 ? -32768 : (int16_t)m);
            }
        }
    }

private:
    Voice* _voices[N] = {};
};

} // namespace dfd
