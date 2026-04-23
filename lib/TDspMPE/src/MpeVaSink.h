// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.
//
// MpeVaSink — MPE-aware voice allocator for a virtual-analog synth built
// on stock Teensy Audio primitives.
//
// Why this wrapper exists
// -----------------------
// DexedSink (see projects/.../src/DexedSink.h) shares global pitch bend
// / mod wheel / pressure across every sounding voice. That is fine for
// DAW-style playback but useless for MPE controllers like LinnStrument
// where each note needs its own expression. MpeVaSink keeps per-voice
// state — which MIDI channel owns the voice, what note it's playing,
// whether it is still held — so a channel-N pitch bend or pressure
// message only touches the voice(s) started on channel N.
//
// MPE convention recap
// --------------------
// * Channel 1 (default master channel) carries global messages that
//   should apply to every voice — mod wheel, sustain, program changes.
//   Notes on the master channel are IGNORED by this sink; a true MPE
//   controller only sends notes on member channels.
// * Channels 2..16 are member channels. Each note-on picks a free voice
//   and remembers the channel; subsequent pitch bend / pressure on that
//   channel steers that voice only.
// * The router pre-scales pitch bend into floating-point semitones using
//   the per-channel pitch bend range (RPN 0), so onPitchBend() receives
//   signed semitones ready to apply. Pressure is already 0..1.
//
// Allocation + stealing
// ---------------------
// Fixed pool of kVoiceCount voices (compile-time 4 for Phase 2d). On
// note-on:
//   1. Prefer a voice with note_held == false (released / never held).
//      Among these, pick the one with the oldest start_time so a voice
//      that has been decaying a long time is retriggered before a voice
//      whose release tail is still fresh.
//   2. If every voice is held, steal the one with the oldest start_time
//      (true LRU). The re-triggered envelope implicitly cuts the old
//      note's tail.
// On note-off the matching voice's envelope is released and note_held
// is cleared. start_time is deliberately left intact so the freshly
// released voice isn't immediately stolen by the next note-on — only
// voices that have been released for a while become steal candidates.
//
// Thread / context safety
// -----------------------
// All MidiSink methods run on whatever context drives the router
// (USB host poll loop, OSC handler, serial bridge). The sink does no
// dynamic allocation and touches only its own state plus the
// AudioSynthWaveform / AudioEffectEnvelope objects it was handed — safe
// to call from those contexts. It does NOT touch the audio ISR directly;
// AudioSynthWaveform::frequency / amplitude / AudioEffectEnvelope::noteOn
// are themselves safe to call from foreground.

#pragma once

#include <stdint.h>

#include <MidiSink.h>

// Forward-declare Teensy Audio primitives so the header doesn't force
// every includer to drag in Audio.h. MpeVaSink.cpp pulls in Audio.h.
class AudioSynthWaveform;
class AudioEffectEnvelope;
class AudioFilterStateVariable;

class MpeVaSink : public tdsp::MidiSink {
public:
    // Wiring struct passed to the sink at construction. One per voice.
    // The sketch owns the actual AudioSynthWaveform / AudioEffectEnvelope
    // instances (Teensy Audio objects must be statically allocated so
    // their ctors register with the audio graph) and hands us pointers.
    struct VoicePorts {
        AudioSynthWaveform       *osc;
        AudioEffectEnvelope      *env;
        AudioFilterStateVariable *filter = nullptr;  // optional (Phase 2d2)
    };

    // Maximum voices the sink can manage. kVoiceCount is the Phase 2d
    // default; kMaxVoices bounds the internal storage so different
    // builds can choose a different count without changing headers.
    static constexpr int kVoiceCount = 4;
    static constexpr int kMaxVoices  = 8;

    // voices: array of kVoiceCount VoicePorts. Pointer and pointees
    // must outlive the sink (typically file-scope statics in main.cpp).
    // voiceCount: how many entries are valid in `voices`. Clamped to
    // [0, kMaxVoices]; values above kMaxVoices are truncated silently.
    MpeVaSink(VoicePorts *voices, int voiceCount);

    // Master channel. 1..16 = standard MPE behaviour: notes on this
    // channel are IGNORED (only global messages — mod wheel, sustain,
    // program change — are honored there). 0 = no master channel;
    // notes on every channel 1..16 allocate voices, which is what you
    // want when a plain non-MPE controller is driving the sink. Values
    // > 16 are clamped to 0.
    void    setMasterChannel(uint8_t ch);
    uint8_t masterChannel() const { return _masterChannel; }

