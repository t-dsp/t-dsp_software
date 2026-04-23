// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.

#include "SupersawSink.h"

#include <math.h>

#include <Audio.h>

SupersawSink::SupersawSink(const VoicePorts &ports) : _ports(ports) {
    // All 5 saws begin at 0 amplitude; applyAmplitudes writes velocity-
    // scaled values on note-on.
    AudioSynthWaveform *oscs[5] = { _ports.osc1, _ports.osc2, _ports.osc3, _ports.osc4, _ports.osc5 };
    for (int i = 0; i < 5; ++i) {
        if (oscs[i]) {
            oscs[i]->begin(WAVEFORM_SAWTOOTH);
            oscs[i]->amplitude(0.0f);
            oscs[i]->frequency(220.0f);
        }
    }
    // mixAB sums 4 oscs (center + 3 of the side voices); mixFinal
    // sums mixAB + the 5th osc + applies the mix-center balance.
    // Actually we compose the balance at osc.amplitude() level so
    // both mixers stay at passthrough gains.
    if (_ports.mixAB) {
        _ports.mixAB->gain(0, 1.0f); _ports.mixAB->gain(1, 1.0f);
        _ports.mixAB->gain(2, 1.0f); _ports.mixAB->gain(3, 1.0f);
    }
    if (_ports.mixFinal) {
        _ports.mixFinal->gain(0, 1.0f); _ports.mixFinal->gain(1, 1.0f);
        _ports.mixFinal->gain(2, 0.0f); _ports.mixFinal->gain(3, 0.0f);
    }
    if (_ports.filter) {
        _ports.filter->frequency(_cutoffHz);
        _ports.filter->resonance(_resonance);
    }
    if (_ports.env) {
        _ports.env->attack (_attackSec  * 1000.0f);
        _ports.env->decay  (_decaySec   * 1000.0f);
        _ports.env->sustain(_sustain);
        _ports.env->release(_releaseSec * 1000.0f);
    }
    _currentHz = noteToHz(_targetNote);
    _targetHz  = _currentHz;
}

void SupersawSink::setMidiChannel(uint8_t ch)  { if (ch > 16) ch = 0; _midiCh = ch; }

void SupersawSink::setDetuneCents(float cents) {
    if (cents < 0.0f)   cents = 0.0f;
    if (cents > 100.0f) cents = 100.0f;
    _detuneCents = cents;
    if (_held) applyFrequencies();
}

void SupersawSink::setMixCenter(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    _mixCenter = v;
    if (_held) applyAmplitudes();
}

void SupersawSink::setCutoff(float hz) {
    if (hz < 20.0f)    hz = 20.0f;
    if (hz > 20000.0f) hz = 20000.0f;
    _cutoffHz = hz;
    if (_ports.filter) _ports.filter->frequency(hz);
}

void SupersawSink::setResonance(float q) {
    if (q < 0.707f) q = 0.707f;
    if (q > 5.0f)   q = 5.0f;
    _resonance = q;
    if (_ports.filter) _ports.filter->resonance(q);
}

void SupersawSink::setAttack (float s) { if (s < 0.0001f) s = 0.0001f; _attackSec = s;
    if (_ports.env) _ports.env->attack(s * 1000.0f); }
void SupersawSink::setDecay  (float s) { if (s < 0.001f)  s = 0.001f;  _decaySec  = s;
    if (_ports.env) _ports.env->decay (s * 1000.0f); }
void SupersawSink::setSustain(float v) { if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f; _sustain = v;
    if (_ports.env) _ports.env->sustain(v); }
void SupersawSink::setRelease(float s) { if (s < 0.001f) s = 0.001f; _releaseSec = s;
    if (_ports.env) _ports.env->release(s * 1000.0f); }

void SupersawSink::setPortamentoMs(float ms) {
    if (ms < 0.0f) ms = 0.0f; if (ms > 1000.0f) ms = 1000.0f;
    _portMs = ms;
}

void SupersawSink::setVoiceVolumeScale(float s) {
    if (s < 0.0f) s = 0.0f; if (s > 1.0f) s = 1.0f;
    _voiceVolumeScale = s;
    if (_held) applyAmplitudes();
}

void SupersawSink::tick(uint32_t nowMs) {
    const uint32_t dtMs = (_lastTickMs == 0) ? 0 : (nowMs - _lastTickMs);
    _lastTickMs = nowMs;
    if (_currentHz == _targetHz) return;

    if (_portMs <= 0.0f || dtMs == 0) {
        _currentHz = _targetHz;
        applyFrequencies();
        return;
    }
    const float logNow = log2f(_currentHz);
    const float logTgt = log2f(_targetHz);
    const float diff = logTgt - logNow;
    const float maxStep = (float)dtMs / _portMs;
    float step;
    if (diff >= 0.0f) step = (diff <= maxStep) ? diff : maxStep;
    else              step = (-diff <= maxStep) ? diff : -maxStep;
    _currentHz = powf(2.0f, logNow + step);
    applyFrequencies();
}

