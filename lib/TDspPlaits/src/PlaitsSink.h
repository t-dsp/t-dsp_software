// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.
//
// PlaitsSink — Plaits-inspired polyphonic macro oscillator.
//
// What this engine is
// -------------------
// Mutable Instruments' Plaits is a "macro oscillator" — pick one of N
// synthesis models, then sculpt it with three macro knobs (HARMONICS,
// TIMBRE, MORPH) and a per-note LPG (low-pass gate) decay. Each touch
// triggers a percussive ping that can sustain into a tone or fall into
// silence depending on the decay setting. We don't clone Plaits' DSP
// (the granular / wavetable / chord engines are huge); we capture its
// musical surface with stock Teensy Audio primitives:
//
//   - Per voice: two AudioSynthWaveform oscs into an AudioMixer4, then
//     AudioFilterStateVariable (LP), then AudioEffectEnvelope.
//   - 4 voices, polyphonic, MPE-aware (channel-per-voice routing for
//     pitch bend / pressure / CC#74).
//   - 5 selectable models (different waveform pairs on osc1/osc2).
//   - HARMONICS: osc2 transposition relative to osc1, snapped to musical
//     intervals (unison / fifth / octave / oct+fifth / two octaves).
//   - TIMBRE: base filter cutoff (200 Hz .. 16 kHz, exponential).
//   - MORPH: osc1/osc2 mix balance (0 = osc1 only, 1 = osc2 only).
//   - DECAY: envelope decay/release time (50 ms .. 3 s). Sustain is
//     forced to 0 so each note is a percussive ping — the LPG identity.
//
// MPE behaviour mirrors MpeVaSink: master channel 0 means "every channel
// allocates a voice" (plain keyboard friendly — matches the on-screen
// keyboard which sends ch 1); 1..16 means standard MPE (notes on master
// channel are ignored, member channels each own a voice).
//
// Voice allocation: idle-first, then LRU steal — same algorithm as
// MpeVaSink. Released voices linger until reuse so their tail rings out.
//
// Why a separate sink (vs. just reusing MpeVaSink)
// -----------------------------------------------
// MpeVaSink is built around a single oscillator per voice. The macro-
// oscillator identity needs two oscillators per voice to express the
// HARMONICS detune and MORPH mix — adding that to MpeVaSink would
// expand its struct, change its OSC contract, and dilute its identity.
// Cleaner to ship a separate sink with the same MPE plumbing copied
// across.

#pragma once

#include <stdint.h>

#include <MidiSink.h>

// Forward-declare Teensy Audio primitives so the header doesn't pull in
// Audio.h transitively. PlaitsSink.cpp pulls them in.
class AudioSynthWaveform;
class AudioMixer4;
class AudioFilterStateVariable;
class AudioEffectEnvelope;

class PlaitsSink : public tdsp::MidiSink {
public:
    // Per-voice wiring. The sketch owns the audio nodes (Teensy Audio
    // requires static allocation so AudioConnection ctors can register)
    // and hands us pointers. Pointers must outlive the sink.
    //
    // voiceMix is a 4-input AudioMixer4 with osc1 on input 0 and osc2
    // on input 1 (inputs 2..3 unused). MORPH writes the input gains.
    struct VoicePorts {
        AudioSynthWaveform       *osc1;
        AudioSynthWaveform       *osc2;
        AudioMixer4              *voiceMix;
        AudioFilterStateVariable *filter;
        AudioEffectEnvelope      *env;
    };

    static constexpr int kVoiceCount = 4;
    static constexpr int kMaxVoices  = 8;

    // Synthesis model — picks the waveform pair on osc1/osc2. Models are
    // chosen so HARMONICS/TIMBRE/MORPH all stay musically meaningful
    // across them. Stock Teensy waveforms only — no external DSP.
    enum Model : uint8_t {
        ModelVaSaw      = 0,  // saw + saw — classic supersaw-ish stack
        ModelVaSquare   = 1,  // square + square — hollow / reedy
        ModelFmSine     = 2,  // sine + sine — pure interval; HARMONICS = ratio feel
        ModelHollowTri  = 3,  // triangle + triangle — soft mallet / bell
        ModelMixedPulse = 4,  // saw + square — broad-spectrum buzz
        kModelCount     = 5,
    };

    // voices: array of kVoiceCount VoicePorts. voiceCount is clamped to
    // [0, kMaxVoices].
    PlaitsSink(VoicePorts *voices, int voiceCount);

