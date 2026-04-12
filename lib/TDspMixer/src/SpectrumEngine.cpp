#include "SpectrumEngine.h"
#include "OscDispatcher.h"

#include <Arduino.h>
#include <Audio.h>  // AudioAnalyzeFFT1024
#include <OSCBundle.h>

#include <math.h>
#include <string.h>

namespace tdsp {

static constexpr int kBins = 512;

SpectrumEngine::SpectrumEngine() {
    memset(_bins, 0, sizeof(_bins));
}

void SpectrumEngine::setChannels(AudioAnalyzeFFT1024 *fftL,
                                 AudioAnalyzeFFT1024 *fftR) {
    _fftL = fftL;
    _fftR = fftR;
}

// Compress a single FFT magnitude (0..1 float) to a uint8 dB byte.
// -80 dB -> 0, 0 dB -> 255. Anything below the noise floor clamps to 0;
// anything at or above unity clamps to 255. The +1e-6f floor avoids
// log(0) for silent bins.
static inline uint8_t magToDbByte(float mag) {
    const float db = 20.0f * log10f(mag + 1e-6f);
    // Map [-80, 0] dB -> [0, 255].
    float scaled = (db + 80.0f) * (255.0f / 80.0f);
    if (scaled < 0.0f)   scaled = 0.0f;
    if (scaled > 255.0f) scaled = 255.0f;
    return (uint8_t)(scaled + 0.5f);
}

bool SpectrumEngine::tick(OSCBundle &reply) {
    if (!_enabled) return false;
    const uint32_t now = millis();
    // Warmup: suppress the first 200ms after enabling so the USB Audio
    // controller has time to settle after the connect/subscribe burst.
    if (_warmupPending) { _enabledAtMs = now; _warmupPending = false; }
    if (now - _enabledAtMs < _warmupMs) return false;
    if (now - _lastPollMs < _pollIntervalMs) return false;

    // Only emit a frame when BOTH FFTs have fresh data. If either is
    // stale, skip this tick entirely — don't advance _lastPollMs, so
    // we'll re-check on the next loop iteration and pick the frame up
    // as soon as it lands. The stock AudioAnalyzeFFT1024 does not gate
    // on calling read() without available(); it returns stale floats,
    // which would quietly freeze one side of the display.
    if (!_fftL || !_fftR) return false;
    if (!_fftL->available() || !_fftR->available()) return false;

    _lastPollMs = now;

    // 512 L bins, then 512 R bins. read(bin) returns the linear
    // magnitude (0..1) for that bin.
    for (int i = 0; i < kBins; ++i) {
        _bins[i]              = magToDbByte(_fftL->read(i));
        _bins[kBins + i]      = magToDbByte(_fftR->read(i));
    }

    if (_dispatcher) {
        _dispatcher->broadcastMainSpectrum(reply, _bins, sizeof(_bins));
    }
    return true;
}

}  // namespace tdsp
