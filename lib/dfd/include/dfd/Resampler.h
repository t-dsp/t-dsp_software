// dfd/Resampler.h — variable-rate read over a Region (DESIGN §8).
//
// Pitch sits ON TOP OF a Region: the Region owns buffering/streaming/looping; the Resampler
// only turns a rate into fractional stepping with interpolation. At rate 1.0 it is a bit-exact
// passthrough (Region::read), which is what the drum one-shots use. For rate != 1.0 it does
// linear interpolation between adjacent frames — the technique is adapted from
// newdigate/teensy-variable-playback (MIT; see LICENSE).
//
// Portable core: <cstdint> + Region only.
#pragma once
#include <cstdint>
#include "Region.h"

namespace dfd {

class Resampler {
public:
    void  setRate(float r) { _rate = (r == 0.0f) ? 1.0f : r; }
    float rate() const { return _rate; }

    // Called when a fresh region playback starts, so interpolation state is clean.
    void reset() { _primed = false; _frac = 0.0; }

    // Emit `frames` frames from `region` at the current rate into interleaved `dst`.
    // Returns true if it produced any audio. Zero-fills the tail past end-of-region.
    bool read(Region& region, int16_t* dst, uint16_t frames) {
        const uint8_t ch = region.channels();
        if (_rate == 1.0f) return region.read(dst, frames);        // bit-exact fast path (drums)

        if (!_primed) {
            if (!region.readFrame(_cur)) return false;             // first frame
            if (!region.readFrame(_nxt)) { for (uint8_t c = 0; c < ch; c++) _nxt[c] = _cur[c]; }
            _primed = true; _frac = 0.0;
        }

        uint16_t f = 0;
        bool ended = false;
        for (; f < frames; f++) {
            for (uint8_t c = 0; c < ch; c++) {
                double v = (double)_cur[c] * (1.0 - _frac) + (double)_nxt[c] * _frac;
                dst[(uint32_t)f * ch + c] = clamp16(v);
            }
            _frac += _rate;
            while (_frac >= 1.0) {                                  // advance one or more frames
                _frac -= 1.0;
                for (uint8_t c = 0; c < ch; c++) _cur[c] = _nxt[c];
                if (!region.readFrame(_nxt)) { ended = true; break; }
            }
            if (ended) { f++; break; }
        }
        for (uint32_t i = (uint32_t)f * ch; i < (uint32_t)frames * ch; i++) dst[i] = 0;
        if (ended) _primed = false;
        return f > 0;
    }

private:
    static int16_t clamp16(double v) {
        if (v >  32767.0) return  32767;
        if (v < -32768.0) return -32768;
        return (int16_t)(v >= 0 ? v + 0.5 : v - 0.5);
    }
    float   _rate = 1.0f;
    double  _frac = 0.0;
    bool    _primed = false;
    int16_t _cur[2] = {0, 0};   // core supports mono/stereo (ch <= 2)
    int16_t _nxt[2] = {0, 0};
};

} // namespace dfd
