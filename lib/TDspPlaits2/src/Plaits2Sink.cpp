// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.

#include "Plaits2Sink.h"

#include <math.h>

#include "AudioSynthPlaits.h"

static inline float clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

Plaits2Sink::Plaits2Sink(VoicePorts *voices, int voiceCount)
    : _voices(voices) {
    if (voiceCount < 0)          voiceCount = 0;
    if (voiceCount > kMaxVoices) voiceCount = kMaxVoices;
    _voiceCount = voiceCount;

    for (int i = 0; i < 16; ++i) _channelTimbre[i] = 0.5f;

    // Seed every voice into a defined, silent state.
    for (int i = 0; i < _voiceCount; ++i) {
        if (!_voices[i].engine) continue;
        _voices[i].engine->setEngineIndex(_engine);
        _voices[i].engine->setHarmonics(_harmonics);
        _voices[i].engine->setTimbre(_timbre);
        _voices[i].engine->setMorph(_morph);
        _voices[i].engine->setDecay(_decay);
        _voices[i].engine->setLpgColour(_lpgColour);
        _voices[i].engine->setLevel(0.0f);
        _voices[i].engine->gate(false);
    }
}

void Plaits2Sink::setMasterChannel(uint8_t ch) {
    if (ch > 16) ch = 0;
    _masterChannel = ch;
}

// --- Global macros -------------------------------------------------------

void Plaits2Sink::setEngine(uint8_t e) {
    if (e >= AudioSynthPlaits::kNumEngines) e = AudioSynthPlaits::kNumEngines - 1;
    _engine = e;
    for (int i = 0; i < _voiceCount; ++i)
        if (_voices[i].engine) _voices[i].engine->setEngineIndex(_engine);
}

void Plaits2Sink::setHarmonics(float v) {
    _harmonics = clamp01(v);
    for (int i = 0; i < _voiceCount; ++i)
        if (_voices[i].engine) _voices[i].engine->setHarmonics(_harmonics);
}

void Plaits2Sink::setMorph(float v) {
    _morph = clamp01(v);
    for (int i = 0; i < _voiceCount; ++i)
        if (_voices[i].engine) _voices[i].engine->setMorph(_morph);
}

void Plaits2Sink::setDecay(float v) {
    _decay = clamp01(v);
    for (int i = 0; i < _voiceCount; ++i)
        if (_voices[i].engine) _voices[i].engine->setDecay(_decay);
}

void Plaits2Sink::setLpgColour(float v) {
    _lpgColour = clamp01(v);
    for (int i = 0; i < _voiceCount; ++i)
        if (_voices[i].engine) _voices[i].engine->setLpgColour(_lpgColour);
}

void Plaits2Sink::setTimbre(float v) {
    _timbre = clamp01(v);
    // Re-derive per-voice timbre (global base + each voice's CC#74 latch).
    for (int i = 0; i < _voiceCount; ++i) applyTimbre(i);
}

void Plaits2Sink::setTimbreDepth(float v) {
    _timbreDepth = clamp01(v);
    for (int i = 0; i < _voiceCount; ++i) applyTimbre(i);
}

// --- MidiSink ------------------------------------------------------------

void Plaits2Sink::onNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
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
    v.timbre     = _channelTimbre[channel - 1];
    v.pressure   = 0.0f;

    if (!_voices[vi].engine) return;
    pushMacros(vi);
    applyPitch (vi);
    applyTimbre(vi);
    applyLevel (vi);
    _voices[vi].engine->gate(true);
}

void Plaits2Sink::onNoteOff(uint8_t channel, uint8_t note, uint8_t /*velocity*/) {
    const int vi = findActive(channel, note);
    if (vi < 0) return;
    _state[vi].note_held = false;
    if (_voices[vi].engine) {
        _voices[vi].engine->gate(false);
        _voices[vi].engine->setLevel(0.0f);   // LPG release
    }
}

void Plaits2Sink::onPitchBend(uint8_t channel, float semitones) {
    for (int i = 0; i < _voiceCount; ++i) {
        Voice &v = _state[i];
        if (!v.note_held || v.channel != channel) continue;
        v.bend_semi = semitones;
        applyPitch(i);
    }
}

void Plaits2Sink::onPressure(uint8_t channel, float value) {
    value = clamp01(value);
    for (int i = 0; i < _voiceCount; ++i) {
        Voice &v = _state[i];
        if (!v.note_held || v.channel != channel) continue;
        v.pressure = value;
        applyLevel(i);
    }
}

void Plaits2Sink::onTimbre(uint8_t channel, float value) {
    if (channel < 1 || channel > 16) return;
    value = clamp01(value);
    _channelTimbre[channel - 1] = value;
    for (int i = 0; i < _voiceCount; ++i) {
        Voice &v = _state[i];
        if (!v.note_held || v.channel != channel) continue;
        v.timbre = value;
        applyTimbre(i);
    }
}

void Plaits2Sink::onAllNotesOff(uint8_t channel) {
    for (int i = 0; i < _voiceCount; ++i) {
        Voice &v = _state[i];
        if (channel != 0 && v.channel != channel) continue;
        v.note_held = false;
        if (_voices[i].engine) {
            _voices[i].engine->gate(false);
            _voices[i].engine->setLevel(0.0f);
        }
    }
}

// --- Private -------------------------------------------------------------

int Plaits2Sink::pickVoice() {
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

int Plaits2Sink::findActive(uint8_t ch, uint8_t note) {
    for (int i = 0; i < _voiceCount; ++i) {
        const Voice &v = _state[i];
        if (v.note_held && v.channel == ch && v.note == note) return i;
    }
    return -1;
}

void Plaits2Sink::applyPitch(int vi) {
    // Plaits' patch.note is a MIDI note number (float). Add per-note bend.
    const Voice &v = _state[vi];
    _voices[vi].engine->setNote((float)v.note + v.bend_semi);
}

void Plaits2Sink::applyLevel(int vi) {
    // Pressure lifts level from a floor so an un-pressed key is still audible
    // (matches MPE feel): 0.5..1.0 of velocity across the pressure range.
    const Voice &v = _state[vi];
    const float level = v.base_level * (0.5f + 0.5f * v.pressure);
    _voices[vi].engine->setLevel(level);
}

void Plaits2Sink::applyTimbre(int vi) {
    if (!_voices[vi].engine) return;
    // Per-note CC#74 moves TIMBRE ± _timbreDepth around the global (0.5 = neutral).
    const float t = clamp01(_timbre + (_state[vi].timbre - 0.5f) * _timbreDepth);
    _voices[vi].engine->setTimbre(t);
}

void Plaits2Sink::pushMacros(int vi) {
    AudioSynthPlaits *e = _voices[vi].engine;
    e->setEngineIndex(_engine);
    e->setHarmonics(_harmonics);
    e->setMorph(_morph);
    e->setDecay(_decay);
    e->setLpgColour(_lpgColour);
}
