// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.

#include "ChipSink.h"

#include <math.h>
#include <stdlib.h>

#include <Audio.h>

ChipSink::ChipSink(const VoicePorts &ports) : _ports(ports) {
    if (_ports.pulse1) {
        _ports.pulse1->begin(WAVEFORM_PULSE);
        _ports.pulse1->amplitude(0.0f);
        _ports.pulse1->frequency(440.0f);
        _ports.pulse1->pulseWidth(pulseWidthForDuty(_pulse1Duty));
    }
    if (_ports.pulse2) {
        _ports.pulse2->begin(WAVEFORM_PULSE);
        _ports.pulse2->amplitude(0.0f);
        _ports.pulse2->frequency(440.0f);
        _ports.pulse2->pulseWidth(pulseWidthForDuty(_pulse2Duty));
    }
    if (_ports.triangle) {
        _ports.triangle->begin(WAVEFORM_TRIANGLE);
        _ports.triangle->amplitude(0.0f);
        _ports.triangle->frequency(220.0f);
    }
    if (_ports.noise) {
        _ports.noise->amplitude(0.0f);
    }
    if (_ports.mix) {
        _ports.mix->gain(0, 1.0f); _ports.mix->gain(1, 1.0f);
        _ports.mix->gain(2, 1.0f); _ports.mix->gain(3, 1.0f);
    }
    if (_ports.env) {
        _ports.env->attack (_attackSec  * 1000.0f);
        _ports.env->decay  (_decaySec   * 1000.0f);
        _ports.env->sustain(_sustain);
        _ports.env->release(_releaseSec * 1000.0f);
    }
}

void ChipSink::setMidiChannel(uint8_t ch) { if (ch > 16) ch = 0; _midiCh = ch; }

void ChipSink::setPulse1Duty(uint8_t d) {
    if (d > 3) d = 0;
    _pulse1Duty = d;
    if (_ports.pulse1) _ports.pulse1->pulseWidth(pulseWidthForDuty(d));
}
void ChipSink::setPulse2Duty(uint8_t d) {
    if (d > 3) d = 0;
    _pulse2Duty = d;
    if (_ports.pulse2) _ports.pulse2->pulseWidth(pulseWidthForDuty(d));
}
void ChipSink::setPulse2Detune(float cents) {
    if (cents < -100.0f) cents = -100.0f;
    if (cents >  100.0f) cents =  100.0f;
    _pulse2Detune = cents;
    if (_held) applyFrequencies();
}

void ChipSink::setTriangleLevel(float v) {
    if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f;
    _triLevel = v;
    if (_held) applyLevels();
}
void ChipSink::setNoiseLevel(float v) {
    if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f;
    _noiseLevel = v;
    if (_held) applyLevels();
}

void ChipSink::setVoicing(uint8_t v) {
    if (v > 3) v = 0;
    _voicing = v;
    if (_held) applyFrequencies();
}
void ChipSink::setArpeggio(uint8_t mode) {
    if (mode > 3) mode = 0;
    _arpMode = mode;
    _arpStep = 0;
}
void ChipSink::setArpRate(float hz) {
    if (hz < 0.0f)  hz = 0.0f;
    if (hz > 40.0f) hz = 40.0f;
    _arpRateHz = hz;
}

void ChipSink::setAttack (float s) { if (s < 0.0001f) s = 0.0001f; _attackSec = s;
    if (_ports.env) _ports.env->attack(s * 1000.0f); }
void ChipSink::setDecay  (float s) { if (s < 0.001f)  s = 0.001f;  _decaySec = s;
    if (_ports.env) _ports.env->decay(s * 1000.0f); }
void ChipSink::setSustain(float v) { if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f; _sustain = v;
    if (_ports.env) _ports.env->sustain(v); }
void ChipSink::setRelease(float s) { if (s < 0.001f) s = 0.001f; _releaseSec = s;
    if (_ports.env) _ports.env->release(s * 1000.0f); }

void ChipSink::setVoiceVolumeScale(float s) {
    if (s < 0.0f) s = 0.0f; if (s > 1.0f) s = 1.0f;
    _voiceVolumeScale = s;
    if (_held) applyLevels();
}

// ---------------------------------------------------------------------

void ChipSink::tick(uint32_t nowMs) {
    if (!_held || _arpMode == 0 || _arpRateHz <= 0.0f) return;
    const uint32_t stepMs = (uint32_t)(1000.0f / _arpRateHz);
    if (nowMs - _lastArpMs < stepMs) return;
    _lastArpMs = nowMs;
    switch (_arpMode) {
        case 1: _arpStep = (_arpStep + 1) % 3;               break; // up
        case 2: _arpStep = (_arpStep + 2) % 3;               break; // down
        case 3: _arpStep = (uint8_t)(rand() % 3);            break; // random
        default: return;
    }
    applyTriangleArpFreq();
}

// ---------------------------------------------------------------------

