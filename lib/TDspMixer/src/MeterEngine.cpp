#include "MeterEngine.h"
#include "OscDispatcher.h"

#include <Arduino.h>
#include <Audio.h>  // AudioAnalyzePeak, AudioAnalyzeRMS
#include <OSCBundle.h>

namespace tdsp {

MeterEngine::MeterEngine() {
    for (int i = 0; i <= kChannelCount; ++i) {
        _peaks[i] = nullptr;
        _rmses[i] = nullptr;
    }
}

void MeterEngine::setChannel(int n, AudioAnalyzePeak *peak, AudioAnalyzeRMS *rms) {
    if (n < 1 || n > kChannelCount) return;
    _peaks[n] = peak;
    _rmses[n] = rms;
}

void MeterEngine::setMain(AudioAnalyzePeak *peakL, AudioAnalyzeRMS *rmsL,
                          AudioAnalyzePeak *peakR, AudioAnalyzeRMS *rmsR) {
    _mainPeakL = peakL;
    _mainRmsL  = rmsL;
    _mainPeakR = peakR;
    _mainRmsR  = rmsR;
}

void MeterEngine::setHost(AudioAnalyzePeak *peakL, AudioAnalyzeRMS *rmsL,
                          AudioAnalyzePeak *peakR, AudioAnalyzeRMS *rmsR) {
    _hostPeakL = peakL;
    _hostRmsL  = rmsL;
    _hostPeakR = peakR;
    _hostRmsR  = rmsR;
}

bool MeterEngine::tick(OSCBundle &reply) {
    if (!_enabled) return false;
    const uint32_t now = millis();
    if (now - _lastPollMs < _pollIntervalMs) return false;
    _lastPollMs = now;

    // Per-channel input meters.
    for (int n = 1; n <= kChannelCount; ++n) {
        float peak = 0.0f;
        float rms  = 0.0f;
        // AudioAnalyzePeak::available() returns true when a new peak
        // measurement is ready. AudioAnalyzeRMS::available() likewise.
        // If no new sample, we report 0 (cheap; the meter bar just dips).
        if (_peaks[n] && _peaks[n]->available()) peak = _peaks[n]->read();
        if (_rmses[n] && _rmses[n]->available()) rms  = _rmses[n]->read();
        _pairs[(n - 1) * 2 + 0] = peak;
        _pairs[(n - 1) * 2 + 1] = rms;
    }

    // Main (master) L/R meters — packed as 2 pairs in declared order
    // [peakL, rmsL, peakR, rmsR] for a total of 4 float32s = 16 bytes.
    {
        float peakL = 0.0f, rmsL = 0.0f, peakR = 0.0f, rmsR = 0.0f;
        if (_mainPeakL && _mainPeakL->available()) peakL = _mainPeakL->read();
        if (_mainRmsL  && _mainRmsL->available())  rmsL  = _mainRmsL->read();
        if (_mainPeakR && _mainPeakR->available()) peakR = _mainPeakR->read();
        if (_mainRmsR  && _mainRmsR->available())  rmsR  = _mainRmsR->read();
        _mainPairs[0] = peakL;
        _mainPairs[1] = rmsL;
        _mainPairs[2] = peakR;
        _mainPairs[3] = rmsR;
    }

    // Host (post-hostvol) L/R meters — same layout as main.
    {
        float peakL = 0.0f, rmsL = 0.0f, peakR = 0.0f, rmsR = 0.0f;
        if (_hostPeakL && _hostPeakL->available()) peakL = _hostPeakL->read();
        if (_hostRmsL  && _hostRmsL->available())  rmsL  = _hostRmsL->read();
        if (_hostPeakR && _hostPeakR->available()) peakR = _hostPeakR->read();
        if (_hostRmsR  && _hostRmsR->available())  rmsR  = _hostRmsR->read();
        _hostPairs[0] = peakL;
        _hostPairs[1] = rmsL;
        _hostPairs[2] = peakR;
        _hostPairs[3] = rmsR;
    }

    if (_dispatcher) {
        _dispatcher->broadcastMetersInput(reply, _pairs, kChannelCount);
        _dispatcher->broadcastMetersOutput(reply, _mainPairs, 2);
        _dispatcher->broadcastMetersHost(reply, _hostPairs, 2);
    }
    return true;
}

}  // namespace tdsp
