// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.
//
// SupersawSink — Roland JP-8000-style 5-voice detuned-saw lead.
//
// The "supersaw" is a lead/pad texture built from 5+ slightly detuned
// saw oscillators stacked. The JP-8000 fame came from two controls:
// "Detune" (how far the side voices spread in cents) and "Mix" (a
// crossfade between center-voice-only and side-voices-only). The
// Adam Szabo paper describes exact detune curves and gain matching;
// we use a simplified version: 5 saws with symmetric detune, plus
// a mix param that trades center saw level vs the four side saws.
//
// Voice model: MONO with optional portamento. Lead synth, not a pad
// (a true supersaw pad wants 8-voice polyphony which would be ~40
// oscs — out of budget on this board).
//
// Envelope: full ADSR (this is a melodic lead, so sustain matters
// unlike the bass synths).

#pragma once

#include <stdint.h>

#include <MidiSink.h>

class AudioSynthWaveform;
class AudioMixer4;
class AudioFilterStateVariable;
class AudioEffectEnvelope;

class SupersawSink : public tdsp::MidiSink {
public:
    // Five saw oscillators: one center + two above + two below.
    // Detune spread is symmetric; mix balances center vs sides.
    struct VoicePorts {
        AudioSynthWaveform       *osc1;   // center, 0 cents
        AudioSynthWaveform       *osc2;   // -d cents
        AudioSynthWaveform       *osc3;   // +d cents
        AudioSynthWaveform       *osc4;   // -2d cents
        AudioSynthWaveform       *osc5;   // +2d cents
        AudioMixer4              *mixAB;  // sums osc1..osc4 (4 inputs available)
        AudioMixer4              *mixFinal; // sums mixAB + osc5 into 2 inputs (rest unused)
        AudioFilterStateVariable *filter;
        AudioEffectEnvelope      *env;
    };

    explicit SupersawSink(const VoicePorts &ports);

    void    setMidiChannel(uint8_t ch);
    uint8_t midiChannel() const { return _midiCh; }

    // Detune: 0..100 cents between adjacent voices. Side 1 pair is
    // at ±detune, side 2 pair at ±2×detune — a linear spread that
    // approximates the JP-8000 curve well enough for Phase 1.
    void  setDetuneCents(float cents);
    float detuneCents() const { return _detuneCents; }

    // Mix center: 0..1. 0 = only side voices (hollow, wide). 1 =
    // only center (thin, in-tune). 0.3-0.5 is the classic supersaw
    // sweet spot.
    void  setMixCenter(float v);
    float mixCenter() const { return _mixCenter; }

    void  setCutoff(float hz);
    float cutoff() const { return _cutoffHz; }

    void  setResonance(float q);
    float resonance() const { return _resonance; }

    // Full ADSR — unlike the bass synths, a lead needs sustain and
    // release to play held notes and fades.
    void  setAttack (float seconds);
    void  setDecay  (float seconds);
    void  setSustain(float v);
    void  setRelease(float seconds);
    float attack()  const { return _attackSec;  }
    float decay()   const { return _decaySec;   }
    float sustain() const { return _sustain;    }
    float release() const { return _releaseSec; }

    // Portamento on/off via > 0. Applies on any note-on transition
    // (unlike Acid where slide is legato-only). 0-500 ms.
    void  setPortamentoMs(float ms);
    float portamentoMs() const { return _portMs; }

    void  setVoiceVolumeScale(float s);

    // Advance portamento glide. No LFO in Phase 1 — keep it simple.
    void tick(uint32_t nowMs);

    void onNoteOn     (uint8_t, uint8_t, uint8_t) override;
    void onNoteOff    (uint8_t, uint8_t, uint8_t) override;
    void onPitchBend  (uint8_t, float)            override;
    void onAllNotesOff(uint8_t)                   override;

private:
    VoicePorts _ports;

    uint8_t _midiCh           = 0;
    float   _detuneCents      = 18.0f;
    float   _mixCenter        = 0.4f;
    float   _cutoffHz         = 9000.0f;
    float   _resonance        = 1.0f;
    float   _attackSec        = 0.05f;
    float   _decaySec         = 0.3f;
    float   _sustain          = 0.8f;
    float   _releaseSec       = 0.6f;
    float   _portMs           = 0.0f;
    float   _voiceVolumeScale = 1.0f;

    // Voice state
    bool     _held            = false;
    uint8_t  _targetNote      = 60;
    float    _bend_semi       = 0.0f;
    float    _velocity        = 0.0f;
    float    _currentHz       = 440.0f;
    float    _targetHz        = 440.0f;
    uint32_t _lastTickMs      = 0;

    static constexpr int kStackSize = 8;
    uint8_t  _noteStack[kStackSize];
    int      _noteStackCount  = 0;

    bool channelMatches(uint8_t ch) const { return _midiCh == 0 || ch == _midiCh; }
    void pushNote(uint8_t);
    void popNote (uint8_t);
    bool topNote (uint8_t *out) const;

    void applyFrequencies();
    void applyAmplitudes();
    void applyMixBalance();
    void applyFilter();

    static float noteToHz(uint8_t note);
};
