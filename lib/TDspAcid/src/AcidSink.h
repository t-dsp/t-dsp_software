// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.
//
// AcidSink — monophonic TB-303-style acid bass voice built from stock
// Teensy Audio primitives.
//
// Design notes
// ------------
// The 303's signature sound comes from three quirks:
//
//   1. Filter envelope: a separate decay-only envelope that drives the
//      low-pass filter's cutoff. On every note-on, the filter opens
//      briefly then decays closed. We don't burn an AudioEffectEnvelope
//      on this — instead tick() runs a software exp-decay envelope and
//      writes AudioFilterStateVariable::frequency() directly. Amplitude
//      has its own AudioEffectEnvelope (plucked-string attack-decay
//      shape).
//
//   2. Slide: portamento ENGAGES ONLY when a new note-on arrives while
//      the previous note is still held. If you lift the first key
//      before pressing the second, no slide — the voice retriggers
//      normally. This is the "tie" feature on the 303's pattern
//      sequencer; we expose it as "play legato to slide."
//
//   3. Accent: velocity above a threshold makes the note louder AND
//      boosts the filter envelope amount. The combination is a
//      "pokier," more aggressive transient that cuts through the mix.
//      Accent notes also don't retrigger slide on the NEXT note — the
//      accented note becomes the new reference for the glide. (Our
//      implementation keeps slide logic simple: any overlapping
//      note-on triggers slide regardless of accent status.)
//
// Voice model: MONO, last-note priority. Same note-stack pattern as
// NeuroSink.
//
// Thread / context safety: same as every other sink — MidiSink
// callbacks run on the router's thread, no dynamic allocation, only
// borrowed audio-object pointers touched.

#pragma once

#include <stdint.h>

#include <MidiSink.h>

class AudioSynthWaveform;
class AudioFilterStateVariable;
class AudioEffectEnvelope;

class AcidSink : public tdsp::MidiSink {
public:
    struct VoicePorts {
        AudioSynthWaveform       *osc;
        AudioFilterStateVariable *filter;
        AudioEffectEnvelope      *ampEnv;
    };

    explicit AcidSink(const VoicePorts &ports);

    // MIDI listen channel. 0 = omni, 1..16 = specific channel.
    void    setMidiChannel(uint8_t ch);
    uint8_t midiChannel() const { return _midiCh; }

    // Waveform: 0 = saw (classic 303 sound), 1 = square. 303's
    // original had a "mode" switch between these two — square is
    // rounder, saw is more aggressive.
    void    setWaveform(uint8_t w);
    uint8_t waveform() const { return _waveform; }

    // Coarse tuning, -12..+12 semitones. Useful for quick octave
    // shifts without retuning the sequencer.
    void  setTuning(int semitones);
    int   tuning() const { return _tuningSemi; }

    // Filter base cutoff (Hz). The envelope opens the filter ABOVE
    // this base; when the envelope fully decays, the cutoff returns
    // to baseCutoff. Classic 303: low-ish base (300-800 Hz) +
    // high envelope mod for the "wow" sweep.
    void  setCutoff(float hz);
    float cutoff() const { return _cutoffHz; }

    // Resonance / Q. Clamped to [0.707, 5.0] to match
    // AudioFilterStateVariable's accepted range. High Q (3-5) gives
    // the squelchy, vocal 303 character.
    void  setResonance(float q);
    float resonance() const { return _resonance; }

    // Envelope-mod amount, 0..1. Scales how far the filter envelope
    // pushes the cutoff above baseCutoff. 0 = no envelope (filter at
    // static cutoff). 1 = cutoff swings up to +6 octaves above base
    // on each note. Accent multiplies this further.
    void  setEnvModAmount(float v);
    float envModAmount() const { return _envModAmount; }

