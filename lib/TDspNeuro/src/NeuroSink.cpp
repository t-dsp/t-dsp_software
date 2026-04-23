// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.

#include "NeuroSink.h"

#include <math.h>

#include <Audio.h>

NeuroSink::NeuroSink(const VoicePorts &ports)
    : _ports(ports)
{
    // Seed oscillators. osc1/2/3 = saw, oscSub = sine. All at zero
    // amplitude until a note-on brings velocity in. Frequencies are
    // updated on note-on and in tick() for portamento/LFO.
    if (_ports.osc1)   { _ports.osc1  ->begin(WAVEFORM_SAWTOOTH); _ports.osc1  ->amplitude(0.0f); _ports.osc1  ->frequency(220.0f); }
    if (_ports.osc2)   { _ports.osc2  ->begin(WAVEFORM_SAWTOOTH); _ports.osc2  ->amplitude(0.0f); _ports.osc2  ->frequency(220.0f); }
    if (_ports.osc3)   { _ports.osc3  ->begin(WAVEFORM_SAWTOOTH); _ports.osc3  ->amplitude(0.0f); _ports.osc3  ->frequency(220.0f); }
    if (_ports.oscSub) { _ports.oscSub->begin(WAVEFORM_SINE);     _ports.oscSub->amplitude(0.0f); _ports.oscSub->frequency(110.0f); }

    // Voice-mixer slots at unity. Balance is controlled on the osc
    // amplitudes themselves (see applyOscAmplitudes).
    if (_ports.voiceMix) {
        for (int i = 0; i < 4; ++i) _ports.voiceMix->gain(i, 1.0f);
    }

    if (_ports.filter) {
        _ports.filter->frequency(_filterCutoffHz);
        _ports.filter->resonance(_filterResonance);
    }
    if (_ports.env) {
        _ports.env->attack (_attackSec  * 1000.0f);
        _ports.env->release(_releaseSec * 1000.0f);
    }

    _currentHz = noteToHz(_targetNote);
    _targetHz  = _currentHz;
}

void NeuroSink::setMidiChannel(uint8_t ch) {
    if (ch > 16) ch = 0;
    _midiCh = ch;
}

void NeuroSink::setDetuneCents(float cents) {
    if (cents < 0.0f)  cents = 0.0f;
    if (cents > 50.0f) cents = 50.0f;
    _detuneCents = cents;
    if (_held) applyFrequenciesFromTarget();
}

void NeuroSink::setSubLevel(float level) {
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    _subLevel = level;
    if (_held) applyOscAmplitudes();
}

void NeuroSink::setOsc3Level(float level) {
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    _osc3Level = level;
    if (_held) applyOscAmplitudes();
}

void NeuroSink::setAttack(float seconds) {
    if (seconds < 0.0f) seconds = 0.0f;
    _attackSec = seconds;
    if (_ports.env) _ports.env->attack(seconds * 1000.0f);
}

void NeuroSink::setRelease(float seconds) {
    if (seconds < 0.0f) seconds = 0.0f;
    _releaseSec = seconds;
    if (_ports.env) _ports.env->release(seconds * 1000.0f);
}

void NeuroSink::setFilterCutoff(float hz) {
    if (hz < 20.0f)    hz = 20.0f;
    if (hz > 20000.0f) hz = 20000.0f;
    _filterCutoffHz = hz;
    applyFilter();
}

void NeuroSink::setFilterResonance(float q) {
    if (q < 0.707f) q = 0.707f;
    if (q > 5.0f)   q = 5.0f;
    _filterResonance = q;
    if (_ports.filter) _ports.filter->resonance(q);
}

void NeuroSink::setVoiceVolumeScale(float s) {
    if (s < 0.0f) s = 0.0f;
    if (s > 1.0f) s = 1.0f;
    _voiceVolumeScale = s;
    if (_held) applyOscAmplitudes();
}

// --- LFO -----------------------------------------------------------

