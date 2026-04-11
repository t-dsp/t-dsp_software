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

bool MeterEngine::tick(OSCBundle &reply) {
    const uint32_t now = millis();
    if (now - _lastPollMs < _pollIntervalMs) return false;
    _lastPollMs = now;

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

    if (_dispatcher) {
        _dispatcher->broadcastMetersInput(reply, _pairs, kChannelCount);
    }
    return true;
}

}  // namespace tdsp
