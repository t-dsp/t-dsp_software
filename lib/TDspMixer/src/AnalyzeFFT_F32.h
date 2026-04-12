// AnalyzeFFT_F32 — configurable-size real FFT analyzer for I16 AudioStream.
//
// Wraps ARM CMSIS-DSP arm_rfft_fast_f32 with a familiar available()/read()
// interface matching AudioAnalyzeFFT1024. Supports 1024, 2048, and 4096
// point sizes selectable at runtime via setSize(). Default is 1024.
//
// Lives in TDspMixer (not a vendored lib mod). Receives I16 audio blocks,
// converts to F32 internally, applies a Hanning window, and computes
// magnitudes for N/2 bins.
//
// Memory: ~40 KB per instance at max size (4096). Two instances (stereo)
// plus a shared 16 KB scratch buffer = ~96 KB total.

#pragma once

#include <Arduino.h>      // must precede AudioStream.h (provides F_CPU_ACTUAL etc.)
#include <AudioStream.h>
#include <arm_math.h>

namespace tdsp {

static constexpr int kMaxFFTSize = 4096;
static constexpr int kMaxFFTBins = kMaxFFTSize / 2;

class AnalyzeFFT_F32 : public AudioStream {
public:
    AnalyzeFFT_F32();

    // Change FFT size at runtime. Supported: 1024, 2048, 4096.
    // Takes effect at the start of the next accumulation cycle.
    bool setSize(int n);
    int  size() const     { return _fftSize; }
    int  binCount() const { return _fftSize / 2; }

    bool available() {
        if (_outputFlag) { _outputFlag = false; return true; }
        return false;
    }

    // Read magnitude for a single bin (0..binCount()-1). Returns 0..1
    // linear scale, matching AudioAnalyzeFFT1024::read().
    float read(unsigned int bin) {
        if ((int)bin >= _fftSize / 2) return 0.0f;
        return _magnitudes[bin];
    }

    virtual void update(void);

private:
    int  _fftSize  = 1024;
    int  _pending  = 0;       // non-zero = resize at next cycle start
    int  _writePos = 0;       // samples accumulated so far
    volatile bool _outputFlag = false;

    arm_rfft_fast_instance_f32 _fftInst;

    float _buffer[kMaxFFTSize];       // sample accumulator
    float _window[kMaxFFTSize];       // Hanning coefficients
    float _magnitudes[kMaxFFTBins];   // output magnitudes

    audio_block_t *_inputQueueArray[1];

    void initFFT(int n);
    void computeWindow(int n);
};

}  // namespace tdsp
