// MultisampleSlot.h — ISynthSlot for SD-streaming multisample playback.
//
// Phase 2 of the synth-slot roadmap. Wraps newdigate/teensy-variable-playback
// (AudioPlaySdResmp) to play stereo WAV files directly off the Teensy 4.1's
// built-in SD card. Eight voices polyphonic; oldest-voice stealing.
//
// Bank format on SD:
//
//   /samples/<bank>/<note>.wav             (no velocity layers)
//   /samples/<bank>/<note>_v<N>.wav        (Salamander-style velocity layers)
//
//   where <note> is a name parsable by NoteParser ("C4", "F#3", "A0", "C-1", ...)
//   and <N> is 1..16 mapping linearly to MIDI velocity 8..127. Files
//   without a v-tag are treated as MIDI velocity 64 (mf). Multiple
//   velocity layers per note are supported; the voice picker chooses
//   the sample whose root note + velocity is closest to the played
//   (note, velocity) pair.
//
//   At least one sample required. Banks with no parseable .wav files are
//   ignored. WAV must be 48 kHz / stereo / 16-bit signed PCM (matches our
//   AUDIO_SAMPLE_RATE_EXACT and the int16 path the lib supports).
//
// The slot picks the closest sample by root note and pitch-shifts via
// AudioPlaySdResmp::setPlaybackRate(). Quadratic interpolation is on by
// default — better quality, slightly higher CPU.
//
// Audio nodes (player array, envelopes, mixer chain, bridge, gains) live
// at file scope in main.cpp — same pattern Dexed uses. The slot stores
// pointers to them via the AudioContext struct passed at construction.

#pragma once

#include <stdint.h>

#include <MidiSink.h>

#include "SynthSlot.h"

class AudioPlaySdResmp;
class AudioEffectEnvelope;
class AudioMixer4;
class AudioEffectGain_F32;

namespace tdsp_synth {

class MultisampleSlot : public ISynthSlot {
public:
    static constexpr int kVoices             = 8;
    // 1024 samples covers a fully velocity-layered piano: ~40 root notes
    // × 16 layers = 640 (Salamander Grand V3), plus headroom for other
    // velocity-layered banks. ~84 KB of static path strings — fits
    // comfortably in Teensy 4.1's RAM.
    static constexpr int kMaxSamples         = 1024;
    // Release samples are damper-thunk recordings played on key-up.
    // Per-note when filename includes a note name (Salamander's 88
    // per-key set, mapped to "_release_<note>.wav"); generic-pool
    // otherwise (e.g., "_release1.wav", round-robin). Stored separately
    // from pitched note samples — different filename parser, different
    // pick logic.
    static constexpr int kMaxReleaseSamples  = 128;   // ~10 KB of paths
    static constexpr int kBankPathLen        = 80;
    static constexpr int kBankNameLen        = 24;

    // The audio graph nodes the slot drives. Arrays must be sized
    // exactly to kVoices (players, envL, envR) and 3 (mix arrays —
    // [0],[1] are stage 1 4-into-1 mixers; [2] is the stage 2 final
    // 2-into-1 mixer).
    struct AudioContext {
        AudioPlaySdResmp    *players;   // [kVoices]
        AudioEffectEnvelope *envL;      // [kVoices]
        AudioEffectEnvelope *envR;      // [kVoices]
        AudioMixer4         *mixL;      // [3]
        AudioMixer4         *mixR;      // [3]
        AudioEffectGain_F32 *gainL;     // post-bridge per-slot fader (left)
        AudioEffectGain_F32 *gainR;     // post-bridge per-slot fader (right)
    };

    explicit MultisampleSlot(const AudioContext &ctx);

    // ISynthSlot
    const char*     id()          const override { return "sampler"; }
    const char*     displayName() const override;
    tdsp::MidiSink* midiSink()          override { return &_sink; }
    void            setActive(bool active) override;
    void            panic() override;

    // Initialize envelopes, mixer gains. Call once from setup() before
    // any noteOn arrives.
    void begin();

    // Scan an SD path for <note>.wav files. Replaces the current bank.
    // Returns the number of samples found. 0 = bank empty / not present.
    // Safe to call when slot is active (panics held notes).
    int scanBank(const char *bankPath);

    // Stop voices whose envelope has finished decaying. Call from loop()
    // at any cadence (cheap).
    void pollVoices();

    // Per-slot fader (X32 form 0..1) and on/off. The slot's audible
    // gain is (active && on) ? faderToLinear(volume) : 0; toggling on
    // / fader does NOT panic held notes — only switching the slot
    // away does.
    void setVolume(float v);
    void setOn(bool on);
    bool on() const { return _on; }
    float volume() const { return _volume; }

