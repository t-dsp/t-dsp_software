// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.

#include "MpeVaSink.h"

#include <math.h>

#include <Audio.h>  // AudioSynthWaveform, AudioEffectEnvelope, WAVEFORM_*

MpeVaSink::MpeVaSink(VoicePorts *voices, int voiceCount)
    : _voices(voices)
{
    if (voiceCount < 0)          voiceCount = 0;
    if (voiceCount > kMaxVoices) voiceCount = kMaxVoices;
    _voiceCount = voiceCount;

    // Default every channel's CC#74 state to neutral (0.5). New note-ons
    // inherit this so a fresh voice starts with centered brightness
    // even if the controller hasn't sent a CC#74 yet.
    for (int i = 0; i < 16; ++i) _channelTimbre[i] = 0.5f;

    // Seed each voice's tone type, envelope timings, silent amplitude,
    // and filter cutoff so the graph is in a defined state even before
    // the first note-on.
    for (int i = 0; i < _voiceCount; ++i) {
        if (_voices[i].osc) {
            _voices[i].osc->begin(toneTypeFor(_waveform));
            _voices[i].osc->amplitude(0.0f);
            _voices[i].osc->frequency(440.0f);
        }
        if (_voices[i].env) {
            _voices[i].env->attack (_attackSec  * 1000.0f);
            _voices[i].env->release(_releaseSec * 1000.0f);
        }
        if (_voices[i].filter) {
            _voices[i].filter->frequency(_filterCutoffHz);
            _voices[i].filter->resonance(_filterResonance);
        }
    }
}

void MpeVaSink::setMasterChannel(uint8_t ch) {
    // 0 means "no master channel" — notes on every channel 1..16 allocate
    // voices, as if the entire MIDI space were member channels. Useful
    // when the incoming controller is a plain non-MPE keyboard and the
    // user just wants MPE to play anything that arrives. 1..16 = standard
    // MPE behaviour (that channel is global, notes on it are ignored).
    // Values > 16 are clamped to 0 (no master).
    if (ch > 16) ch = 0;
    _masterChannel = ch;
}

void MpeVaSink::setWaveform(uint8_t wave) {
    if (wave > 3) wave = 0;
    _waveform = wave;
    const short t = toneTypeFor(wave);
    for (int i = 0; i < _voiceCount; ++i) {
        if (_voices[i].osc) _voices[i].osc->begin(t);
    }
}

void MpeVaSink::setAttack(float seconds) {
    if (seconds < 0.0f) seconds = 0.0f;
    _attackSec = seconds;
    const float ms = seconds * 1000.0f;
    for (int i = 0; i < _voiceCount; ++i) {
        if (_voices[i].env) _voices[i].env->attack(ms);
    }
}

void MpeVaSink::setRelease(float seconds) {
    if (seconds < 0.0f) seconds = 0.0f;
    _releaseSec = seconds;
    const float ms = seconds * 1000.0f;
    for (int i = 0; i < _voiceCount; ++i) {
        if (_voices[i].env) _voices[i].env->release(ms);
    }
}

void MpeVaSink::setVoiceVolumeScale(float s) {
    if (s < 0.0f) s = 0.0f;
    if (s > 1.0f) s = 1.0f;
    _volumeScale = s;
}

void MpeVaSink::setFilterCutoff(float hz) {
    if (hz < 20.0f)    hz = 20.0f;
    if (hz > 20000.0f) hz = 20000.0f;
    _filterCutoffHz = hz;
    for (int i = 0; i < _voiceCount; ++i) applyFilter(i);
}

void MpeVaSink::setFilterResonance(float q) {
    // Match AudioFilterStateVariable's acceptable range (0.7..5.0) so
    // we don't ship values that would be silently clamped by the
    // library. 0.707 is Butterworth (no resonant peak); 5.0 is the
    // library's self-oscillation ceiling.
    if (q < 0.707f) q = 0.707f;
    if (q > 5.0f)   q = 5.0f;
    _filterResonance = q;
    for (int i = 0; i < _voiceCount; ++i) {
        if (_voices[i].filter) _voices[i].filter->resonance(q);
    }
}

// --- LFO -----------------------------------------------------------

void MpeVaSink::setLfoRate(float hz) {
    if (hz < 0.0f)  hz = 0.0f;
    if (hz > 20.0f) hz = 20.0f;
    _lfoRateHz = hz;
}

