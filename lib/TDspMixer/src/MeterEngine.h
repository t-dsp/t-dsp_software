// MeterEngine — periodic peak/RMS sampling for the mixer input channels.
//
// Polls stock AudioAnalyzePeak + AudioAnalyzeRMS objects that the project
// wires into the audio graph, packs peak+RMS pairs into a float array in
// channel order, and hands the buffer to OscDispatcher for blob emission.
//
// Streaming is gated by a single enable bit (setEnabled / isEnabled).
// Clients toggle the stream by sending /sub addSub … /meters/input and
// /sub unsubscribe /meters/input — handled by OscDispatcher::handleSub
// which flips this engine's enable flag. Default: disabled. A future
// SubscriptionMgr milestone will add per-client lifetime tracking so
// multiple clients can subscribe independently; for MVP a single global
// flag is enough.

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

    // Register main (master bus) L/R analyzers. Emitted as /meters/output
    // at the same cadence as /meters/input, two pairs (L peak/rms, R
    // peak/rms). Any nullptr is reported as 0.
    void setMain(AudioAnalyzePeak *peakL, AudioAnalyzeRMS *rmsL,
                 AudioAnalyzePeak *peakR, AudioAnalyzeRMS *rmsR);

    // Register host-volume (post-hostvol, pre-DAC) L/R analyzers.
    // Emitted as /meters/host at the same cadence.
    void setHost(AudioAnalyzePeak *peakL, AudioAnalyzeRMS *rmsL,
                 AudioAnalyzePeak *peakR, AudioAnalyzeRMS *rmsR);

    // Register the dispatcher used to broadcast meter blobs.
    void setDispatcher(OscDispatcher *dispatcher) { _dispatcher = dispatcher; }

    // Poll interval in milliseconds (default 33 ms ≈ 30 Hz).
    void setPollIntervalMs(uint32_t ms) { _pollIntervalMs = ms; }

    // Enable/disable meter blob streaming. Off by default; the client
    // turns it on by sending /sub addSub … /meters/input (handled by
    // OscDispatcher::handleSub which calls this method), and turns it
    // off with /sub unsubscribe /meters/input. When disabled, tick()
    // returns false immediately and no blob is emitted.
    void setEnabled(bool enabled) {
        if (enabled && !_enabled) _warmupPending = true;
        _enabled = enabled;
    }
    bool isEnabled() const        { return _enabled; }

    // Call from loop() every iteration. If the poll interval has elapsed,
    // samples all analyzers and emits a /meters/input blob via the
    // dispatcher's broadcast helper. The caller must flush the resulting
    // reply bundle on the transport.
    //
    // Returns true if a meter blob was emitted into `reply` this tick.
    bool tick(OSCBundle &reply);

private:
    OscDispatcher    *_dispatcher     = nullptr;
    bool              _enabled        = false;
    bool              _warmupPending  = false;
    uint32_t          _enabledAtMs    = 0;
    static constexpr uint32_t _warmupMs = 200;
    uint32_t          _pollIntervalMs = 100;
    uint32_t          _lastPollMs     = 0;

    AudioAnalyzePeak *_peaks[kChannelCount + 1] = {0};
    AudioAnalyzeRMS  *_rmses[kChannelCount + 1] = {0};

    // Main (master) L/R analyzers.
    AudioAnalyzePeak *_mainPeakL = nullptr;
    AudioAnalyzeRMS  *_mainRmsL  = nullptr;
    AudioAnalyzePeak *_mainPeakR = nullptr;
    AudioAnalyzeRMS  *_mainRmsR  = nullptr;

    // Host (post-hostvol, pre-DAC) L/R analyzers.
    AudioAnalyzePeak *_hostPeakL = nullptr;
    AudioAnalyzeRMS  *_hostRmsL  = nullptr;
    AudioAnalyzePeak *_hostPeakR = nullptr;
    AudioAnalyzeRMS  *_hostRmsR  = nullptr;

    // Scratch buffer: peak, rms pairs per channel.
    float _pairs[kChannelCount * 2] = {0};
    // Scratch buffers: 2 pairs each (L peak/rms, R peak/rms).
    float _mainPairs[4] = {0};
    float _hostPairs[4] = {0};
};

}  // namespace tdsp