    // Filter envelope decay (seconds). Short values (0.1-0.5) give
    // the classic 303 "bwap" plucked feel; longer (1-3) gives a
    // slower sweep like a ska bass.
    void  setEnvDecay(float seconds);
    float envDecay() const { return _envDecaySec; }

    // Amp envelope decay (seconds). Usually similar to or slightly
    // longer than envDecay. The 303 has no sustain — the note dies
    // regardless of whether you hold the key. Classic values 0.2-1.0.
    void  setAmpDecay(float seconds);
    float ampDecay() const { return _ampDecaySec; }

    // Accent strength, 0..1. Accented notes boost both filter env
    // and amp gain. 0 = accent disabled; 1 = very loud + very
    // filter-open on accented notes.
    void  setAccent(float v);
    float accent() const { return _accentAmount; }

    // Velocity threshold for accent, 1..127. Notes at or above
    // this velocity count as accented. Default 110 — matches the
    // ~85% velocity most keyboards send on "hard" hits.
    void    setAccentThreshold(uint8_t v);
    uint8_t accentThreshold() const { return _accentThreshold; }

    // Slide time (ms). The duration of the glide when a new note
    // arrives while the previous is still held. 0 = no slide (note
    // snaps). Typical 303 values 30-120 ms.
    void  setSlideMs(float ms);
    float slideMs() const { return _slideMs; }

    // Voice volume trim; sketch's own amp is the user-facing fader.
    void  setVoiceVolumeScale(float s);
    float voiceVolumeScale() const { return _voiceVolumeScale; }

    // Advance the filter envelope + portamento glide. Call every
    // main-loop iteration. Cheap no-op when the voice is idle and
    // no glide is in flight.
    void tick(uint32_t nowMs);

    // MidiSink overrides
    void onNoteOn     (uint8_t channel, uint8_t note, uint8_t velocity) override;
    void onNoteOff    (uint8_t channel, uint8_t note, uint8_t velocity) override;
    void onPitchBend  (uint8_t channel, float   semitones)              override;
    void onAllNotesOff(uint8_t channel)                                 override;

private:
    VoicePorts _ports;

    uint8_t  _midiCh           = 0;       // 0 = omni
    uint8_t  _waveform         = 0;       // 0 = saw
    int      _tuningSemi       = 0;
    float    _cutoffHz         = 500.0f;
    float    _resonance        = 3.8f;
    float    _envModAmount     = 0.6f;
    float    _envDecaySec      = 0.3f;
    float    _ampDecaySec      = 0.4f;
    float    _accentAmount     = 0.5f;
    uint8_t  _accentThreshold  = 110;
    float    _slideMs          = 60.0f;
    float    _voiceVolumeScale = 1.0f;

    // Runtime voice state
    bool     _held             = false;
    uint8_t  _targetNote       = 60;
    float    _bend_semi        = 0.0f;
    float    _currentHz        = 440.0f;
    float    _targetHz         = 440.0f;
    bool     _lastAccent       = false;   // was the current sounding note accented?

    // Filter envelope — software, runs in tick(). Value is 0..1,
    // seeded to 1 on note-on, decays exponentially toward 0.
    float    _filterEnvVal     = 0.0f;
    uint32_t _lastTickMs       = 0;

    // Note stack for mono last-note priority (up to 8 held notes).
    // Same design as NeuroSink.
    static constexpr int kStackSize = 8;
    uint8_t  _noteStack[kStackSize];
    int      _noteStackCount   = 0;

    bool channelMatches(uint8_t ch) const {
        return _midiCh == 0 || ch == _midiCh;
    }
    void pushNote(uint8_t note);
    void popNote (uint8_t note);
    bool topNote (uint8_t *out) const;

    void applyFrequencies();        // writes osc.frequency() from _currentHz + bend
    void applyFilterCutoff();       // writes filter.frequency() from env value + base
    void applyOscAmplitude(bool accent, float velNormalized);

    static float noteToHz(uint8_t note);
    static short toneTypeFor(uint8_t wave);
};
