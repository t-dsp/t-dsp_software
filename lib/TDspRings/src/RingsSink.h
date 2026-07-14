// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.
//
// RingsSink — MidiSink-driven, MPE-aware polyphonic front-end for the Rings-
// style modal/string resonator. Owns no audio nodes: the sketch statically
// instantiates N AudioSynthRings voices and hands us pointers via VoicePorts.
//
// One resonator per note. Global macros (MODE / STRUCTURE / BRIGHTNESS /
// DAMPING) push to every voice; the MPE axes are per-note:
//   pitch bend (X) -> that voice's resonator frequency
//   pressure   (Z) -> that voice's DAMPING (more pressure = longer ring)
//   CC#74      (Y) -> that voice's BRIGHTNESS, ± _brightnessDepth around global
// Velocity sets the excitation ACCENT. Allocation mirrors Plaits2Sink /
// MpeVaSink (idle-first, then LRU steal).

#pragma once

#include <stdint.h>

#include <MidiSink.h>

class AudioSynthRings;

class RingsSink : public tdsp::MidiSink {
public:
    static constexpr int kMaxVoices = 8;

    struct VoicePorts {
        AudioSynthRings *engine;   // one resonator per polyphony slot
    };

    RingsSink(VoicePorts *voices, int voiceCount);

    void    setMasterChannel(uint8_t ch);   // 0 = omni; 1..16 = MPE master
    uint8_t masterChannel() const { return _masterChannel; }

    // --- Global macros ---------------------------------------------------
    void setMode(uint8_t mode);              // 0 = Modal, 1 = String
    uint8_t mode() const { return _mode; }
    void setStructure(float v);              // 0..1
    void setBrightness(float v);             // 0..1
    void setDamping(float v);                // 0..1
    void setBrightnessDepth(float v);        // per-note CC#74 depth, 0..1

    // --- tdsp::MidiSink overrides ---------------------------------------
    void onNoteOn    (uint8_t channel, uint8_t note, uint8_t velocity) override;
    void onNoteOff   (uint8_t channel, uint8_t note, uint8_t velocity) override;
    void onPitchBend (uint8_t channel, float   semitones)              override;
    void onPressure  (uint8_t channel, float   value)                  override;
    void onTimbre    (uint8_t channel, float   value)                  override;
    void onAllNotesOff(uint8_t channel)                                override;

private:
    struct Voice {
        uint8_t  channel    = 0;
        uint8_t  note       = 0;
        bool     note_held  = false;
        uint32_t start_time = 0;
        float    bend_semi  = 0.0f;
        float    brightness = 0.5f;   // CC#74 latch
        float    pressure   = 0.0f;
    };

    VoicePorts *_voices;
    Voice       _state[kMaxVoices];
    int         _voiceCount;
    uint32_t    _counter        = 0;
    uint8_t     _masterChannel  = 0;

    uint8_t _mode            = 0;
    float   _structure       = 0.5f;
    float   _brightness      = 0.5f;
    float   _damping         = 0.5f;
    float   _brightnessDepth = 1.0f;
    float   _channelTimbre[16];

    int  pickVoice();
    int  findActive(uint8_t ch, uint8_t note);
    void applyFreq      (int vi);
    void applyBrightness(int vi);
    void applyDamping   (int vi);
    static float noteToHz(float note);
};
