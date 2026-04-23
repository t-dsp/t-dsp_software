// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.
//
// NeuroSink — monophonic reese / neuro bass voice built from stock Teensy
// Audio primitives.
//
// Why this engine exists
// ----------------------
// DnB "reese" bass is a single sustained tone made from 2+ detuned saw
// oscillators. The beating between detuned saws creates the hollow,
// moving quality; a sub sine an octave down adds weight. A resonant
// low-pass filter (often with LFO wobble) shapes the tone into the
// classic growl. Phase 2e is the BARE reese — no multiband destruction
// yet (that's Phase 2f, "the stink chain"). What this phase ships is
// Massive-style raw material: 3 detuned saws + sub sine + SVF + LFO.
//
// Voice model: MONO, last-note priority
// -------------------------------------
// Bass in this genre is monophonic. Playing a second note while one is
// held should NOT stack — it should retune the single voice to the new
// pitch (and retrigger the envelope). Releasing the top note while a
// lower one is still held should fall back to that lower note without
// retriggering (legato). A small note stack tracks held notes and the
// top of the stack is the sounding note.
//
// Portamento
// ----------
// When _portMs > 0, the voice frequency glides from currentHz toward
// targetHz in tick(). Linear in log-frequency space (so an octave takes
// the same time regardless of register). When _portMs == 0 the glide
// snaps instantly on note-on, which is the normal default.
//
// Oscillator balance
// ------------------
// Four oscs feed a 4-input mixer:
//   slot 0: saw +detune  (osc1)
//   slot 1: saw -detune  (osc2)
//   slot 2: saw 0 cents  (osc3) — reinforces fundamental, usually lower level
//   slot 3: sine -12 semi (oscSub)
// The mixer slot gains stay at 1.0 (summing); the actual level balance
// is baked into the oscs' .amplitude() calls, which also fold in
// velocity and the voice-volume-scale trim. This lets setSubLevel etc.
// change balance without a mixer-gain getter being required (Teensy
// Audio's mixer has setters only).
//
// Thread / context safety
// -----------------------
// Same contract as MpeVaSink: MidiSink callbacks run on the router's
// context (USB host poll / OSC / serial). No dynamic allocation; only
// the audio objects handed in at construction are touched.

#pragma once

#include <stdint.h>

#include <MidiSink.h>

class AudioSynthWaveform;
class AudioMixer4;
class AudioFilterStateVariable;
class AudioEffectEnvelope;

class NeuroSink : public tdsp::MidiSink {
public:
    // Wiring struct. Sketch owns the actual audio objects (they must
    // be statically allocated so their ctors register with the graph)
    // and hands us pointers. All fields are required.
    struct VoicePorts {
        AudioSynthWaveform       *osc1;      // saw, +detune cents
        AudioSynthWaveform       *osc2;      // saw, -detune cents
        AudioSynthWaveform       *osc3;      // saw, 0 cents (fundamental reinforce)
        AudioSynthWaveform       *oscSub;    // sine, -12 semi
        AudioMixer4              *voiceMix;  // 4-in sum for the oscs
        AudioFilterStateVariable *filter;    // resonant LP
        AudioEffectEnvelope      *env;       // amp envelope
    };

    explicit NeuroSink(const VoicePorts &ports);

    // MIDI listen channel. 0 = omni (accept any channel), 1..16 = that
    // channel only. Values outside [0,16] are clamped to 0 (omni).
    void    setMidiChannel(uint8_t ch);
    uint8_t midiChannel() const { return _midiCh; }

    // Oscillator balance + detune
    //
    // setDetuneCents: spread between osc1 and osc2 in cents. osc1 goes
    // to +cents/2, osc2 to -cents/2 (so total spread = cents). 0 = all
    // three saws in unison (phaser-ish); 7..14 = classic reese beating;
    // 30+ = noticeable chorus-like width. Clamped to [0, 50].
    //
    // setSubLevel: sine sub amplitude, 0..1. 0 = no sub; 0.6 = default
    // (strong but below saw level).
    //
    // setOsc3Level: middle saw amplitude, 0..1. 0 = only detuned pair
    // (hollower sound); 0.7 = default (rich mid).
    void  setDetuneCents(float cents);
    void  setSubLevel   (float level);
    void  setOsc3Level  (float level);
    float detuneCents() const { return _detuneCents; }
    float subLevel   () const { return _subLevel;    }
    float osc3Level  () const { return _osc3Level;   }

    // Envelope — attack / release in SECONDS. Decay + sustain stay at
    // defaults set externally on the AudioEffectEnvelope instance
    // (Phase 1 exposes only A/R like MpeVaSink).
    void  setAttack (float seconds);
    void  setRelease(float seconds);
    float attack()  const { return _attackSec;  }
    float release() const { return _releaseSec; }

    // Resonant LP filter
    //
    // Range: cutoff 20..20000 Hz, resonance 0.707..5.0 (matches
    // AudioFilterStateVariable's accepted range — 5.0 self-oscillates).
    // Default cutoff 600 Hz, resonance 2.5 — aggressive reese sweet spot.
    void  setFilterCutoff   (float hz);
    void  setFilterResonance(float q);
    float filterCutoff()    const { return _filterCutoffHz;  }
    float filterResonance() const { return _filterResonance; }