void ChipSink::onNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (!channelMatches(channel)) return;
    if (velocity == 0) { onNoteOff(channel, note, 0); return; }
    pushNote(note);
    _targetNote = note;
    _velocity   = (float)velocity / 127.0f;
    _held       = true;
    _arpStep    = 0;

    applyFrequencies();
    applyLevels();
    if (_ports.env) _ports.env->noteOn();
}

void ChipSink::onNoteOff(uint8_t channel, uint8_t note, uint8_t) {
    if (!channelMatches(channel)) return;
    popNote(note);
    uint8_t top;
    if (topNote(&top)) {
        _targetNote = top;
        applyFrequencies();
    } else {
        _held = false;
        if (_ports.env) _ports.env->noteOff();
    }
}

void ChipSink::onPitchBend(uint8_t channel, float semitones) {
    if (!channelMatches(channel)) return;
    _bend_semi = semitones;
    if (_held) applyFrequencies();
}

void ChipSink::onAllNotesOff(uint8_t channel) {
    if (channel != 0 && !channelMatches(channel)) return;
    _noteStackCount = 0;
    if (_held) { _held = false; if (_ports.env) _ports.env->noteOff(); }
}

// ---------------------------------------------------------------------

void ChipSink::pushNote(uint8_t note) {
    popNote(note);
    if (_noteStackCount >= kStackSize) {
        for (int i = 1; i < kStackSize; ++i) _noteStack[i - 1] = _noteStack[i];
        _noteStackCount = kStackSize - 1;
    }
    _noteStack[_noteStackCount++] = note;
}
void ChipSink::popNote(uint8_t note) {
    for (int i = 0; i < _noteStackCount; ++i) {
        if (_noteStack[i] == note) {
            for (int j = i + 1; j < _noteStackCount; ++j) _noteStack[j - 1] = _noteStack[j];
            --_noteStackCount;
            return;
        }
    }
}
bool ChipSink::topNote(uint8_t *out) const {
    if (_noteStackCount <= 0) return false;
    if (out) *out = _noteStack[_noteStackCount - 1];
    return true;
}

// ---------------------------------------------------------------------

void ChipSink::applyDuties() {
    if (_ports.pulse1) _ports.pulse1->pulseWidth(pulseWidthForDuty(_pulse1Duty));
    if (_ports.pulse2) _ports.pulse2->pulseWidth(pulseWidthForDuty(_pulse2Duty));
}

void ChipSink::applyLevels() {
    // Pulses are the primary voices; triangle and noise layer on top.
    const float v = _velocity * _voiceVolumeScale;
    // Pulses at fixed balanced level; users tweak pulse2 via voicing +
    // detune rather than level.
    if (_ports.pulse1)   _ports.pulse1  ->amplitude(v * 0.5f);
    if (_ports.pulse2)   _ports.pulse2  ->amplitude(v * 0.45f);
    if (_ports.triangle) _ports.triangle->amplitude(v * _triLevel);
    if (_ports.noise)    _ports.noise   ->amplitude(v * _noiseLevel * 0.5f);  // 0.5 headroom
}

void ChipSink::applyFrequencies() {
    const float rootHz = noteToHz(_targetNote) * powf(2.0f, _bend_semi / 12.0f);
    if (_ports.pulse1) _ports.pulse1->frequency(rootHz);

    // Pulse 2 at voicing interval + detune offset.
    const int interval = voicingInterval(_voicing);
    const float p2Hz = rootHz * powf(2.0f, (float)interval / 12.0f)
                              * powf(2.0f, _pulse2Detune / 1200.0f);
    if (_ports.pulse2) _ports.pulse2->frequency(p2Hz);

    // Triangle: -12 semi (sub) when arp is off. Otherwise cycles
    // through arp intervals.
    applyTriangleArpFreq();

    // Noise has no pitch — it's broadband.
}

void ChipSink::applyTriangleArpFreq() {
    if (!_ports.triangle) return;
    const float rootHz = noteToHz(_targetNote) * powf(2.0f, _bend_semi / 12.0f);
    int semi = -12;  // sub when arp off
    if (_arpMode != 0) {
        // Arp pattern: root (0), 5th (+7), octave (+12). Step 0/1/2.
        switch (_arpStep) {
            case 0: semi = 0;   break;
            case 1: semi = 7;   break;
            case 2: semi = 12;  break;
        }
    }
    _ports.triangle->frequency(rootHz * powf(2.0f, (float)semi / 12.0f));
}

float ChipSink::pulseWidthForDuty(uint8_t d) {
    switch (d) {
        case 0: return 0.125f;
        case 1: return 0.25f;
        case 2: return 0.5f;
        case 3: return 0.75f;
        default: return 0.5f;
    }
}

int ChipSink::voicingInterval(uint8_t v) {
    switch (v) {
        case 0: return 0;    // unison
        case 1: return 12;   // octave
        case 2: return 7;    // fifth
        case 3: return 4;    // major third
        default: return 0;
    }
}

float ChipSink::noteToHz(uint8_t note) {
    return 440.0f * powf(2.0f, ((float)note - 69.0f) / 12.0f);
}