    // Oscillator waveform applied to ALL voices. 0=saw, 1=square,
    // 2=triangle, 3=sine. Values outside that range are clamped to saw.
    // Calling this mid-note is legal — the osc switches instantly, which
    // sounds like a timbre change rather than a click since the envelope
    // is still open.
    void    setWaveform(uint8_t wave);
    uint8_t waveform() const { return _waveform; }

    // Envelope attack / release in SECONDS (the Teensy library takes
    // ms internally; we convert). Decay + sustain stay at the defaults
    // set in the AudioEffectEnvelope ctor (35 ms / 50%). A dedicated
    // attack/release pair is enough to make the synth respond to MPE
    // expression musically without surfacing full ADSR into the OSC
    // surface for Phase 2d.
    void  setAttack (float seconds);
    void  setRelease(float seconds);
    float attack()  const { return _attackSec; }
    float release() const { return _releaseSec; }

    // Per-voice amplitude ceiling. Every voice's amplitude is
    // velocity × pressure × _volumeScale; this is a sketch-visible trim
    // that keeps head-room under polyphonic pile-ups. Left at 1.0 by
    // default; the sketch's own gain stage (mpeGainL/R) handles the
    // user-facing "synth volume" fader and this stays fixed.
    void  setVoiceVolumeScale(float s);
    float voiceVolumeScale() const { return _volumeScale; }

    // --- Filter (Phase 2d2) --------------------------------------------
    //
    // Per-voice resonant state-variable filter (lowpass). All voices
    // share the same base cutoff + resonance. Per-voice cutoff is then
    // multiplied by a CC#74-driven timbre factor so each MPE finger
    // steers its own note's brightness independently of every other
    // held note. Range: timbre 0..1 maps to cutoff × 2^(-1..+1), i.e.
    // ±1 octave around the base — enough travel to sound expressive
    // without the filter self-oscillating at either extreme.
    //
    // If a voice was constructed with a null filter pointer (legacy
    // Phase 2d wiring with no filter in the chain), filter writes are
    // no-ops for that voice — the rest of the sink still works.
    void  setFilterCutoff   (float hz);      // 20..20000
    void  setFilterResonance(float q);       // 0.707..5.0
    float filterCutoff()    const { return _filterCutoffHz; }
    float filterResonance() const { return _filterResonance; }

    // --- LFO (Phase 2d3) -----------------------------------------------
    //
    // Global software LFO — one phase, one destination, all voices
    // share it. We don't burn a Teensy Audio primitive per voice for
    // the LFO: the rates we care about (<= 20 Hz) are so far below
    // the audio rate that polling from the main loop (tick()) is
    // indistinguishable from block-rate modulation. Saves ~20 audio
    // primitives for the same musical effect.
    //
    // Destinations (LfoDest): OFF = 0, CUTOFF, PITCH, AMP.
    //   CUTOFF: ±(depth) octaves added to voice's filter cutoff
    //   PITCH:  ±(depth × 12) semitones added to voice's frequency
    //   AMP:    tremolo — amplitude multiplier swings from (1 − depth)
    //           to 1 (modulation is only downward, so peaks stay at
    //           the note's natural loudness — matches how a real amp
    //           tremolo works).
    //
    // Waveforms: 0=sine, 1=triangle, 2=saw, 3=square. Matches the VA
    // OSC codes but the LFO set is chosen for smooth modulation
    // shapes — saw gives a ramp-up-then-reset effect; square is a
    // hard on/off switch.
    //
    // Call tick(nowMs) every iteration of the main loop (1–10 ms
    // granularity is fine). When LFO is disabled (dest=OFF or depth=0),
    // tick() short-circuits and does nothing — no cost when unused.
    enum LfoDest : uint8_t {
        LfoOff    = 0,
        LfoCutoff = 1,
        LfoPitch  = 2,
        LfoAmp    = 3,
    };

    void    setLfoRate    (float hz);       // 0..20, 0 also disables
    void    setLfoDepth   (float depth);    // 0..1
    void    setLfoDest    (uint8_t dest);   // LfoDest enum
    void    setLfoWaveform(uint8_t wave);   // 0=sine, 1=tri, 2=saw, 3=square
    float   lfoRate()     const { return _lfoRateHz; }
    float   lfoDepth()    const { return _lfoDepth; }
    uint8_t lfoDest()     const { return _lfoDest; }
    uint8_t lfoWaveform() const { return _lfoWaveform; }

    // Advance the LFO and re-apply its contribution to every held voice.
    // Cheap no-op when dest=OFF or depth=0. Idempotent — calling it
    // twice in a row with the same nowMs is safe.
    void tick(uint32_t nowMs);

