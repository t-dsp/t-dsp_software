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
        _playPos = 0;
        _state   = Playing;
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
        uint32_t pp = _playPos;
        const int32_t scale = _returnScaleQ15;
        for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            if (pp >= length) pp = 0;
            int32_t s = (int32_t)_buffer[pp] * scale;
            s >>= 15;
            if (s >  32767) s =  32767;
            if (s < -32768) s = -32768;
            out->data[i] = (int16_t)s;
            pp++;
        }
        _playPos = pp;
    } else {
        for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) out->data[i] = 0;
    }

    transmit(out, 0);
    release(out);
    if (in) release(in);
}

}  // namespace tdsp