    // Per-voice amplitude trim. Same semantics as MpeVaSink: multiplier
    // on every note's amplitude (velocity × _voiceVolumeScale). Left at
    // 1.0 by default; the sketch's g_neuroGain is the user-facing
    // volume fader.
    void  setVoiceVolumeScale(float s);
    float voiceVolumeScale() const { return _voiceVolumeScale; }

    // --- LFO -----------------------------------------------------------
    //
    // Same global-software LFO model as MpeVaSink: one phase, one
    // destination, updated in tick(). Rates <= 20 Hz so polling from
    // the main loop at block-rate granularity is musically identical
    // to a true audio-rate LFO.
    //
    // Destinations:
    //   OFF    = 0 — LFO disabled (cheap no-op in tick)
    //   CUTOFF = 1 — ± depth octaves around filter cutoff
    //   PITCH  = 2 — ± depth × 12 semitones around current pitch
    //   AMP    = 3 — downward tremolo (1 − depth)..1 amp multiplier
    //
    // Waveforms: 0=sine, 1=tri, 2=saw, 3=square.
    enum LfoDest : uint8_t {
        LfoOff    = 0,
        LfoCutoff = 1,
        LfoPitch  = 2,
        LfoAmp    = 3,
    };
    void    setLfoRate    (float hz);       // 0..20
    void    setLfoDepth   (float depth);    // 0..1
    void    setLfoDest    (uint8_t dest);
    void    setLfoWaveform(uint8_t wave);
    float   lfoRate()     const { return _lfoRateHz;   }
    float   lfoDepth()    const { return _lfoDepth;    }
    uint8_t lfoDest()     const { return _lfoDest;     }
    uint8_t lfoWaveform() const { return _lfoWaveform; }

    // Portamento time in milliseconds. 0 = snap (no glide). Up to 2000
    // is accepted; longer values are clamped. Glide is applied in tick().
    void  setPortamentoMs(float ms);
    float portamentoMs() const { return _portMs; }

    // Advance LFO + portamento glide. Call every main-loop iteration
    // (same contract as MpeVaSink::tick). Cheap no-op when nothing is
    // modulating.
    void tick(uint32_t nowMs);

    // --- MidiSink overrides ---------------------------------------------

    void onNoteOn     (uint8_t channel, uint8_t note, uint8_t velocity) override;
    void onNoteOff    (uint8_t channel, uint8_t note, uint8_t velocity) override;
    void onPitchBend  (uint8_t channel, float   semitones)              override;
    void onAllNotesOff(uint8_t channel)                                 override;

private:
    VoicePorts _ports;

    // Channel filter. _midiCh==0 means omni. _midiCh in 1..16 means
    // that exact channel only — messages on other channels are dropped.
    uint8_t _midiCh = 0;

    // Oscillator / balance params
    float _detuneCents    = 7.0f;
    float _subLevel       = 0.6f;
    float _osc3Level      = 0.7f;
    // Envelope
    float _attackSec      = 0.005f;
    float _releaseSec     = 0.300f;
    // Filter
    float _filterCutoffHz  = 600.0f;
    float _filterResonance = 2.5f;
    // Voice trim
    float _voiceVolumeScale = 1.0f;

    // LFO
    float   _lfoRateHz     = 0.0f;
    float   _lfoDepth      = 0.0f;
    uint8_t _lfoDest       = LfoOff;
    uint8_t _lfoWaveform   = 0;
    float   _lfoPitchSemi  = 0.0f;
    float   _lfoCutoffOct  = 0.0f;
    float   _lfoAmpMult    = 1.0f;

    // Portamento
    float    _portMs       = 0.0f;
    uint32_t _lastTickMs   = 0;

    // Mono voice state. _targetNote is the top of the held stack; the
    // osc frequencies glide toward noteToHz(_targetNote) + bend + LFO.
    // _currentHz is the actual frequency applied to the oscs right now
    // (so portamento can interpolate from here to the target).
    bool    _held         = false;
    uint8_t _targetNote   = 60;
    float   _bend_semi    = 0.0f;
    float   _velocity     = 0.0f;   // last note-on velocity, 0..1
    float   _targetHz     = 0.0f;   // semantic target (note + bend + LFO)
    float   _currentHz    = 0.0f;   // glided value currently applied to oscs

    // Note stack: up to 8 held notes for mono last-note priority. When
    // the user plays a chord, the most-recent note sounds. Releasing
    // the top falls back to the next one down without retriggering
    // the envelope (legato).
    static constexpr int kStackSize = 8;
    uint8_t _noteStack[kStackSize];
    int     _noteStackCount = 0;

    // Helpers
    bool channelMatches(uint8_t ch) const {
        // _midiCh 0 = omni; otherwise exact match. LinnStrument-style
        // MPE is NOT supported on Neuro (single mono voice) — if the
        // user sets omni and a controller sends on ch 2..16 those all
        // fight for the same voice, which is fine for a mono bass.
        return _midiCh == 0 || ch == _midiCh;
    }
    void pushNote (uint8_t note);     // O(N) but N<=8
    void popNote  (uint8_t note);     // remove exact match; no-op if absent
    bool topNote  (uint8_t *out) const;

    void   applyOscAmplitudes();      // writes osc.amplitude() × velocity × balance
    void   applyFrequenciesFromTarget();  // snaps or glides; writes osc.frequency()
    void   applyFilter();
    void   retargetFromTop(bool triggerEnv);  // sets _targetNote from stack top

    static float noteToHz(uint8_t note);
};