    // --- Voice telemetry (Phase 2d4) -----------------------------------
    //
    // Read-only snapshot of per-voice state, flat struct, no pointers —
    // safe to copy and broadcast as OSC. The sketch consumes these at
    // ~30 Hz to feed a web UI that draws one "voice orb" per active
    // voice (pitch → vertical position, pressure → radius, channel →
    // hue, held/released → brightness).
    //
    // The snapshot includes the LFO's CURRENT contribution (note +
    // bend + lfoPitchSemi composed into a final pitch offset),
    // because the UI wants to render what you're hearing, not what
    // the MIDI controller last sent. An orb visibly wobbling during
    // a pitch-LFO run is the point.
    struct VoiceSnapshot {
        bool     held;        // true iff the voice is currently note-on
        uint8_t  channel;     // 1..16 MIDI channel; 0 when never used
        uint8_t  note;        // 0..127 MIDI note
        float    pitchSemi;   // signed semitone offset from note (bend + LFO)
        float    pressure;    // last observed pressure, 0..1
        float    timbre;      // last observed CC#74, 0..1
    };

    // Copy the current per-voice state into `out`. Writes min(maxVoices,
    // voiceCount()) entries. Returns the number of entries written.
    // `out` may be null if `maxVoices == 0` (returns 0, no-op).
    int voiceSnapshot(VoiceSnapshot *out, int maxVoices) const;
    int voiceCount() const { return _voiceCount; }

    // --- tdsp::MidiSink overrides ---------------------------------------

    void onNoteOn    (uint8_t channel, uint8_t note, uint8_t velocity) override;
    void onNoteOff   (uint8_t channel, uint8_t note, uint8_t velocity) override;
    void onPitchBend (uint8_t channel, float   semitones)              override;
    void onPressure  (uint8_t channel, float   value)                  override;
    void onTimbre    (uint8_t channel, float   value)                  override;
    void onAllNotesOff(uint8_t channel)                                override;

private:
    struct Voice {
        uint8_t  channel    = 0;      // MIDI channel 1..16 that owns this voice
        uint8_t  note       = 0;      // MIDI note number (0..127)
        bool     note_held  = false;  // true between noteOn and noteOff
        uint32_t start_time = 0;      // monotonic counter (see _counter)
        float    base_amp   = 0.0f;   // amplitude before pressure modulation
        float    bend_semi  = 0.0f;   // last pitch bend in semitones
        float    timbre     = 0.5f;   // CC#74 (Phase 2d2), 0..1, 0.5 = neutral
        float    pressure   = 0.0f;   // last channel pressure, 0..1
    };

    VoicePorts *_voices;              // borrowed pointer, not owned
    Voice       _state[kMaxVoices];   // parallel per-voice state
    int         _voiceCount;          // effective count, clamped
    uint32_t    _counter = 0;         // monotonic increment for LRU
    uint8_t     _masterChannel = 1;
    uint8_t     _waveform      = 0;   // OSC code 0 = saw (default)
    float       _attackSec     = 0.005f;
    float       _releaseSec    = 0.300f;
    float       _volumeScale   = 1.0f;
    float       _filterCutoffHz   = 8000.0f;  // base cutoff before timbre
    float       _filterResonance  = 0.707f;   // butterworth-ish, no peak
    float       _channelTimbre[16];           // last CC#74 per MIDI channel 1..16

    // LFO state. _lfoPitchSemi etc. are the *current applied* offsets,
    // updated by tick() and read by apply*() helpers — so LFO and MIDI
    // events compose additively without either clobbering the other.
    float       _lfoRateHz        = 0.0f;     // disabled by default
    float       _lfoDepth         = 0.0f;
    uint8_t     _lfoDest          = LfoOff;
    uint8_t     _lfoWaveform      = 0;        // sine
    float       _lfoPitchSemi     = 0.0f;     // current LFO → pitch semitones
    float       _lfoCutoffOct     = 0.0f;     // current LFO → cutoff octaves
    float       _lfoAmpMult       = 1.0f;     // current LFO → amp multiplier

    // Helpers
    int  pickVoice();                          // allocation / stealing
    int  findActive(uint8_t ch, uint8_t note); // channel+note match, held only
    void applyFrequency(int vi);               // osc.frequency using note+bend+LFO
    void applyAmplitude(int vi);               // osc.amplitude using voice.pressure+LFO
    void applyFilter   (int vi);               // per-voice cutoff from base×timbre+LFO

    static float noteToHz(uint8_t note);
    static short toneTypeFor(uint8_t wave);
};
