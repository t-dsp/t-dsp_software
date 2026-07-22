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
        // Declick START: unity gain (the drum transient is preserved in full), and arm a decaying
        // OFFSET that cancels the step between the LAST sample this node emitted and the new
        // sample[0]. That makes both a fresh start (last≈0) AND a hard voice STEAL/reuse (last at
        // some mid-decay level, e.g. a ringing 808 sub-bass) continuous — no instant jump to the new
        // attack. Computed on the first update() (once the new samples are readable).
        _gain = 1.0f; _fadeLen = 0; _fadePos = 0; _releasing = false;
        _dcArm = true;
        return _voice->active();
    }

    // HARD stop — immediate silence (voice teardown / kit change). No declick; the caller is
    // tearing the source down, so a ramp would read stale geometry (see Region::play()). Zero the
    // held-output memory so the NEXT play() declicks from silence, not a stale level.
    void stop() {
        if (_voice) _voice->stop();
        _gain = 1.0f; _fadeLen = 0; _fadePos = 0; _releasing = false;
        _dcArm = false; _dcLen = 0; _lastOutL = 0.0f; _lastOutR = 0.0f;
    }

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

        // Arm the declick offset now that the new samples are readable: offset = (last emitted) -
        // (new sample[0]); it decays to 0 over kDeclickFrames, so output[0] == last emitted (no step).
        if (_dcArm) {
            _dcL = _lastOutL - (float)scratch[0];
            _dcR = _lastOutR - (float)(stereo ? scratch[1] : scratch[0]);
            _dcPos = 0; _dcLen = kDeclickFrames; _dcArm = false;
        }

        const bool fading = (_fadeLen > 0) || (_gain < 1.0f);
        const bool dcing  = (_dcLen > 0 && _dcPos < _dcLen);
        if (!fading && !dcing) {
            // Fast path: unity gain, no fade/offset in flight — plain copy (bit-identical to none).
            if (stereo) for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) { bl->data[i] = scratch[i*2]; br->data[i] = scratch[i*2+1]; }
            else        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) { bl->data[i] = scratch[i];   br->data[i] = scratch[i]; }
        } else {
            for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
                if (_fadePos < _fadeLen) { _fadePos++; _gain = fadeGain(); }
                float dl = 0.0f, dr = 0.0f;
                if (_dcPos < _dcLen) {                  // decaying declick offset (smoothstep -> 0)
                    const float frac = 1.0f - smoothstep((float)_dcPos / (float)_dcLen);
                    dl = _dcL * frac; dr = _dcR * frac; _dcPos++;
                }
                const float l = (float)(stereo ? scratch[i*2]   : scratch[i]);
                const float r = (float)(stereo ? scratch[i*2+1] : scratch[i]);
                bl->data[i] = clip16((l + dl) * _gain);
                br->data[i] = clip16((r + dr) * _gain);
            }
            if (_fadePos >= _fadeLen) {                 // gain fade complete this block
                _gain = _fadeTo; _fadeLen = 0;
                if (_releasing) { _voice->stop(); _releasing = false; }
            }
            if (_dcPos >= _dcLen) _dcLen = 0;
        }
        // Remember the last emitted sample so the NEXT play() can declick from it.
        _lastOutL = (float)bl->data[AUDIO_BLOCK_SAMPLES - 1];
        _lastOutR = (float)br->data[AUDIO_BLOCK_SAMPLES - 1];
        transmit(bl, 0);
        transmit(br, 1);
        release(bl);
        release(br);
    }

private:
    // Declick lengths (frames @ AUDIO_SAMPLE_RATE_EXACT). Overridable per build. The RELEASE (a
    // softStop fade) and the START-offset decay are both ~30 ms — longer than one sub-bass cycle
    // (50 Hz = 20 ms) so an 808 kick fades / re-anchors over a full cycle, never chopped mid-cycle.
#ifndef TDSP_DFD_RELEASE_FRAMES
#define TDSP_DFD_RELEASE_FRAMES 1440
#endif
#ifndef TDSP_DFD_DECLICK_FRAMES
#define TDSP_DFD_DECLICK_FRAMES 1440
#endif
    static constexpr int kReleaseFrames = TDSP_DFD_RELEASE_FRAMES;
    static constexpr int kDeclickFrames = TDSP_DFD_DECLICK_FRAMES;

    // smoothstep p*p*(3-2p): 0->1 with zero SLOPE at both ends (no corner-click a linear ramp leaves).
    static float smoothstep(float p) { return p * p * (3.0f - 2.0f * p); }

    static int16_t clip16(float v) {
        if (v >  32767.0f) return  32767;
        if (v < -32768.0f) return -32768;
        return (int16_t)v;
    }

    // Arm a fade from `from` to `to` over `len` frames (smoothstep). len<=0 applies instantly.
    void beginFade(float from, float to, int len, bool releasing) {
        _fadeFrom = from; _fadeTo = to; _fadeLen = (len > 0) ? len : 0; _fadePos = 0;
        _releasing = releasing;
        _gain = (_fadeLen == 0) ? to : from;
    }
    float fadeGain() const { return _fadeFrom + (_fadeTo - _fadeFrom) * smoothstep((float)_fadePos / (float)_fadeLen); }

    Voice*   _voice = nullptr;
    uint32_t _lenFrames = 0;
    float    _gain = 1.0f;                 // current output gain (1 = pass-through fast path)
    float    _fadeFrom = 1.0f, _fadeTo = 1.0f;
    int      _fadeLen = 0, _fadePos = 0;   // gain-fade progress in frames (_fadeLen 0 = no fade)
    bool     _releasing = false;           // this fade ends by stopping the voice
    // Start-declick offset (cancels the step from the last emitted sample into a new/stolen note).
    float    _dcL = 0.0f, _dcR = 0.0f;     // offset magnitude captured at the first block after play
    int      _dcLen = 0, _dcPos = 0;       // offset-decay progress in frames (_dcLen 0 = inactive)
    bool     _dcArm = false;               // compute the offset on the next update()
    float    _lastOutL = 0.0f, _lastOutR = 0.0f;   // last emitted sample (for the next declick)
};

} // namespace dfd
