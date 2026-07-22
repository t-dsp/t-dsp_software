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
        // to a non-zero sample[0]. Kept short so the drum transient is preserved.
        beginFade(0.0f, 1.0f, kAttackFrames, /*releasing=*/false);
        return _voice->active();
    }

    // HARD stop — immediate silence (voice teardown / kit change). No declick; the caller is
    // tearing the source down, so a ramp would read stale geometry (see Region::play()).
    void stop() { if (_voice) _voice->stop(); _gain = 1.0f; _fadeLen = 0; _fadePos = 0; _releasing = false; }

    // SOFT stop — smoothstep the tail to zero over kReleaseFrames (STARTING from the current gain),
    // THEN stop the voice. Kills the "snap" when a still-ringing hit is retriggered / stolen /
    // hi-hat-choked: the OUTGOING voice fades instead of being cut at a non-zero sample. The curve
    // is slope-continuous at both ends (see fadeGain) so even a loud SUB-BASS tail (an 808 kick,
    // ~50 Hz) fades without the corner-click a linear ramp leaves. update() finishes the stop at 0.
    // (Named softStop, not release(), to avoid clashing with AudioStream::release(block).)
    void softStop() { if (_voice && _voice->active()) beginFade(_gain, 0.0f, kReleaseFrames, /*releasing=*/true); }

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

        if (_fadeLen == 0 && _gain >= 1.0f) {
            // Fast path: unity gain, no fade in flight — plain copy (bit-identical to no declick).
            if (stereo) for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) { bl->data[i] = scratch[i*2]; br->data[i] = scratch[i*2+1]; }
            else        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) { bl->data[i] = scratch[i];   br->data[i] = scratch[i]; }
        } else {
            // Fade in flight: advance the smoothstep envelope one frame at a time and scale L/R.
            for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
                if (_fadePos < _fadeLen) { _fadePos++; _gain = fadeGain(); }
                const int16_t l = stereo ? scratch[i*2]   : scratch[i];
                const int16_t r = stereo ? scratch[i*2+1] : scratch[i];
                bl->data[i] = (int16_t)(l * _gain);
                br->data[i] = (int16_t)(r * _gain);
            }
            if (_fadePos >= _fadeLen) {                 // fade complete this block
                _gain = _fadeTo; _fadeLen = 0;
                if (_releasing) { _voice->stop(); _releasing = false; }
            }
        }
        transmit(bl, 0);
        transmit(br, 1);
        release(bl);
        release(br);
    }

private:
    // Declick fade lengths (frames @ AUDIO_SAMPLE_RATE_EXACT). Overridable per build. Attack is
    // short (~1 ms) to keep the drum transient; release is ~30 ms — longer than one sub-bass cycle
    // (50 Hz = 20 ms) so an 808 kick tail fades over a full cycle, not chopped mid-cycle.
#ifndef TDSP_DFD_ATTACK_FRAMES
#define TDSP_DFD_ATTACK_FRAMES 48
#endif
#ifndef TDSP_DFD_RELEASE_FRAMES
#define TDSP_DFD_RELEASE_FRAMES 1440
#endif
    static constexpr int kAttackFrames  = TDSP_DFD_ATTACK_FRAMES;
    static constexpr int kReleaseFrames = TDSP_DFD_RELEASE_FRAMES;

    // Arm a fade from `from` to `to` over `len` frames (smoothstep). len<=0 applies instantly.
    void beginFade(float from, float to, int len, bool releasing) {
        _fadeFrom = from; _fadeTo = to; _fadeLen = (len > 0) ? len : 0; _fadePos = 0;
        _releasing = releasing;
        _gain = (_fadeLen == 0) ? to : from;
    }
    // Current envelope gain via smoothstep p*p*(3-2p): zero SLOPE at both ends, so no corner-click
    // (the artifact a linear ramp leaves on a loud low-frequency tail).
    float fadeGain() const {
        const float p  = (float)_fadePos / (float)_fadeLen;   // 0..1 across the fade
        const float ss = p * p * (3.0f - 2.0f * p);
        return _fadeFrom + (_fadeTo - _fadeFrom) * ss;
    }

    Voice*   _voice = nullptr;
    uint32_t _lenFrames = 0;
    float    _gain = 1.0f;                 // current output gain (1 = pass-through fast path)
    float    _fadeFrom = 1.0f, _fadeTo = 1.0f;
    int      _fadeLen = 0, _fadePos = 0;   // fade progress in frames (_fadeLen 0 = no fade)
    bool     _releasing = false;           // this fade ends by stopping the voice
};

} // namespace dfd