void MpeVaSink::setLfoDepth(float depth) {
    if (depth < 0.0f) depth = 0.0f;
    if (depth > 1.0f) depth = 1.0f;
    _lfoDepth = depth;
}

void MpeVaSink::setLfoDest(uint8_t dest) {
    if (dest > LfoAmp) dest = LfoOff;
    if (_lfoDest != dest) {
        // Reset LFO contributions when destination changes so a stale
        // value (e.g. we were modulating cutoff, now switched to amp)
        // doesn't leak past the changeover. Next tick() writes the
        // new destination's contribution.
        _lfoPitchSemi = 0.0f;
        _lfoCutoffOct = 0.0f;
        _lfoAmpMult   = 1.0f;
        _lfoDest      = dest;
        // Re-apply the clean state so voices settle back to their
        // un-modulated frequency/amp/cutoff immediately on OFF.
        for (int i = 0; i < _voiceCount; ++i) {
            if (!_state[i].note_held) continue;
            applyFrequency(i);
            applyAmplitude(i);
            applyFilter   (i);
        }
    }
}

void MpeVaSink::setLfoWaveform(uint8_t wave) {
    if (wave > 3) wave = 0;
    _lfoWaveform = wave;
}

static float lfoShape(uint8_t wave, float phase01) {
    // phase01 ∈ [0, 1); return −1..+1.
    static constexpr float kTwoPi = 6.2831853071795864f;
    switch (wave) {
        case 0: {  // sine
            // sinf produces −1..+1 directly over a full 2π period.
            return sinf(phase01 * kTwoPi);
        }
        case 1: {  // triangle: rises −1..+1 over first half, falls +1..−1 over second
            return (phase01 < 0.5f)
                 ? (4.0f * phase01 - 1.0f)
                 : (3.0f - 4.0f * phase01);
        }
        case 2:    // saw: linear ramp −1..+1
            return 2.0f * phase01 - 1.0f;
        case 3:    // square: +1 first half, −1 second half
            return (phase01 < 0.5f) ? 1.0f : -1.0f;
        default:
            return 0.0f;
    }
}

void MpeVaSink::tick(uint32_t nowMs) {
    // Short-circuit when the LFO contributes nothing. Protects the
    // audio graph from per-loop write traffic when the user hasn't
    // engaged the LFO — rate / depth / dest all have to be non-trivial
    // before we do anything.
    if (_lfoDest == LfoOff)     return;
    if (_lfoDepth  <= 0.0f)     return;
    if (_lfoRateHz <= 0.0f)     return;

    const float periodMs = 1000.0f / _lfoRateHz;
    // fmodf on a 32-bit ms counter can lose precision over days of
    // uptime, but we only care about periods under ~10 s — within
    // that window the modulo is fine.
    const float phase01  = fmodf((float)nowMs, periodMs) / periodMs;
    const float shape    = lfoShape(_lfoWaveform, phase01);  // −1..+1
    const float scaled   = shape * _lfoDepth;                // −depth..+depth

    _lfoPitchSemi = 0.0f;
    _lfoCutoffOct = 0.0f;
    _lfoAmpMult   = 1.0f;
    switch (_lfoDest) {
        case LfoPitch:
            // Full depth = ±1 octave (12 semitones). Nudge down for
            // gentler vibrato defaults.
            _lfoPitchSemi = scaled * 12.0f;
            break;
        case LfoCutoff:
            // scaled is already in "octaves of swing" units directly.
            _lfoCutoffOct = scaled;
            break;
        case LfoAmp: {
            // Classic downward-only tremolo: multiplier swings between
            // (1 − depth) and 1. Peak stays at note's natural level;
            // trough dips by depth. Map shape (−1..+1) → (0..1) and
            // scale: mult = 1 − depth × (1 − (shape+1)/2).
            const float unipolar = (shape + 1.0f) * 0.5f;       // 0..1
            _lfoAmpMult = 1.0f - _lfoDepth * (1.0f - unipolar); // (1-depth)..1
            break;
        }
        default:
            break;
    }

    // Push the new contribution into every held voice. Released voices
    // are intentionally left alone — their envelope tail has its own
    // shape, and LFO-ing that sounds wrong.
    for (int i = 0; i < _voiceCount; ++i) {
        if (!_state[i].note_held) continue;
        switch (_lfoDest) {
            case LfoPitch:  applyFrequency(i); break;
            case LfoCutoff: applyFilter   (i); break;
            case LfoAmp:    applyAmplitude(i); break;
            default:                           break;
        }
    }
}

