// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.
//
// DaisyVaSink — MidiSink-driven, MPE-aware polyphonic front-end for the DaisySP
// virtual-analog voice. Owns no audio nodes: the sketch statically instantiates
// N AudioSynthDaisyVa voices and hands us pointers via VoicePorts.
//
// A small PRESET bank defines the voice character (waveform / detune / filter /
// ADSR); the app picker selects a preset. The MPE axes are per-note:
//   pitch bend (X) -> voice frequency
//   pressure   (Z) -> voice level (VCA swell)
//   CC#74      (Y) -> voice filter cutoff, ± around the preset's cutoff
// Allocation mirrors Plaits2Sink / RingsSink (idle-first, then LRU steal).

#pragma once

#include <stdint.h>

#include <MidiSink.h>

class AudioSynthDaisyVa;

class DaisyVaSink : public tdsp::MidiSink {
public:
    static constexpr int kMaxVoices = 12;

    struct VoicePorts {
        AudioSynthDaisyVa *engine;
    };

    struct Preset {
        const char *name;
        uint8_t     wave;       // AudioSynthDaisyVa::Wave
        float       detuneCents;
        float       cutoffHz;
        float       resonance;
        float       attack, decay, sustain, release;
    };

    DaisyVaSink(VoicePorts *voices, int voiceCount);

    void    setMasterChannel(uint8_t ch);   // 0 = omni; 1..16 = MPE master
    uint8_t masterChannel() const { return _masterChannel; }

    // --- Presets ---------------------------------------------------------
    static int         numPresets();
    static const char *presetName(int idx);
    void setPreset(int idx);                 // applies to all voices
    int  preset() const { return _preset; }

    // Per-note CC#74 cutoff depth in octaves (± around the preset cutoff).
    void setCutoffDepthOct(float oct) { _cutoffDepthOct = oct; }

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
        float    base_level = 0.0f;
        float    cutoff     = 0.5f;   // CC#74 latch (0.5 neutral)
        float    pressure   = 0.0f;
        float    bend_semi  = 0.0f;
    };

    VoicePorts *_voices;
    Voice       _state[kMaxVoices];
    int         _voiceCount;
    uint32_t    _counter        = 0;
    uint8_t     _masterChannel  = 0;
    int         _preset         = 0;
    float       _cutoffBaseHz   = 4000.0f;
    float       _cutoffDepthOct = 2.0f;
    float       _channelTimbre[16];

    int  pickVoice();
    int  findActive(uint8_t ch, uint8_t note);
    void applyPitch (int vi);
    void applyLevel (int vi);
    void applyCutoff(int vi);
    static float noteToHz(float note);
};