// ---------------------------------------------------------------------

void SupersawSink::onNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (!channelMatches(channel)) return;
    if (velocity == 0) { onNoteOff(channel, note, 0); return; }

    const bool wasHeld = _held;
    pushNote(note);
    _targetNote = note;
    _velocity   = (float)velocity / 127.0f;
    _targetHz   = noteToHz(note) * powf(2.0f, _bend_semi / 12.0f);
    if (!wasHeld || _portMs <= 0.0f) _currentHz = _targetHz;
    _held = true;

    applyFrequencies();
    applyAmplitudes();
    applyFilter();
    if (_ports.env) _ports.env->noteOn();
}

void SupersawSink::onNoteOff(uint8_t channel, uint8_t note, uint8_t) {
    if (!channelMatches(channel)) return;
    popNote(note);

    uint8_t top;
    if (topNote(&top)) {
        _targetNote = top;
        _targetHz   = noteToHz(top) * powf(2.0f, _bend_semi / 12.0f);
        // Legato: glide to the fallback note without retriggering envelope.
    } else {
        _held = false;
        if (_ports.env) _ports.env->noteOff();
    }
}

void SupersawSink::onPitchBend(uint8_t channel, float semitones) {
    if (!channelMatches(channel)) return;
    _bend_semi = semitones;
    if (_held) {
        _targetHz = noteToHz(_targetNote) * powf(2.0f, semitones / 12.0f);
        if (_portMs <= 0.0f) _currentHz = _targetHz;
        applyFrequencies();
    }
}

void SupersawSink::onAllNotesOff(uint8_t channel) {
    if (channel != 0 && !channelMatches(channel)) return;
    _noteStackCount = 0;
    if (_held) { _held = false; if (_ports.env) _ports.env->noteOff(); }
}

// ---------------------------------------------------------------------

void SupersawSink::pushNote(uint8_t note) {
    popNote(note);
    if (_noteStackCount >= kStackSize) {
        for (int i = 1; i < kStackSize; ++i) _noteStack[i - 1] = _noteStack[i];
        _noteStackCount = kStackSize - 1;
    }
    _noteStack[_noteStackCount++] = note;
}
void SupersawSink::popNote(uint8_t note) {
    for (int i = 0; i < _noteStackCount; ++i) {
        if (_noteStack[i] == note) {
            for (int j = i + 1; j < _noteStackCount; ++j) _noteStack[j - 1] = _noteStack[j];
            --_noteStackCount;
            return;
        }
    }
}
bool SupersawSink::topNote(uint8_t *out) const {
    if (_noteStackCount <= 0) return false;
    if (out) *out = _noteStack[_noteStackCount - 1];
    return true;
}

// ---------------------------------------------------------------------

void SupersawSink::applyFrequencies() {
    // 5 saws: center at _currentHz; sides at ±detune, ±2×detune cents.
    const float d  = _detuneCents;
    const float r1up = powf(2.0f,   d / 1200.0f);
    const float r1dn = powf(2.0f,  -d / 1200.0f);
    const float r2up = powf(2.0f,  2.0f * d / 1200.0f);
    const float r2dn = powf(2.0f, -2.0f * d / 1200.0f);
    if (_ports.osc1) _ports.osc1->frequency(_currentHz);
    if (_ports.osc2) _ports.osc2->frequency(_currentHz * r1dn);
    if (_ports.osc3) _ports.osc3->frequency(_currentHz * r1up);
    if (_ports.osc4) _ports.osc4->frequency(_currentHz * r2dn);
    if (_ports.osc5) _ports.osc5->frequency(_currentHz * r2up);
}

void SupersawSink::applyAmplitudes() {
    // Mix-center balance: at _mixCenter = 1, only osc1 sounds. At
    // _mixCenter = 0, only the four side saws sound. Linear crossfade
    // in power (sqrt) for equal loudness.
    const float cw = sqrtf(_mixCenter);
    const float sw = sqrtf(1.0f - _mixCenter) * 0.5f;  // 4 sides × 0.5 ≈ 2.0 peak
    const float v  = _velocity * _voiceVolumeScale;
    if (_ports.osc1) _ports.osc1->amplitude(v * 0.35f * cw);
    if (_ports.osc2) _ports.osc2->amplitude(v * 0.35f * sw);
    if (_ports.osc3) _ports.osc3->amplitude(v * 0.35f * sw);
    if (_ports.osc4) _ports.osc4->amplitude(v * 0.35f * sw);
    if (_ports.osc5) _ports.osc5->amplitude(v * 0.35f * sw);
}

void SupersawSink::applyMixBalance() {
    // Unused — balance lives in osc amplitudes for simplicity.
}

void SupersawSink::applyFilter() {
    if (_ports.filter) _ports.filter->frequency(_cutoffHz);
}

float SupersawSink::noteToHz(uint8_t note) {
    return 440.0f * powf(2.0f, ((float)note - 69.0f) / 12.0f);
}