// ---------------------------------------------------------------------
// MidiSink overrides
// ---------------------------------------------------------------------

void MpeVaSink::onNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    // Master-channel notes are by MPE convention "global" and should not
    // allocate a voice. LinnStrument etc. don't send note-on here, but
    // misconfigured controllers might — silently ignore instead of
    // producing a voice that nobody can pitch-bend or release cleanly.
    // _masterChannel == 0 disables this ignore entirely, so a plain
    // non-MPE controller can play MPE on any channel including 1.
    if (_masterChannel != 0 && channel == _masterChannel) return;
    if (channel < 1 || channel > 16) return;
    if (_voiceCount <= 0)            return;

    const int vi = pickVoice();
    if (vi < 0) return;  // defensive: pickVoice only returns <0 when count==0

    Voice &v = _state[vi];
    v.channel    = channel;
    v.note       = note;
    v.note_held  = true;
    v.start_time = ++_counter;
    v.bend_semi  = 0.0f;
    v.base_amp   = (velocity <= 0 ? 0.0f : (float)velocity / 127.0f)
                 * _volumeScale;
    // Inherit the channel's most recent CC#74 — so if a LinnStrument
    // key is pressed while the finger is already at a non-center
    // height, the voice starts at the correct brightness instead of
    // snapping from neutral to wherever on the next CC#74 frame.
    v.timbre     = _channelTimbre[channel - 1];
    v.pressure   = 0.0f;

    applyFrequency(vi);
    applyAmplitude(vi);
    applyFilter   (vi);
    if (_voices[vi].env) _voices[vi].env->noteOn();
}

void MpeVaSink::onNoteOff(uint8_t channel, uint8_t note, uint8_t /*velocity*/) {
    const int vi = findActive(channel, note);
    if (vi < 0) return;

    _state[vi].note_held = false;
    if (_voices[vi].env) _voices[vi].env->noteOff();
    // start_time is intentionally NOT refreshed — we want the releasing
    // voice to become a steal candidate only after actual-held voices do.
}

void MpeVaSink::onPitchBend(uint8_t channel, float semitones) {
    for (int i = 0; i < _voiceCount; ++i) {
        Voice &v = _state[i];
        if (!v.note_held || v.channel != channel) continue;
        v.bend_semi = semitones;
        applyFrequency(i);
    }
}

void MpeVaSink::onPressure(uint8_t channel, float value) {
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    for (int i = 0; i < _voiceCount; ++i) {
        Voice &v = _state[i];
        if (!v.note_held || v.channel != channel) continue;
        v.pressure = value;
        applyAmplitude(i);
    }
}

void MpeVaSink::onTimbre(uint8_t channel, float value) {
    if (channel < 1 || channel > 16) return;
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    _channelTimbre[channel - 1] = value;
    // Update every voice currently owned by this channel. A held note
    // has its filter brightness follow the channel's CC#74 in real
    // time; a released voice doesn't (the sound is in its tail and
    // re-opening the filter on a dying note would sound wrong).
    for (int i = 0; i < _voiceCount; ++i) {
        Voice &v = _state[i];
        if (!v.note_held || v.channel != channel) continue;
        v.timbre = value;
        applyFilter(i);
    }
}

void MpeVaSink::onAllNotesOff(uint8_t channel) {
    // channel == 0 is the panic convention (release everything); otherwise
    // only release voices that were allocated on that channel.
    for (int i = 0; i < _voiceCount; ++i) {
        Voice &v = _state[i];
        if (channel != 0 && v.channel != channel) continue;
        if (_voices[i].env) _voices[i].env->noteOff();
        v.note_held = false;
    }
}

// ---------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------