void NeuroSink::setLfoRate(float hz) {
    if (hz < 0.0f)  hz = 0.0f;
    if (hz > 20.0f) hz = 20.0f;
    _lfoRateHz = hz;
}

void NeuroSink::setLfoDepth(float depth) {
    if (depth < 0.0f) depth = 0.0f;
    if (depth > 1.0f) depth = 1.0f;
    _lfoDepth = depth;
}

void NeuroSink::setLfoDest(uint8_t dest) {
    if (dest > LfoAmp) dest = LfoOff;
    if (_lfoDest != dest) {
        _lfoPitchSemi = 0.0f;
        _lfoCutoffOct = 0.0f;
        _lfoAmpMult   = 1.0f;
        _lfoDest      = dest;
        if (_held) {
            applyFrequenciesFromTarget();
            applyOscAmplitudes();
            applyFilter();
        }
    }
}

void NeuroSink::setLfoWaveform(uint8_t wave) {
    if (wave > 3) wave = 0;
    _lfoWaveform = wave;
}

static float neuroLfoShape(uint8_t wave, float phase01) {
    static constexpr float kTwoPi = 6.2831853071795864f;
    switch (wave) {
        case 0:  return sinf(phase01 * kTwoPi);
        case 1:  return (phase01 < 0.5f) ? (4.0f * phase01 - 1.0f)
                                         : (3.0f - 4.0f * phase01);
        case 2:  return 2.0f * phase01 - 1.0f;
        case 3:  return (phase01 < 0.5f) ? 1.0f : -1.0f;
        default: return 0.0f;
    }
}

void NeuroSink::setPortamentoMs(float ms) {
    if (ms < 0.0f)    ms = 0.0f;
    if (ms > 2000.0f) ms = 2000.0f;
    _portMs = ms;
}

void NeuroSink::tick(uint32_t nowMs) {
    const uint32_t dtMs = (_lastTickMs == 0) ? 0 : (nowMs - _lastTickMs);
    _lastTickMs = nowMs;

    // --- LFO phase + contribution ---
    bool lfoActive = (_lfoDest != LfoOff) && (_lfoDepth > 0.0f) && (_lfoRateHz > 0.0f);
    if (lfoActive) {
        const float periodMs = 1000.0f / _lfoRateHz;
        const float phase01  = fmodf((float)nowMs, periodMs) / periodMs;
        const float shape    = neuroLfoShape(_lfoWaveform, phase01);
        const float scaled   = shape * _lfoDepth;

        _lfoPitchSemi = 0.0f;
        _lfoCutoffOct = 0.0f;
        _lfoAmpMult   = 1.0f;
        switch (_lfoDest) {
            case LfoPitch:
                _lfoPitchSemi = scaled * 12.0f;
                break;
            case LfoCutoff:
                _lfoCutoffOct = scaled;
                break;
            case LfoAmp: {
                const float unipolar = (shape + 1.0f) * 0.5f;
                _lfoAmpMult = 1.0f - _lfoDepth * (1.0f - unipolar);
                break;
            }
            default:
                break;
        }
    }

    // Push LFO + portamento into the audio graph. When nothing's held,
    // skip the writes — osc amps are already at 0 so there's no audible
    // change to apply.
    if (!_held) return;

    if (lfoActive) {
        switch (_lfoDest) {
            case LfoPitch:  applyFrequenciesFromTarget(); break;
            case LfoCutoff: applyFilter();                break;
            case LfoAmp:    applyOscAmplitudes();         break;
            default:                                       break;
        }
    }

    // --- Portamento glide ---
    //
    // Target frequency includes the current bend + LFO-pitch offset.
    // Compute it fresh each tick so a pitch-LFO that's modulating at
    // the same time as a portamento glide composes correctly.
    const float targetHz = noteToHz(_targetNote)
                         * powf(2.0f, (_bend_semi + _lfoPitchSemi) / 12.0f);
    _targetHz = targetHz;

    if (_portMs <= 0.0f) {
        // No glide; applyFrequenciesFromTarget has already written
        // frequencies from any note-on / bend / LFO-pitch change.
        // Keeping _currentHz == _targetHz avoids a spurious glide if
        // the user engages portamento mid-note.
        _currentHz = _targetHz;
    } else if (dtMs > 0 && _currentHz != _targetHz) {
        // Glide in log-space so an octave takes the same time
        // regardless of register. Step size is (total-log-diff ×
        // dt/portMs); clamp so we never overshoot the target.
        const float logNow  = log2f(_currentHz);
        const float logTgt  = log2f(_targetHz);
        const float diff    = logTgt - logNow;
        const float maxStep = (float)dtMs / _portMs;  // in log-octaves per total-glide
        float step;
        if (diff >= 0.0f) step = (diff <= maxStep) ? diff : maxStep;
        else              step = (-diff <= maxStep) ? diff : -maxStep;
        _currentHz = powf(2.0f, logNow + step);

        // Write the glided frequency. Detune ratio reused below.
        const float halfCents = _detuneCents * 0.5f;
        const float upRatio   = powf(2.0f,  halfCents / 1200.0f);
        const float dnRatio   = powf(2.0f, -halfCents / 1200.0f);
        if (_ports.osc1)   _ports.osc1  ->frequency(_currentHz * upRatio);
        if (_ports.osc2)   _ports.osc2  ->frequency(_currentHz * dnRatio);
        if (_ports.osc3)   _ports.osc3  ->frequency(_currentHz);
        if (_ports.oscSub) _ports.oscSub->frequency(_currentHz * 0.5f);  // -12 semi
    }
}