    // MPE master channel. 0 = no master (every 1..16 allocates a voice;
    // good for plain keyboards including the dev surface's on-screen
    // keys which send ch 1); 1..16 = MPE master (notes ignored there,
    // only mod wheel + sustain). Values > 16 clamped to 0.
    void    setMasterChannel(uint8_t ch);
    uint8_t masterChannel() const { return _masterChannel; }

    // Model selector — applies to all voices simultaneously. Mid-note
    // changes are legal; the osc waveform updates on the fly.
    void    setModel(uint8_t model);
    uint8_t model() const { return _model; }

    // Macros — Plaits' three signature controls. All take 0..1; values
    // outside that range are clamped.
    //
    // HARMONICS: osc2 transposition from osc1 in snapped musical
    //   intervals — 0=unison, 0.25=+5th (7 semi), 0.5=+oct (12 semi),
    //   0.75=+oct+5th (19 semi), 1.0=+2oct (24 semi). Snap (vs. linear
    //   detune) keeps every HARMONICS setting musically usable; smooth
    //   detune was the alternative considered but it wanders out of
    //   tune at most settings.
    void  setHarmonics(float v);
    float harmonics() const { return _harmonics; }

    // TIMBRE: base filter cutoff in 200 Hz .. 16 kHz, mapped
    // exponentially. Per-voice CC#74 then modulates ±1 octave on top.
    void  setTimbre(float v);
    float timbre() const { return _timbre; }

    // MORPH: osc1/osc2 mix. 0 = osc1 only (most fundamental). 1 = osc2
    // only (HARMONICS-shifted). 0.5 = equal mix (rich combination). The
    // crossfade is power-preserving (sin/cos shaped) so mid-MORPH isn't
    // louder or quieter than the endpoints — keeps level matched as the
    // user sweeps.
    void  setMorph(float v);
    float morph() const { return _morph; }

    // DECAY: envelope decay/release time. 0 = 50 ms (short pluck).
    // 1 = 3 s (long ring-out tail). Percussive feel by default; sustain
    // is locked at 0 so a held note still decays to silence (LPG
    // identity — Plaits doesn't have a sustain stage).
    void  setDecay(float v);
    float decay() const { return _decay; }

    // Filter resonance, separate from the macros (Plaits' LPG mostly
    // hides Q, but exposing it lets the user dial in pingier or softer
    // tones). 0.707..5.0 (the Teensy SVF's accepted range).
    void  setResonance(float q);
    float resonance() const { return _resonance; }

    // Per-voice amplitude ceiling. Keeps headroom under polyphonic
    // pile-ups; the per-slot fader is the user-facing volume.
    void  setVoiceVolumeScale(float s);
    float voiceVolumeScale() const { return _volumeScale; }

    // --- tdsp::MidiSink overrides -------------------------------------

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
        float    base_amp   = 0.0f;
        float    bend_semi  = 0.0f;
        float    timbre     = 0.5f;   // CC#74, 0..1, 0.5 neutral
        float    pressure   = 0.0f;
    };

    VoicePorts *_voices;
    Voice       _state[kMaxVoices];
    int         _voiceCount;
    uint32_t    _counter        = 0;
    uint8_t     _masterChannel  = 0;     // omni by default (plain-keyboard friendly)
    uint8_t     _model          = ModelVaSaw;
    float       _harmonics      = 0.5f;  // octave by default — most musical
    float       _timbre         = 0.7f;  // ~5 kHz cutoff — open and bright
    float       _morph          = 0.5f;  // equal mix
    float       _decay          = 0.4f;  // ~500 ms — pluck with body
    float       _resonance      = 1.0f;  // mild lift; Plaits LPG is forgiving
    float       _volumeScale    = 0.7f;  // headroom for 4-voice pile-ups
    float       _channelTimbre[16];

    // Helpers
    int  pickVoice();
    int  findActive(uint8_t ch, uint8_t note);
    void applyFrequency(int vi);
    void applyAmplitude(int vi);
    void applyFilter   (int vi);
    void applyMorphAll();
    void applyWaveformAll();
    void applyEnvelopeAll();

    // Map HARMONICS (0..1) to a snapped semitone offset for osc2.
    static float harmonicsToSemitones(float h);
    // Map DECAY (0..1) to a time in ms (50..3000), exponential curve so
    // small movements at low end give fine pluck control.
    static float decayToMs(float d);
    // TIMBRE (0..1) → base filter cutoff Hz (200..16000, exponential).
    static float timbreToCutoffHz(float t);
    static float noteToHz(uint8_t note);
    static short toneTypeForOsc1(uint8_t model);
    static short toneTypeForOsc2(uint8_t model);
};