int MpeVaSink::pickVoice() {
    if (_voiceCount <= 0) return -1;

    // Pass 1: prefer a voice that is not currently held. Among candidates
    // pick the oldest start_time so long-released voices get reused before
    // recently-released ones (their tails are nearly silent anyway).
    int      bestIdle     = -1;
    uint32_t bestIdleTime = 0;
    for (int i = 0; i < _voiceCount; ++i) {
        if (_state[i].note_held) continue;
        if (bestIdle < 0 || _state[i].start_time < bestIdleTime) {
            bestIdle     = i;
            bestIdleTime = _state[i].start_time;
        }
    }
    if (bestIdle >= 0) return bestIdle;

    // Pass 2: every voice is held. Steal the LRU one.
    int      bestHeld     = 0;
    uint32_t bestHeldTime = _state[0].start_time;
    for (int i = 1; i < _voiceCount; ++i) {
        if (_state[i].start_time < bestHeldTime) {
            bestHeld     = i;
            bestHeldTime = _state[i].start_time;
        }
    }
    return bestHeld;
}

int MpeVaSink::findActive(uint8_t ch, uint8_t note) {
    for (int i = 0; i < _voiceCount; ++i) {
        const Voice &v = _state[i];
        if (v.note_held && v.channel == ch && v.note == note) return i;
    }
    return -1;
}

void MpeVaSink::applyFrequency(int vi) {
    const Voice &v = _state[vi];
    // Bend + LFO both add into semitone offset. When LFO is off
    // (_lfoPitchSemi == 0) this reduces to the Phase 2d behavior.
    const float totalSemi = v.bend_semi + _lfoPitchSemi;
    const float hz = noteToHz(v.note) * powf(2.0f, totalSemi / 12.0f);
    if (_voices[vi].osc) _voices[vi].osc->frequency(hz);
}

void MpeVaSink::applyAmplitude(int vi) {
    // Pressure maps linearly onto the range [0.5 × base, 1.0 × base].
    // That matches the spec: full release = -6 dB relative to unity
    // pressure. base_amp already folds velocity × _volumeScale.
    // LFO tremolo multiplies the result; when off _lfoAmpMult == 1.
    const Voice &v = _state[vi];
    const float amp = v.base_amp
                    * (0.5f + 0.5f * v.pressure)
                    * _lfoAmpMult;
    if (_voices[vi].osc) _voices[vi].osc->amplitude(amp);
}

void MpeVaSink::applyFilter(int vi) {
    if (!_voices[vi].filter) return;
    // Cutoff = baseCutoff × 2^(timbre × 2 - 1 + lfo_octaves). Timbre
    // 0 → ×0.5 (−1 octave), timbre 1 → ×2 (+1 octave), timbre 0.5 →
    // ×1 (baseCutoff). LFO adds/subtracts additional octaves on top.
    const float t = _state[vi].timbre;
    const float exp2Arg = (t * 2.0f - 1.0f) + _lfoCutoffOct;
    float hz = _filterCutoffHz * powf(2.0f, exp2Arg);
    if (hz < 20.0f)    hz = 20.0f;
    if (hz > 20000.0f) hz = 20000.0f;
    _voices[vi].filter->frequency(hz);
}

float MpeVaSink::noteToHz(uint8_t note) {
    // Standard 12-TET: A4 = note 69 = 440 Hz.
    return 440.0f * powf(2.0f, ((float)note - 69.0f) / 12.0f);
}

int MpeVaSink::voiceSnapshot(VoiceSnapshot *out, int maxVoices) const {
    if (!out || maxVoices <= 0) return 0;
    const int n = (maxVoices < _voiceCount) ? maxVoices : _voiceCount;
    for (int i = 0; i < n; ++i) {
        const Voice &v = _state[i];
        out[i].held      = v.note_held;
        out[i].channel   = v.channel;
        out[i].note      = v.note;
        out[i].pitchSemi = v.bend_semi + _lfoPitchSemi;
        out[i].pressure  = v.pressure;
        out[i].timbre    = v.timbre;
    }
    return n;
}

short MpeVaSink::toneTypeFor(uint8_t wave) {
    // OSC-level codes (spec): 0=saw, 1=square, 2=triangle, 3=sine.
    // Teensy Audio WAVEFORM_* constants differ, so map explicitly.
    switch (wave) {
        case 0: return WAVEFORM_SAWTOOTH;
        case 1: return WAVEFORM_SQUARE;
        case 2: return WAVEFORM_TRIANGLE;
        case 3: return WAVEFORM_SINE;
        default: return WAVEFORM_SAWTOOTH;
    }
}