    // 0 = omni; 1..16 = single channel; >16 clamped to omni.
    void setListenChannel(uint8_t ch);
    uint8_t listenChannel() const { return _listenChannel; }

    // Read access to the bank — drives the dev-surface OSC tree later.
    int         numSamples()        const { return _numSamples; }
    int         numReleaseSamples() const { return _numReleaseSamples; }
    const char* bankName()          const { return _bankName; }
    uint8_t     sampleRoot(int idx) const {
        return (idx >= 0 && idx < _numSamples) ? _sampleRoot[idx] : 0;
    }

private:
    class Sink : public tdsp::MidiSink {
    public:
        explicit Sink(MultisampleSlot *slot) : _slot(slot) {}
        void onNoteOn (uint8_t channel, uint8_t note, uint8_t velocity) override;
        void onNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) override;
        void onSustain(uint8_t channel, bool on) override;
        void onAllNotesOff(uint8_t channel) override;
    private:
        MultisampleSlot *_slot;
    };

    // Sustain pedal handler — exposed so OSC handlers can drive it
    // independently of MIDI input (e.g., a UI toggle).
    void setSustain(bool on);

    AudioContext _ctx;
    Sink         _sink;

    // Voice state. _voiceNote[i] == -1 means idle (or playing a release
    // sample — distinguished by _voiceIsRelease[i]). Voices in their
    // release fade hold the original note in _voiceNote until
    // pollVoices() observes the envelope finishing and frees the slot.
    int8_t   _voiceNote     [kVoices];
    uint8_t  _voiceVel      [kVoices];   // attack velocity for the held note
    uint32_t _voiceStartMs  [kVoices];
    bool     _voiceReleasing[kVoices];
    bool     _voiceIsRelease[kVoices];   // voice is playing a release sample

    // Bank state. _sampleVelocity holds each sample's recorded velocity
    // in MIDI 0..127 space (default 64 = mf for files without a v-tag).
    // The picker uses (root note, velocity) jointly to choose the best
    // sample for an incoming (note, velocity) — see closestSampleIdx().
    char    _bankName      [kBankNameLen];
    char    _samplePath    [kMaxSamples][kBankPathLen];
    uint8_t _sampleRoot    [kMaxSamples];
    uint8_t _sampleVelocity[kMaxSamples];
    int     _numSamples = 0;

    // Release samples — short damper-thunk recordings played on every
    // note-off. Polyphony shared with the main voice pool: if no voice
    // is free at note-off time, the release sample is skipped (graceful
    // degradation under load).
    //
    // _releaseRoot[i] = MIDI note this sample was recorded for, or
    // 0xFF for samples without a parseable note in the filename
    // (those fall back to round-robin via _releaseRrCounter).
    char    _releasePath   [kMaxReleaseSamples][kBankPathLen];
    uint8_t _releaseRoot   [kMaxReleaseSamples];
    int     _numReleaseSamples = 0;
    uint8_t _releaseRrCounter  = 0;
    static constexpr uint8_t kReleaseNoNote = 0xFF;

    // Slot config.
    bool    _active = false;
    bool    _on     = true;
    float   _volume = 0.75f;
    uint8_t _listenChannel = 0;

    // Sustain pedal (CC#64). When down, doNoteOff() suppresses the
    // per-voice release until the pedal is lifted, then all sustained
    // voices release together. _voiceSustained[i] flags voices whose
    // noteOff was deferred by the pedal.
    bool    _sustain = false;
    bool    _voiceSustained[kVoices];

    bool listens(uint8_t ch) const { return _listenChannel == 0 || _listenChannel == ch; }

    void doNoteOn(uint8_t note, uint8_t velocity);
    void doNoteOff(uint8_t note);
    void releaseVoice(int v);
    void hardStopVoice(int v);
    void hardStopAll();
    // Trigger a one-shot release sample for the released MIDI note.
    // Picks the closest-by-root-note sample if any have notes assigned;
    // falls back to round-robin among unassigned samples. Velocity
    // scales the playback gain.
    //
    // `pedal` distinguishes per-key release (subtle, almost subliminal —
    // every note off in normal play) from pedal-release (clear collective
    // damper-thunk on sustain-pedal lift). Different gain constants for
    // each so the collective sounds intentional and the per-key ones don't
    // call attention to themselves.
    void triggerReleaseSample(uint8_t note, uint8_t velocity, bool pedal);

    int  pickVoice(uint8_t note);
    // Hierarchical pick: closest root note first, then closest velocity
    // among samples with that root distance. Returns -1 if bank empty.
    int  closestSampleIdx(uint8_t note, uint8_t velocity) const;

    void initEnvelopes();
    void applyGain();
};

}  // namespace tdsp_synth
