// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.

#include "AudioSynthDaisyVa.h"

#include <math.h>
#include <string.h>

#ifndef AUDIO_SAMPLE_RATE_EXACT
#define AUDIO_SAMPLE_RATE_EXACT 44100.0f
#endif

using daisysp::Oscillator;

AudioSynthDaisyVa::AudioSynthDaisyVa() : AudioStream(0, nullptr) {
    // Empty of DSP work — see begin().
}

void AudioSynthDaisyVa::begin() {
    const float sr = AUDIO_SAMPLE_RATE_EXACT;
    _osc1.Init(sr);
    _osc2.Init(sr);
    _filter.Init(sr);
    _env.Init(sr, 1);   // per-sample processing (block size 1)

    _osc1.SetWaveform(toDaisyWave(_wave));
    _osc2.SetWaveform(toDaisyWave(_wave));
    _osc1.SetAmp(1.0f);
    _osc2.SetAmp(1.0f);
    _filter.SetRes(_res);
    _filter.SetFreq(_cutoffHz);
    _env.SetAttackTime(0.005f);
    _env.SetDecayTime(0.12f);
    _env.SetSustainLevel(0.8f);
    _env.SetReleaseTime(0.25f);
    setFreqHz(_baseHz);
}

void AudioSynthDaisyVa::setWaveform(uint8_t wave) {
    if (wave >= kNumWaves) wave = WaveSaw;
    _wave = wave;
    _osc1.SetWaveform(toDaisyWave(_wave));
    _osc2.SetWaveform(toDaisyWave(_wave));
}

void AudioSynthDaisyVa::setFreqHz(float hz) {
    if (hz < 1.0f) hz = 1.0f;
    _baseHz = hz;
    applyDetune();
}

void AudioSynthDaisyVa::setDetuneCents(float cents) {
    _detune = cents;
    applyDetune();
}

void AudioSynthDaisyVa::applyDetune() {
    _osc1.SetFreq(_baseHz);
    _osc2.SetFreq(_baseHz * powf(2.0f, _detune / 1200.0f));
}

void AudioSynthDaisyVa::setCutoffHz(float hz) {
    if (hz < 20.0f)    hz = 20.0f;
    if (hz > 18000.0f) hz = 18000.0f;
    _cutoffHz = hz;
    _filter.SetFreq(hz);
}

void AudioSynthDaisyVa::setResonance(float r) {
    if (r < 0.0f) r = 0.0f;
    if (r > 1.0f) r = 1.0f;
    _res = r;
    _filter.SetRes(r);
}

void AudioSynthDaisyVa::setAttack(float s)       { _env.SetAttackTime(s); }
void AudioSynthDaisyVa::setDecay(float s)        { _env.SetDecayTime(s); }
void AudioSynthDaisyVa::setSustain(float level)  { _env.SetSustainLevel(level); }
void AudioSynthDaisyVa::setRelease(float s)      { _env.SetReleaseTime(s); }
void AudioSynthDaisyVa::setLevel(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    _level = v;
}

void AudioSynthDaisyVa::noteOn() {
    _gate         = true;
    _active       = true;
    _silentBlocks = 0;
}
void AudioSynthDaisyVa::noteOff() {
    _gate = false;   // env enters release; idle gate deactivates when silent
}

const char *AudioSynthDaisyVa::waveName(uint8_t wave) {
    switch (wave) {
        case WaveSaw:    return "Saw";
        case WaveSquare: return "Square";
        case WaveTri:    return "Triangle";
        default:         return "";
    }
}

uint8_t AudioSynthDaisyVa::toDaisyWave(uint8_t wave) {
    switch (wave) {
        case WaveSquare: return Oscillator::WAVE_POLYBLEP_SQUARE;
        case WaveTri:    return Oscillator::WAVE_POLYBLEP_TRI;
        case WaveSaw:
        default:         return Oscillator::WAVE_POLYBLEP_SAW;
    }
}

void AudioSynthDaisyVa::update(void) {
    // Idle gate: silent + released -> emit zeros without running the DSP.
    if (!_active && !_gate) {
        audio_block_t *z = allocate();
        if (!z) return;
        memset(z->data, 0, sizeof(z->data));
        transmit(z, 0);
        release(z);
        return;
    }

    audio_block_t *out = allocate();
    if (!out) return;

    float blockPeak = 0.0f;
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        float o  = 0.5f * (_osc1.Process() + _osc2.Process());
        float f  = _filter.Process(o);
        float e  = _env.Process(_gate);
        float s  = f * e * _level;
        float a  = fabsf(e);
        if (a > blockPeak) blockPeak = a;
        int32_t v = (int32_t)(s * 32767.0f);
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        out->data[i] = (int16_t)v;
    }

    // Once the gate is off and the envelope has decayed to ~0, deactivate.
    if (!_gate && blockPeak < 1.0e-4f) {
        if (_silentBlocks < 0xFFFF) ++_silentBlocks;
        if (_silentBlocks > 4) _active = false;    // env already at 0; deactivate promptly
    } else {
        _silentBlocks = 0;
    }

    transmit(out, 0);
    release(out);
}
