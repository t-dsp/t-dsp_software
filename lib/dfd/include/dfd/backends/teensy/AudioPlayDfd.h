// dfd/backends/teensy/AudioPlayDfd.h — a Teensy AudioStream node (int16 stereo) wrapping a
// dfd::Voice (DESIGN §2/§3). It is the drop-in sibling of teensy-variable-playback's
// AudioPlaySdResmp: update() pulls one audio block from the Voice's resident head / body ring
// (index math + copy only — NO filesystem, NO allocation on the audio path), while service() —
// called from loop() — is the SOLE place Source::read() runs to keep the ring ahead.
//
// The Voice (head + body ring) is allocated ONCE in begin() so a consumer can allocate only the
// voices it actually uses (fit control on a no-PSRAM heap). play() rebinds a persistent Source
// (the per-sample handle) with no reallocation, so retriggers cost no alloc and no FAT lookup.
//
// Teensy backend: free to include Audio/Arduino. The dfd core stays platform-clean.
#pragma once
#include <Arduino.h>
#include <AudioStream.h>
#include "dfd/Voice.h"

namespace dfd {

class AudioPlayDfd : public AudioStream {
public:
    AudioPlayDfd() : AudioStream(0, NULL) {}
    ~AudioPlayDfd() { delete _voice; }

    // Allocate this node's Voice (head + body ring) once. Off the audio path. Returns true only
    // if both buffers allocated (a failed voice stays silent rather than crashing the ISR).
    bool begin(Allocator& alloc, const RegionConfig& cfg) {
        if (!_voice) _voice = new Voice(alloc, cfg);
        return _voice && _voice->ok();
    }
    bool allocated() const { return _voice != nullptr && _voice->ok(); }

    // Trigger playback from a caller-owned, persistent Source. Off the audio path. loop=false is
    // a one-shot (drums). The Source must stay valid until the voice stops.
    bool play(Source& src, bool loop = false, float rate = 1.0f) {
        if (!allocated()) return false;
        _voice->stop();                 // fully close down the prior sample before rebinding the
                                        // source — the ISR sees silence while play() re-anchors.
        _voice->setSource(src);
        _voice->play(0, src.totalSamples(), loop, 0, rate);
        _lenFrames = _voice->framesTotal();
        // Declick ATTACK: rise from silence over kAttackFrames so a fresh voice never steps from 0
        // to a non-zero sample[0]. Kept sub-millisecond so the drum transient is preserved.
        _gain = 0.0f; _gainTarget = 1.0f; _releasing = false;
        return _voice->active();
    }

    // HARD stop — immediate silence (voice teardown / kit change). No declick; the caller is
    // tearing the source down, so a ramp would read stale geometry (see Region::play()).
    void stop() { if (_voice) _voice->stop(); _gain = 1.0f; _gainTarget = 1.0f; _releasing = false; }

    // SOFT stop — ramp the tail to zero over kReleaseFrames, THEN stop the voice. This kills the
    // "snap" when a still-ringing hit is retriggered / stolen / hi-hat-choked: the OUTGOING voice
    // is faded out instead of cut at a non-zero sample. update() finishes the stop at gain 0.
    // (Named softStop, not release(), to avoid clashing with AudioStream::release(block).)
    void softStop() { if (_voice && _voice->active()) { _gainTarget = 0.0f; _releasing = true; } }

    bool isPlaying() { return _voice && _voice->active(); }
    bool releasing() const { return _releasing; }
    void setPlaybackRate(float r) { if (_voice) _voice->setRate(r); }

    // STREAM PATH: refill the ring. Call from loop() for every node whose voice isPlaying().
    bool service() { return _voice ? _voice->service() : false; }

    // Underrun tally (opt-in probe; DESIGN §4). Aggregated by the consumer's stress test.
    uint32_t underruns() const { return _voice ? _voice->region().underruns() : 0; }
    void resetUnderruns() { if (_voice) _voice->region().resetUnderruns(); }

    uint32_t lengthMillis() const {
        return (uint32_t)((uint64_t)_lenFrames * 1000u / (uint32_t)AUDIO_SAMPLE_RATE_EXACT);
    }
    uint32_t positionMillis() const {
        uint32_t f = _voice ? _voice->framesPlayed() : 0;
        return (uint32_t)((uint64_t)f * 1000u / (uint32_t)AUDIO_SAMPLE_RATE_EXACT);
    }

    void update() override {
        if (!_voice || !_voice->active()) return;
        audio_block_t* bl = allocate();
        audio_block_t* br = allocate();
        if (!bl || !br) { if (bl) release(bl); if (br) release(br); return; }

        int16_t scratch[AUDIO_BLOCK_SAMPLES * 2];
        _voice->read(scratch, AUDIO_BLOCK_SAMPLES);
        const bool stereo = _voice->channels() >= 2;

        if (_gain >= 1.0f && !_releasing) {
            // Fast path: unity gain, no ramp in flight — plain copy (bit-identical to no declick).
            if (stereo) for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) { bl->data[i] = scratch[i*2]; br->data[i] = scratch[i*2+1]; }
            else        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) { bl->data[i] = scratch[i];   br->data[i] = scratch[i]; }
        } else {
            // Declick ramp: step _gain toward _gainTarget each frame and scale L/R. Attack rises to
            // 1 (kAttackStep); a softStop() release falls to 0 (kReleaseStep) and stops the voice.
            const float step = _releasing ? kReleaseStep : kAttackStep;
            float g = _gain;
            for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
                if      (g < _gainTarget) { g += step; if (g > _gainTarget) g = _gainTarget; }
                else if (g > _gainTarget) { g -= step; if (g < _gainTarget) g = _gainTarget; }
                const int16_t l = stereo ? scratch[i*2]   : scratch[i];
                const int16_t r = stereo ? scratch[i*2+1] : scratch[i];
                bl->data[i] = (int16_t)(l * g);
                br->data[i] = (int16_t)(r * g);
            }
            _gain = g;
            if (_releasing && _gain <= 0.0f) { _voice->stop(); _releasing = false; }
        }
        transmit(bl, 0);
        transmit(br, 1);
        release(bl);
        release(br);
    }

private:
    // Declick ramp lengths (frames @ AUDIO_SAMPLE_RATE_EXACT). Overridable per build; defaults are
    // ~0.33 ms attack (transient-preserving) and ~5.3 ms release (enough to erase a mid-tone cut).
    static constexpr int   kAttackFrames  = 16;
    static constexpr int   kReleaseFrames = 256;
    static constexpr float kAttackStep    = 1.0f / (float)kAttackFrames;
    static constexpr float kReleaseStep   = 1.0f / (float)kReleaseFrames;

    Voice*   _voice = nullptr;
    uint32_t _lenFrames = 0;
    float    _gain = 1.0f, _gainTarget = 1.0f;   // declick envelope (1 = pass-through fast path)
    bool     _releasing = false;
};

} // namespace dfd
