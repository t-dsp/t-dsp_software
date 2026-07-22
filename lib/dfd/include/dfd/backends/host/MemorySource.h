// dfd/backends/host/MemorySource.h — a Source over a RAM buffer.
//
// For desktop unit tests (the reason the portable-core split matters — the head->body handoff
// and loop logic are proven off-target) and for a fully-resident mode on any platform.
// Not part of the core: this is a backend, free to use <cstring>.
#pragma once
#include <cstdint>
#include <cstring>
#include "dfd/Source.h"

namespace dfd {

class MemorySource : public Source {
public:
    // `data` is `samples` interleaved int16s (samples = frames * channels). Not owned.
    MemorySource(const int16_t* data, uint32_t samples, uint8_t channels)
        : _data(data), _samples(samples), _channels(channels) {}

    uint32_t totalSamples() const override { return _samples; }
    uint8_t  channels()     const override { return _channels; }

    uint32_t read(uint32_t offsetSamples, int16_t* dst, uint32_t count) override {
        _reads++;
        if (offsetSamples >= _samples) return 0;
        uint32_t avail = _samples - offsetSamples;
        if (count > avail) count = avail;
        std::memcpy(dst, _data + offsetSamples, (size_t)count * sizeof(int16_t));
        return count;
    }

    // Test hook: how many times read() was called (proves the audio path never hits the source).
    uint32_t readCount() const { return _reads; }
    void resetReadCount() { _reads = 0; }

private:
    const int16_t* _data;
    uint32_t _samples;
    uint8_t  _channels;
    uint32_t _reads = 0;
};

} // namespace dfd