// ---------------------------------------------------------------------
// MidiSink overrides
// ---------------------------------------------------------------------

void NeuroSink::onNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (!channelMatches(channel)) return;
    if (velocity == 0) { onNoteOff(channel, note, 0); return; }

    pushNote(note);
    const bool wasHeld = _held;
    _targetNote = note;
    _velocity   = (float)velocity / 127.0f;
    _held       = true;

    applyOscAmplitudes();
    applyFrequenciesFromTarget();
    applyFilter();

    // Retrigger the envelope for a new gesture (legato from a still-
    // held chord simply changes pitch without retriggering — see
    // note-off path). Retrigger on EVERY note-on is the safe default
    // for Phase 1; a future "legato on/off" toggle could change this.
    (void)wasHeld;
    if (_ports.env) _ports.env->noteOn();
}

void NeuroSink::onNoteOff(uint8_t channel, uint8_t note, uint8_t /*velocity*/) {
    if (!channelMatches(channel)) return;
    popNote(note);

    uint8_t top;
    if (topNote(&top)) {
        // Legato: fall back to the next-most-recent held note, change
        // target pitch, DO NOT retrigger envelope. Portamento glides
        // from the current frequency to the new target in tick().
        _targetNote = top;
        applyFrequenciesFromTarget();
        // Keep _held true; voice keeps sounding the fallback note.
    } else {
        // Stack empty — release the voice.
        _held = false;
        if (_ports.env) _ports.env->noteOff();
    }
}

void NeuroSink::onPitchBend(uint8_t channel, float semitones) {
    if (!channelMatches(channel)) return;
    _bend_semi = semitones;
    if (_held) applyFrequenciesFromTarget();
}

void NeuroSink::onAllNotesOff(uint8_t channel) {
    // channel == 0 is panic. For other channels, only fire if ours.
    if (channel != 0 && !channelMatches(channel)) return;
    _noteStackCount = 0;
    if (_held) {
        _held = false;
        if (_ports.env) _ports.env->noteOff();
    }
}

// ---------------------------------------------------------------------
// Note stack (mono last-note priority)
// ---------------------------------------------------------------------

