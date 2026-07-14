// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.

#include "DaisyVaSink.h"

#include <math.h>

#include "AudioSynthDaisyVa.h"

// Preset bank — the picker's "instruments". Character = waveform + detune +
// filter + ADSR; per-note MPE modulates pitch/cutoff/level on top.
static const DaisyVaSink::Preset kPresets[] = {
    // name             wave                    detune  cutoff  res    A      D     S     R
    { "Saw Lead",       AudioSynthDaisyVa::WaveSaw,     7.0f,  6000.f, 0.25f, 0.005f,0.10f,0.80f,0.20f },
    { "Detuned Saws",   AudioSynthDaisyVa::WaveSaw,    18.0f,  4500.f, 0.30f, 0.010f,0.20f,0.85f,0.35f },
    { "Square Reed",    AudioSynthDaisyVa::WaveSquare,  4.0f,  3200.f, 0.35f, 0.008f,0.15f,0.75f,0.25f },
    { "Soft Triangle",  AudioSynthDaisyVa::WaveTri,     3.0f,  8000.f, 0.10f, 0.020f,0.30f,0.90f,0.40f },
    { "Acid Bass",      AudioSynthDaisyVa::WaveSaw,     0.0f,  1200.f, 0.75f, 0.003f,0.18f,0.20f,0.15f },
};
static const int kNumPresets = sizeof(kPresets) / sizeof(kPresets[0]);

static inline float clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

DaisyVaSink::DaisyVaSink(VoicePorts *voices, int voiceCount)
    : _voices(voices) {
    if (voiceCount < 0)          voiceCount = 0;
    if (voiceCount > kMaxVoices) voiceCount = kMaxVoices;
    _voiceCount = voiceCount;
    for (int i = 0; i < 16; ++i) _channelTimbre[i] = 0.5f;
    // Do NOT touch the engines here (runs before AudioSynthDaisyVa::begin()).
}

void DaisyVaSink::setMasterChannel(uint8_t ch) {
    if (ch > 16) ch = 0;
    _masterChannel = ch;
}

int         DaisyVaSink::numPresets()          { return kNumPresets; }
const char *DaisyVaSink::presetName(int idx)   { return (idx >= 0 && idx < kNumPresets) ? kPresets[idx].name : ""; }

void DaisyVaSink::setPreset(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= kNumPresets) idx = kNumPresets - 1;
    _preset = idx;
    const Preset &p = kPresets[idx];
    _cutoffBaseHz = p.cutoffHz;
    for (int i = 0; i < _voiceCount; ++i) {
        AudioSynthDaisyVa *e = _voices[i].engine;
        if (!e) continue;
        e->setWaveform(p.wave);
        e->setDetuneCents(p.detuneCents);
        e->setResonance(p.resonance);
        e->setAttack(p.attack);
        e->setDecay(p.decay);
        e->setSustain(p.sustain);
        e->setRelease(p.release);
        e->setCutoffHz(p.cutoffHz);
    }
}

// --- MidiSink ------------------------------------------------------------

void DaisyVaSink::onNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
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
    v.base_level = (velocity <= 0 ? 0.0f : (float)velocity / 127.0f);
    v.cutoff     = _channelTimbre[channel - 1];
    v.pressure   = 0.0f;

    if (!_voices[vi].engine) return;
    applyPitch (vi);
    applyCutoff(vi);
    applyLevel (vi);
    _voices[vi].engine->noteOn();
}

void DaisyVaSink::onNoteOff(uint8_t channel, uint8_t note, uint8_t /*velocity*/) {
    const int vi = findActive(channel, note);
    if (vi < 0) return;
    _state[vi].note_held = false;
    if (_voices[vi].engine) _voices[vi].engine->noteOff();
}

void DaisyVaSink::onPitchBend(uint8_t channel, float semitones) {
    for (int i = 0; i < _voiceCount; ++i) {
        Voice &v = _state[i];
        if (!v.note_held || v.channel != channel) continue;
        v.bend_semi = semitones;
        applyPitch(i);
    }
}

void DaisyVaSink::onPressure(uint8_t channel, float value) {
    value = clamp01(value);
    for (int i = 0; i < _voiceCount; ++i) {
        Voice &v = _state[i];
        if (!v.note_held || v.channel != channel) continue;
        v.pressure = value;
        applyLevel(i);
    }
}

void DaisyVaSink::onTimbre(uint8_t channel, float value) {
    if (channel < 1 || channel > 16) return;
    value = clamp01(value);
    _channelTimbre[channel - 1] = value;
    for (int i = 0; i < _voiceCount; ++i) {
        Voice &v = _state[i];
        if (!v.note_held || v.channel != channel) continue;
        v.cutoff = value;
        applyCutoff(i);
    }
}

void DaisyVaSink::onAllNotesOff(uint8_t channel) {
    for (int i = 0; i < _voiceCount; ++i) {
        Voice &v = _state[i];
        if (channel != 0 && v.channel != channel) continue;
        v.note_held = false;
        if (_voices[i].engine) _voices[i].engine->noteOff();
    }
}

// --- Private -------------------------------------------------------------

int DaisyVaSink::pickVoice() {
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

int DaisyVaSink::findActive(uint8_t ch, uint8_t note) {
    for (int i = 0; i < _voiceCount; ++i) {
        const Voice &v = _state[i];
        if (v.note_held && v.channel == ch && v.note == note) return i;
    }
    return -1;
}

void DaisyVaSink::applyPitch(int vi) {
    const Voice &v = _state[vi];
    _voices[vi].engine->setFreqHz(noteToHz((float)v.note + v.bend_semi));
}

void DaisyVaSink::applyLevel(int vi) {
    const Voice &v = _state[vi];
    const float level = v.base_level * (0.5f + 0.5f * v.pressure);
    _voices[vi].engine->setLevel(level);
}

void DaisyVaSink::applyCutoff(int vi) {
    if (!_voices[vi].engine) return;
    // CC#74 moves cutoff ± _cutoffDepthOct octaves around the preset base.
    const float oct = (_state[vi].cutoff - 0.5f) * 2.0f * _cutoffDepthOct;
    _voices[vi].engine->setCutoffHz(_cutoffBaseHz * powf(2.0f, oct));
}

float DaisyVaSink::noteToHz(float note) {
    return 440.0f * powf(2.0f, (note - 69.0f) / 12.0f);
}
