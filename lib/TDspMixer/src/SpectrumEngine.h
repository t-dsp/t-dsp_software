// SpectrumEngine — periodic stereo FFT sampling of the main-bus tap.
//
// Mirrors MeterEngine's shape (setChannels / setDispatcher / setEnabled /
// tick) but samples two AudioAnalyzeFFT1024 instances wired to mainAmpL/R
// (post-fader, pre-hostvol), compresses each of 512 magnitude bins to a
// single uint8 dB byte, and ships the 1024-byte result as an OSC blob at
// /spectrum/main.
//
// Encoding per bin: dB = 20 * log10(mag + 1e-6); byte = clamp(round(
// (dB + 80) * 255 / 80), 0, 255). -80 dB -> 0, 0 dB -> 255. One byte
// per bin keeps the payload small (1 KB per frame) so we can stream at
// 30 Hz with zero worry about USB CDC throughput.
//
// Streaming is gated by setEnabled(); OscDispatcher::handleSub flips the
// flag on /sub addSub /spectrum/main and unsubscribe. Mirrors MeterEngine
// exactly — single global enable bit, no per-client tracking for MVP.

#pragma once

#include <stdint.h>

class AudioAnalyzeFFT1024;
class OSCBundle;

namespace tdsp {

class OscDispatcher;

class SpectrumEngine {
public:
    SpectrumEngine();

    // Register the FFT analyzers for the main bus L/R tap. Either
    // pointer may be nullptr; that half of the blob will report zeros.
    void setChannels(AudioAnalyzeFFT1024 *fftL, AudioAnalyzeFFT1024 *fftR);

    void setDispatcher(OscDispatcher *dispatcher) { _dispatcher = dispatcher; }

    // Poll interval in milliseconds (default 33 ms ≈ 30 Hz). FFT1024
    // produces a new frame every ~11.6 ms at 44.1 kHz, so 33 ms is
    // comfortably above the underlying cadence.
    void setPollIntervalMs(uint32_t ms) { _pollIntervalMs = ms; }

    // Enable/disable spectrum blob streaming. Off by default; the
    // client turns it on by sending /sub addSub … /spectrum/main
    // (routed here by OscDispatcher::handleSub). When disabled, tick()
    // returns false immediately and no blob is emitted.
    void setEnabled(bool enabled) { _enabled = enabled; }
    bool isEnabled() const        { return _enabled; }

    // Call from loop() every iteration. If the poll interval has
    // elapsed AND both FFT analyzers have a new frame available, reads
    // all 512 bins per channel, quantizes to dB bytes, and emits a
    // /spectrum/main blob via the dispatcher.
    //
    // Returns true iff a spectrum blob was emitted this tick.
    bool tick(OSCBundle &reply);

private:
    OscDispatcher        *_dispatcher     = nullptr;
    AudioAnalyzeFFT1024  *_fftL           = nullptr;
    AudioAnalyzeFFT1024  *_fftR           = nullptr;
    bool                  _enabled        = false;
    uint32_t              _pollIntervalMs = 33;
    uint32_t              _lastPollMs     = 0;

    // Scratch buffer: 512 L bytes then 512 R bytes = 1024 bytes total.
    uint8_t _bins[1024];
};

}  // namespace tdsp
