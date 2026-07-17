// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.
//
// Plaits2Sink — MidiSink-driven, MPE-aware polyphonic front-end for the
// authentic Plaits engine. Owns no audio nodes: the sketch statically
// instantiates N AudioSynthPlaits voices (Teensy Audio requires static
// allocation for AudioConnection) and hands us pointers via VoicePorts.
//
// One Plaits Voice per note. The five macros (ENGINE/HARMONICS/TIMBRE/MORPH/
// DECAY + LPG colour) are global and pushed to every voice; the three MPE
// expression axes are per-note:
//   pitch bend (X) -> that voice's note offset
//   pressure   (Z) -> that voice's LEVEL (VCA swell)
//   CC#74      (Y) -> that voice's TIMBRE, ± _timbreDepth around the global
//
// Voice allocation (idle-first, then LRU steal) and the MPE master-channel
// convention mirror the proven MpeVaSink / stock PlaitsSink.

#pragma once

#include <stdint.h>

#include <MidiSink.h>

class AudioSynthPlaits;

class Plaits2Sink : public tdsp::MidiSink {
public:
    static constexpr int kMaxVoices = 8;

    struct VoicePorts {
        AudioSynthPlaits *engine;   // one Plaits Voice per polyphony slot
    };

    // voices: array of `voiceCount` VoicePorts (clamped to [0, kMaxVoices]).
    Plaits2Sink(VoicePorts *voices, int voiceCount);

    // MPE master. 0 = omni (every channel allocates a voice; plain-keyboard
    // friendly — the on-screen keys send ch 1). 1..16 = MPE master (notes on
    // it are ignored; member channels each own a voice). >16 clamps to 0.
    void    setMasterChannel(uint8_t ch);
    uint8_t masterChannel() const { return _masterChannel; }

    // --- Global macros (applied to all voices) ---------------------------
    void setEngine(uint8_t e);      // Plaits synthesis model 0..15
    uint8_t engine() const { return _engine; }
    void setHarmonics(float v);     // 0..1
    void setTimbre(float v);        // 0..1
    void setMorph(float v);         // 0..1
    void setDecay(float v);         // 0..1
    void setLpgColour(float v);     // 0..1
    // Per-note CC#74 depth: how far (in TIMBRE units) a full Y swing moves a
    // voice's timbre around the global value. 0 disables per-note timbre.
    void setTimbreDepth(float v);   // 0..1

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
        float    base_level = 0.0f;   // velocity 0..1
        float    timbre     = 0.5f;   // CC#74 latch for this voice
        float    pressure   = 0.0f;   // 0..1
        float    bend_semi  = 0.0f;
    };

    VoicePorts *_voices;
    Voice       _state[kMaxVoices];
    int         _voiceCount;
    uint32_t    _counter        = 0;
    uint8_t     _masterChannel  = 0;

    uint8_t _engine      = 0;
    float   _harmonics   = 0.5f;
    float   _timbre      = 0.5f;
    float   _morph       = 0.5f;
    float   _decay       = 0.5f;
    float   _lpgColour   = 0.5f;
    float   _timbreDepth = 1.0f;
    float   _channelTimbre[16];

    int  pickVoice();
    int  findActive(uint8_t ch, uint8_t note);
    void applyPitch (int vi);
    void applyLevel (int vi);
    void applyTimbre(int vi);
    void pushMacros (int vi);   // engine + harmonics/morph/decay/lpg
};
