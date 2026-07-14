// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.

#include "RingsSink.h"

#include <math.h>

#include "AudioSynthRings.h"

static inline float clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

RingsSink::RingsSink(VoicePorts *voices, int voiceCount)
    : _voices(voices) {
    if (voiceCount < 0)          voiceCount = 0;
    if (voiceCount > kMaxVoices) voiceCount = kMaxVoices;
    _voiceCount = voiceCount;

    for (int i = 0; i < 16; ++i) _channelTimbre[i] = 0.5f;
    // Do NOT touch the engines here: this ctor runs during global static
    // construction, before AudioSynthRings::begin() has Init'd the DaisySP
    // voices. The sketch pushes the global macros after begin() (setup()).
}

void RingsSink::setMasterChannel(uint8_t ch) {
    if (ch > 16) ch = 0;
    _masterChannel = ch;
}

// --- Global macros -------------------------------------------------------

void RingsSink::setMode(uint8_t mode) {
    _mode = mode;
    for (int i = 0; i < _voiceCount; ++i)
        if (_voices[i].engine) _voices[i].engine->setMode(mode);
}

void RingsSink::setStructure(float v) {
    _structure = clamp01(v);
    for (int i = 0; i < _voiceCount; ++i)
        if (_voices[i].engine) _voices[i].engine->setStructure(_structure);
}

void RingsSink::setBrightness(float v) {
    _brightness = clamp01(v);
    for (int i = 0; i < _voiceCount; ++i) applyBrightness(i);
}

void RingsSink::setDamping(float v) {
    _damping = clamp01(v);
    for (int i = 0; i < _voiceCount; ++i) applyDamping(i);
}

void RingsSink::setBrightnessDepth(float v) {
    _brightnessDepth = clamp01(v);
    for (int i = 0; i < _voiceCount; ++i) applyBrightness(i);
}

// --- MidiSink ------------------------------------------------------------

void RingsSink::onNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (_masterChannel != 0 && channel == _masterChannel) return;
    if (channel < 1 || channel > 16) return;
    if (_voiceCount <= 0)            return;

    const int vi = pickVoice();
    if (vi < 0) return;

    Voice &v = _state[vi];
    v.channel    = channel;
    v.note       = note;
    v.note_held  = true;
    v.start_time = ++_counter;
    v.bend_semi  = 0.0f;
    v.brightness = _channelTimbre[channel - 1];
    v.pressure   = 0.0f;

    AudioSynthRings *e = _voices[vi].engine;
    if (!e) return;
    e->setMode(_mode);
    e->setStructure(_structure);
    applyFreq(vi);
    applyBrightness(vi);
    applyDamping(vi);
    // Velocity -> excitation accent.
    e->setAccent(velocity <= 0 ? 0.0f : (float)velocity / 127.0f);
    e->strike();
}

void RingsSink::onNoteOff(uint8_t channel, uint8_t note, uint8_t /*velocity*/) {
    const int vi = findActive(channel, note);
    if (vi < 0) return;
    _state[vi].note_held = false;
    // Let the resonator ring out naturally (physical decay per damping); just
    // stop any sustained excitation.
    if (_voices[vi].engine) _voices[vi].engine->silence();
}

void RingsSink::onPitchBend(uint8_t channel, float semitones) {
    for (int i = 0; i < _voiceCount; ++i) {
        Voice &v = _state[i];
        if (!v.note_held || v.channel != channel) continue;
        v.bend_semi = semitones;
        applyFreq(i);
    }
}

void RingsSink::onPressure(uint8_t channel, float value) {
    value = clamp01(value);
    for (int i = 0; i < _voiceCount; ++i) {
        Voice &v = _state[i];
        if (!v.note_held || v.channel != channel) continue;
        v.pressure = value;
        applyDamping(i);
    }
}

void RingsSink::onTimbre(uint8_t channel, float value) {
    if (channel < 1 || channel > 16) return;
    value = clamp01(value);
    _channelTimbre[channel - 1] = value;
    for (int i = 0; i < _voiceCount; ++i) {
        Voice &v = _state[i];
        if (!v.note_held || v.channel != channel) continue;
        v.brightness = value;
        applyBrightness(i);
    }
}

void RingsSink::onAllNotesOff(uint8_t channel) {
    for (int i = 0; i < _voiceCount; ++i) {
        Voice &v = _state[i];
        if (channel != 0 && v.channel != channel) continue;
        v.note_held = false;
        if (_voices[i].engine) _voices[i].engine->silence();
    }
}

// --- Private -------------------------------------------------------------

int RingsSink::pickVoice() {
    if (_voiceCount <= 0) return -1;

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

int RingsSink::findActive(uint8_t ch, uint8_t note) {
    for (int i = 0; i < _voiceCount; ++i) {
        const Voice &v = _state[i];
        if (v.note_held && v.channel == ch && v.note == note) return i;
    }
    return -1;
}

void RingsSink::applyFreq(int vi) {
    const Voice &v = _state[vi];
    _voices[vi].engine->setFreqHz(noteToHz((float)v.note + v.bend_semi));
}

void RingsSink::applyBrightness(int vi) {
    if (!_voices[vi].engine) return;
    // Per-note CC#74 moves BRIGHTNESS ± depth around the global (0.5 = neutral).
    const float b = clamp01(_brightness + (_state[vi].brightness - 0.5f) * _brightnessDepth);
    _voices[vi].engine->setBrightness(b);
}

void RingsSink::applyDamping(int vi) {
    if (!_voices[vi].engine) return;
    // Pressure lengthens the ring: lift damping from the global toward 1.0.
    const float d = clamp01(_damping + (1.0f - _damping) * _state[vi].pressure);
    _voices[vi].engine->setDamping(d);
}

float RingsSink::noteToHz(float note) {
    return 440.0f * powf(2.0f, (note - 69.0f) / 12.0f);
}
