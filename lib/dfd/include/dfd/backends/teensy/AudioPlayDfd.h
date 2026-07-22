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
        return _voice->active();
    }

    void stop() { if (_voice) _voice->stop(); }
    bool isPlaying() { return _voice && _voice->active(); }
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
        if (_voice->channels() >= 2) {
            for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) { bl->data[i] = scratch[i*2]; br->data[i] = scratch[i*2+1]; }
        } else {
            for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) { bl->data[i] = scratch[i]; br->data[i] = scratch[i]; }
        }
        transmit(bl, 0);
        transmit(br, 1);
        release(bl);
        release(br);
    }

private:
    Voice*   _voice = nullptr;
    uint32_t _lenFrames = 0;
};

} // namespace dfd
