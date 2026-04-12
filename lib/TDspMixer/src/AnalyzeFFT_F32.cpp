#include "AnalyzeFFT_F32.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>

namespace tdsp {

// Shared scratch buffer for arm_rfft_fast_f32 output. Safe to share
// because the audio ISR is non-reentrant — only one instance is ever
// inside update() at a time.
static float s_fftOut[kMaxFFTSize];

AnalyzeFFT_F32::AnalyzeFFT_F32()
    : AudioStream(1, _inputQueueArray)
{
    memset(_buffer, 0, sizeof(_buffer));
    memset(_magnitudes, 0, sizeof(_magnitudes));
    initFFT(1024);
}

bool AnalyzeFFT_F32::setSize(int n) {
    if (n != 1024 && n != 2048 && n != 4096) return false;
    if (n == _fftSize && _pending == 0) return true;
    _pending = n;
    return true;
}

void AnalyzeFFT_F32::initFFT(int n) {
    _fftSize  = n;
    _writePos = 0;
    computeWindow(n);
    arm_rfft_fast_init_f32(&_fftInst, (uint16_t)n);
}

void AnalyzeFFT_F32::computeWindow(int n) {
    for (int i = 0; i < n; i++) {
        _window[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i / (float)n));
    }
}

void AnalyzeFFT_F32::update() {
    audio_block_t *block = receiveReadOnly();
    if (!block) return;

    // Apply pending resize at start of a new accumulation cycle.
    if (_pending && _writePos == 0) {
        initFFT(_pending);
        _pending = 0;
    }

    // Convert I16 samples to F32 and accumulate into buffer.
    const int16_t *src = block->data;
    float *dst = _buffer + _writePos;
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
        dst[i] = (float)src[i] / 32768.0f;
    }
    release(block);

    _writePos += AUDIO_BLOCK_SAMPLES;
    if (_writePos < _fftSize) return;

    // Apply Hanning window in-place. Safe because _writePos resets below
    // and the buffer will be overwritten by fresh samples next cycle.
    for (int i = 0; i < _fftSize; i++) {
        _buffer[i] *= _window[i];
    }

    // Forward real FFT.  arm_rfft_fast_f32 modifies _buffer in-place and
    // writes complex output to s_fftOut (N floats).
    arm_rfft_fast_f32(&_fftInst, _buffer, s_fftOut, 0);

    // Compute magnitudes from complex output, normalized to 0..1.
    // CMSIS packing: out[0] = DC (real), out[1] = Nyquist (real),
    // then interleaved [re, im] pairs for bins 1..(N/2-1).
    // arm_rfft_fast_f32 does NOT include 1/N normalization, so raw
    // magnitudes for a full-scale sine are ~N/2. Dividing by N/2
    // brings them to ~1.0, matching AudioAnalyzeFFT1024::read().
    const int halfN = _fftSize / 2;
    const float scale = 1.0f / (float)halfN;

    _magnitudes[0] = fabsf(s_fftOut[0]) * scale;
    for (int i = 1; i < halfN; i++) {
        float re = s_fftOut[2 * i];
        float im = s_fftOut[2 * i + 1];
        _magnitudes[i] = sqrtf(re * re + im * im) * scale;
    }

    _outputFlag = true;
    _writePos = 0;
}

}  // namespace tdsp
