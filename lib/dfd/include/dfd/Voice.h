// dfd/Voice.h — a Region + Resampler = one playable streaming voice (DESIGN §3).
//
// The smallest thing a consumer plays: it owns a Region (head+body+loop) and a Resampler
// (pitch). A Teensy AudioStream node owns a Voice (or a Pool of them). Amplitude enveloping is
// intentionally minimal here (Phase 1 drums ring their sample out at unity); richer envelopes
// belong to the consumer or a later phase.
//
// Portable core: Region + Resampler only.
#pragma once
#include <cstdint>
#include "Region.h"
#include "Resampler.h"

namespace dfd {

class Voice {
public:
    Voice(Allocator& alloc, const RegionConfig& cfg) : _region(alloc, cfg) {}

    bool ok() const { return _region.ok(); }
    void setSource(Source& s) { _region.setSource(s); }

    // Trigger playback of [start, start+length) at `rate` (1.0 = native). Off the audio path.
    void play(uint32_t start, uint32_t length, bool loop = false, uint32_t loopStart = 0, float rate = 1.0f) {
        _resampler.setRate(rate);
        _resampler.reset();
        _region.play(start, length, loop, loopStart);
        _startSeq = ++_seqCounter;   // monotonic age tag for oldest-voice stealing
    }

    void setRate(float r) { _resampler.setRate(r); }
    void stop() { _region.stop(); }

    bool active() const { return _region.active(); }
    uint8_t channels() const { return _region.channels(); }

    // AUDIO PATH: fill `frames` interleaved frames. Zero-fills past end. No FS/alloc.
    bool read(int16_t* dst, uint16_t frames) { return _resampler.read(_region, dst, frames); }

    // STREAM PATH: refill the body ring. Call regularly off the audio path.
    bool service() { return _region.service(); }

    Region&       region()       { return _region; }
    const Region& region() const { return _region; }

    uint32_t framesTotal()  const { return _region.framesTotal(); }
    uint32_t framesPlayed() const { return _region.framesPlayed(); }
    uint32_t startSeq()     const { return _startSeq; }

private:
    Region    _region;
    Resampler _resampler;
    uint32_t  _startSeq = 0;
    static uint32_t _seqCounter;
};

// One definition, header-only friendly (inline avoids a .cpp / ODR issues).
inline uint32_t Voice::_seqCounter = 0;

} // namespace dfd