void NeuroSink::pushNote(uint8_t note) {
    // If the note is already on the stack (re-entrant note-on from a
    // buggy controller), treat it as a re-retrigger: remove the old
    // entry first so the new push sits on top.
    popNote(note);
    if (_noteStackCount >= kStackSize) {
        // Stack full. Drop the oldest (bottom) to make room — the user
        // is playing more notes than the bass will ever sound anyway,
        // and the most-recent is what matters for mono priority.
        for (int i = 1; i < kStackSize; ++i) _noteStack[i - 1] = _noteStack[i];
        _noteStackCount = kStackSize - 1;
    }
    _noteStack[_noteStackCount++] = note;
}

void NeuroSink::popNote(uint8_t note) {
    for (int i = 0; i < _noteStackCount; ++i) {
        if (_noteStack[i] == note) {
            for (int j = i + 1; j < _noteStackCount; ++j) {
                _noteStack[j - 1] = _noteStack[j];
            }
            --_noteStackCount;
            return;
        }
    }
}

bool NeuroSink::topNote(uint8_t *out) const {
    if (_noteStackCount <= 0) return false;
    if (out) *out = _noteStack[_noteStackCount - 1];
    return true;
}

// ---------------------------------------------------------------------
// Apply helpers
// ---------------------------------------------------------------------

void NeuroSink::applyOscAmplitudes() {
    // Balance layout (each multiplied by velocity × volumeScale × LFO amp):
    //   saw1/saw2 : fixed 0.5 each (detuned pair)
    //   saw3      : _osc3Level * 0.5 (fundamental reinforce)
    //   subSine   : _subLevel
    //
    // Summing 0.5 + 0.5 + 0.35 + 0.6 at their max is 1.95, beyond unity.
    // The voiceMix slots are unity (no headroom reduction there), so
    // we rely on the downstream g_neuroGain being < 1.0 (0.7 default
    // like the other synths) plus the filter typically shaving the
    // saw energy. procLimiter on the main bus catches any remaining
    // peaks. Int16 saturation would be audible as clipping on very
    // loud presets — the whole chain is intentionally hot for reese.
    const float v = _velocity * _voiceVolumeScale * _lfoAmpMult;
    if (_ports.osc1)   _ports.osc1  ->amplitude(v * 0.5f);
    if (_ports.osc2)   _ports.osc2  ->amplitude(v * 0.5f);
    if (_ports.osc3)   _ports.osc3  ->amplitude(v * 0.5f * _osc3Level);
    if (_ports.oscSub) _ports.oscSub->amplitude(v * _subLevel);
}

void NeuroSink::applyFrequenciesFromTarget() {
    // Compose target from note + bend + LFO-pitch. Snap _currentHz
    // when portamento is disabled so tick()'s glide loop sees a
    // consistent state.
    const float targetHz = noteToHz(_targetNote)
                         * powf(2.0f, (_bend_semi + _lfoPitchSemi) / 12.0f);
    _targetHz = targetHz;
    if (_portMs <= 0.0f) _currentHz = targetHz;

    // Always write out _currentHz — if portamento is on, _currentHz
    // hasn't caught up to _targetHz yet and tick() will continue the
    // glide on the next call.
    const float halfCents = _detuneCents * 0.5f;
    const float upRatio   = powf(2.0f,  halfCents / 1200.0f);
    const float dnRatio   = powf(2.0f, -halfCents / 1200.0f);
    if (_ports.osc1)   _ports.osc1  ->frequency(_currentHz * upRatio);
    if (_ports.osc2)   _ports.osc2  ->frequency(_currentHz * dnRatio);
    if (_ports.osc3)   _ports.osc3  ->frequency(_currentHz);
    if (_ports.oscSub) _ports.oscSub->frequency(_currentHz * 0.5f);
}

void NeuroSink::applyFilter() {
    if (!_ports.filter) return;
    float hz = _filterCutoffHz * powf(2.0f, _lfoCutoffOct);
    if (hz < 20.0f)    hz = 20.0f;
    if (hz > 20000.0f) hz = 20000.0f;
    _ports.filter->frequency(hz);
}

float NeuroSink::noteToHz(uint8_t note) {
    return 440.0f * powf(2.0f, ((float)note - 69.0f) / 12.0f);
}
