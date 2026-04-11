// MeterEngine — periodic peak/RMS sampling for the mixer input channels.
//
// Polls stock AudioAnalyzePeak + AudioAnalyzeRMS objects that the project
// wires into the audio graph, packs peak+RMS pairs into a float array in
// channel order, and hands the buffer to OscDispatcher for blob emission.
//
// MVP v1 simplification: meters always stream at ~30 Hz regardless of
// subscription state. Clients can ignore /meters/input if they don't want
// to display them. A future SubscriptionMgr milestone will add proper
// subscribe/unsubscribe lifecycle to gate the stream.

#pragma once

#include "MixerModel.h"  // kChannelCount

#include <stdint.h>

class AudioAnalyzePeak;
class AudioAnalyzeRMS;
class OSCBundle;

namespace tdsp {

class OscDispatcher;

class MeterEngine {
public:
    MeterEngine();

    // Register the peak/RMS analyzers for each input channel. Called once
    // per channel during setup(). Either analyzer may be nullptr (that
    // channel's pair will report 0).
    void setChannel(int n, AudioAnalyzePeak *peak, AudioAnalyzeRMS *rms);

    // Register the dispatcher used to broadcast meter blobs.
    void setDispatcher(OscDispatcher *dispatcher) { _dispatcher = dispatcher; }

    // Poll interval in milliseconds (default 33 ms ≈ 30 Hz).
    void setPollIntervalMs(uint32_t ms) { _pollIntervalMs = ms; }

    // Call from loop() every iteration. If the poll interval has elapsed,
    // samples all analyzers and emits a /meters/input blob via the
    // dispatcher's broadcast helper. The caller must flush the resulting
    // reply bundle on the transport.
    //
    // Returns true if a meter blob was emitted into `reply` this tick.
    bool tick(OSCBundle &reply);

private:
    OscDispatcher    *_dispatcher     = nullptr;
    uint32_t          _pollIntervalMs = 33;
    uint32_t          _lastPollMs     = 0;

    AudioAnalyzePeak *_peaks[kChannelCount + 1] = {0};
    AudioAnalyzeRMS  *_rmses[kChannelCount + 1] = {0};

    // Scratch buffer: peak, rms pairs per channel.
    float _pairs[kChannelCount * 2] = {0};
};

}  // namespace tdsp
