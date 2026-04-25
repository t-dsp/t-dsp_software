// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.

#include "Looper.h"

#include <Arduino.h>

namespace tdsp {

Looper::Looper(int16_t *buffer, uint32_t bufferSamples)
    : AudioStream(1, _inputQueueArray),
      _buffer(buffer),
      _capacity(bufferSamples) {}

void Looper::record() {
    __disable_irq();
    _writePos      = 0;
    _playPos       = 0;
    _lengthSamples = 0;
    _state         = Recording;
    __enable_irq();
}

void Looper::play() {
    __disable_irq();
    if (_state == Recording) {
        // Finalize the take at whatever's been written so far.
        _lengthSamples = _writePos;
    }
    if (_lengthSamples == 0) {
        _state = Idle;
    } else {
        _playPos  = 0;
        _playFrac = 0.0f;
        _state    = Playing;
    }
    __enable_irq();
}

void Looper::play(uint32_t snapSamples, uint32_t minSamples) {
    __disable_irq();
    if (_state == Recording) {
        uint32_t len = _writePos;
        if (snapSamples > 0) {
            // Round to nearest multiple of snapSamples. Half-up: anything
            // past the midpoint bumps to the next beat — feels natural
            // when the user releases slightly after a beat edge.
            const uint32_t half = snapSamples / 2;
            uint32_t snapped = ((len + half) / snapSamples) * snapSamples;
            if (snapped < minSamples) snapped = minSamples;
            if (snapped > _capacity)  snapped = (_capacity / snapSamples) * snapSamples;
            // Zero-pad any extension so the tail of the loop isn't
            // uninitialized buffer contents. (Truncation is free — we
            // just advertise a smaller length; the extra samples sit
            // past _lengthSamples and are never read.)
            for (uint32_t i = len; i < snapped && i < _capacity; ++i) {
                _buffer[i] = 0;
            }
            len = snapped;
        }
        _lengthSamples = len;
    }
    if (_lengthSamples == 0) {
        _state = Idle;
    } else {
        _playPos  = 0;
        _playFrac = 0.0f;
        _state    = Playing;
    }
    __enable_irq();
}

void Looper::stop() {
    __disable_irq();
    if (_state == Recording) {
        _lengthSamples = _writePos;
    }
    _state = (_lengthSamples > 0) ? Stopped : Idle;
    __enable_irq();
}

void Looper::clear() {
    __disable_irq();
    _writePos      = 0;
    _playPos       = 0;
    _lengthSamples = 0;
    _state         = Idle;
    __enable_irq();
}

void Looper::setReturnLevel(float g01) {
    if (g01 < 0.0f) g01 = 0.0f;
    if (g01 > 1.0f) g01 = 1.0f;
    _returnLevel    = g01;
    _returnScaleQ15 = (int32_t)(g01 * 32767.0f + 0.5f);
}

void Looper::setPlaybackRate(float rate) {
    if (rate < 0.25f) rate = 0.25f;
    if (rate > 4.0f)  rate = 4.0f;
    _playbackRate = rate;
}

float Looper::lengthSeconds() const {
    return (float)_lengthSamples / AUDIO_SAMPLE_RATE_EXACT;
}

float Looper::capacitySeconds() const {
    return (float)_capacity / AUDIO_SAMPLE_RATE_EXACT;
}

void Looper::update() {
    audio_block_t *in  = receiveReadOnly(0);
    audio_block_t *out = allocate();
    if (!out) {
        if (in) release(in);
        return;
    }

    const uint8_t  st     = _state;
    const uint32_t cap    = _capacity;
    const uint32_t length = _lengthSamples;

    if (st == Recording && in && cap > 0) {
        uint32_t wp = _writePos;
        const int16_t *src = in->data;
        for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            if (wp >= cap) {
                // Overflow: lock length at capacity, drop to Stopped.
                // Rest of this block is silent on the output (we're
                // not playing yet).
                _lengthSamples = cap;
                _state         = Stopped;
                break;
            }
            _buffer[wp++] = src[i];
        }
        _writePos = wp;
        for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) out->data[i] = 0;
    } else if (st == Playing && length > 0) {
        // Variable-rate playback with linear interpolation. _playFrac
        // is the fractional sample position between _buffer[pp] and
        // _buffer[pp+1]; each output sample we advance by _playbackRate
        // and wrap the integer part at the loop boundary.
        //
        // Rate == 1.0 is the common case (no clock-follow scaling); the
        // interp still runs but _playFrac stays 0 so the read is a
        // plain _buffer[pp]. The extra cost is a multiply-add and a
        // branch per sample — cheap even at 44.1 kHz on Cortex-M7.
        uint32_t pp   = _playPos;
        float    frac = _playFrac;
        const int32_t scale = _returnScaleQ15;
        const float   rate  = _playbackRate;
        for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            if (pp >= length) pp = 0;
            const uint32_t pnext = (pp + 1 >= length) ? 0 : pp + 1;
            // Linear interp between pp and pnext. Both samples are int16,
            // fits comfortably in a float for the mix.
            const float a = (float)_buffer[pp];
            const float b = (float)_buffer[pnext];
            const float m = a + (b - a) * frac;
            int32_t s = (int32_t)m * scale;
            s >>= 15;
            if (s >  32767) s =  32767;
            if (s < -32768) s = -32768;
            out->data[i] = (int16_t)s;

            frac += rate;
            while (frac >= 1.0f) {
                pp++;
                if (pp >= length) pp = 0;
                frac -= 1.0f;
            }
        }
        _playPos  = pp;
        _playFrac = frac;
    } else {
        for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) out->data[i] = 0;
    }

    transmit(out, 0);
    release(out);
    if (in) release(in);
}

}  // namespace tdsp
