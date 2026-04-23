// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.
//
// ChipSink — NES/Gameboy-inspired monophonic chiptune voice.
//
// Signal path (4 sources summed into a 4-input mixer):
//   Pulse 1 (duty-cycle selectable) at root
//   Pulse 2 (duty-cycle selectable) at root × voicing interval (unison,
//     octave, fifth, or third) with optional detune
//   Triangle (sub, 1 octave down)
//   Noise (LFSR-emulating) with adjustable level
//
// Options:
//   Voicing: how pulse 2 relates to pulse 1 per note. Unison, octave
//     up, perfect 5th up, major 3rd up. This is the chiptune "chord"
//     trick — a single key press makes the NES-style 2-voice harmony
//     without the user needing to play chords.
//   Arpeggio: cycle through root, 3rd, 5th on the triangle at rate N
//     Hz. When combined with voicing, gives the classic NES boss-fight
//     texture.
//   Envelope: full ADSR but with chiptune-appropriate defaults (short
//     attack + decay, low sustain).
//
// Mono, last-note priority. Pitch bend moves all 4 sources together.

#pragma once

#include <stdint.h>

#include <MidiSink.h>

class AudioSynthWaveform;
class AudioSynthNoisePink;
class AudioMixer4;
class AudioEffectEnvelope;

class ChipSink : public tdsp::MidiSink {
public:
    struct VoicePorts {
        AudioSynthWaveform  *pulse1;   // pulse 1 (PULSE waveform, duty via pulseWidth)
        AudioSynthWaveform  *pulse2;   // pulse 2
        AudioSynthWaveform  *triangle; // triangle sub (-12 semi)
        AudioSynthNoisePink *noise;    // pink noise for hi-hats/gunshots
        AudioMixer4         *mix;      // slot 0..3 = pulse1, pulse2, tri, noise
        AudioEffectEnvelope *env;
    };

    explicit ChipSink(const VoicePorts &ports);

    void    setMidiChannel(uint8_t ch);
    uint8_t midiChannel() const { return _midiCh; }

    // Pulse duty cycles: 0 = 12.5%, 1 = 25%, 2 = 50% (square), 3 = 75%.
    void    setPulse1Duty(uint8_t d);
    void    setPulse2Duty(uint8_t d);
    uint8_t pulse1Duty() const { return _pulse1Duty; }
    uint8_t pulse2Duty() const { return _pulse2Duty; }

    // Pulse 2 detune relative to its voiced interval, in cents.
    void  setPulse2Detune(float cents);
    float pulse2Detune() const { return _pulse2Detune; }

    // Layer levels, 0..1.
    void  setTriangleLevel(float v);
    void  setNoiseLevel(float v);
    float triangleLevel() const { return _triLevel; }
    float noiseLevel()    const { return _noiseLevel; }

    // Voicing: how pulse 2 sits above pulse 1.
    //   0 = unison       (pulse 2 at same note as pulse 1)
    //   1 = octave       (pulse 2 = +12)
    //   2 = fifth        (pulse 2 = +7)
    //   3 = major third  (pulse 2 = +4)
    void    setVoicing(uint8_t v);
    uint8_t voicing() const { return _voicing; }

    // Arpeggio on the TRIANGLE only (so the pulses keep the held
    // note). 0 = off, 1 = up (root→5th→octave→repeat), 2 = down,
    // 3 = random.
    void    setArpeggio(uint8_t mode);
    uint8_t arpeggio() const { return _arpMode; }

    // Arpeggio rate in Hz. Typical chiptune: 8-24 Hz.
    void  setArpRate(float hz);
    float arpRate() const { return _arpRateHz; }

    // Full ADSR.
    void  setAttack (float s);
    void  setDecay  (float s);
    void  setSustain(float v);
    void  setRelease(float s);
    float attack()  const { return _attackSec;  }
    float decay()   const { return _decaySec;   }
    float sustain() const { return _sustain;    }
    float release() const { return _releaseSec; }

    void setVoiceVolumeScale(float s);

    // Advance arpeggio. Call from loop().
    void tick(uint32_t nowMs);

    void onNoteOn     (uint8_t, uint8_t, uint8_t) override;
    void onNoteOff    (uint8_t, uint8_t, uint8_t) override;
    void onPitchBend  (uint8_t, float)            override;
    void onAllNotesOff(uint8_t)                   override;

private:
    VoicePorts _ports;

    uint8_t _midiCh        = 0;
    uint8_t _pulse1Duty    = 2;   // 50%
    uint8_t _pulse2Duty    = 1;   // 25%
    float   _pulse2Detune  = 7.0f;
    float   _triLevel      = 0.5f;
    float   _noiseLevel    = 0.0f;
    uint8_t _voicing       = 1;   // octave
    uint8_t _arpMode       = 0;   // off
    float   _arpRateHz     = 12.0f;
    float   _attackSec     = 0.001f;
    float   _decaySec      = 0.08f;
    float   _sustain       = 0.5f;
    float   _releaseSec    = 0.15f;
    float   _voiceVolumeScale = 1.0f;

    bool     _held         = false;
    uint8_t  _targetNote   = 60;
    float    _bend_semi    = 0.0f;
    float    _velocity     = 0.0f;

    uint8_t  _arpStep      = 0;
    uint32_t _lastArpMs    = 0;

    static constexpr int kStackSize = 8;
    uint8_t  _noteStack[kStackSize];
    int      _noteStackCount = 0;

    bool channelMatches(uint8_t ch) const { return _midiCh == 0 || ch == _midiCh; }
    void pushNote(uint8_t);
    void popNote (uint8_t);
    bool topNote (uint8_t *out) const;

    void applyDuties();
    void applyLevels();
    void applyFrequencies();    // writes all 4 sources' freqs from _targetNote
    void applyTriangleArpFreq();  // just triangle for arp updates

    static float pulseWidthForDuty(uint8_t d);
    static int   voicingInterval(uint8_t v);
    static float noteToHz(uint8_t note);
};
