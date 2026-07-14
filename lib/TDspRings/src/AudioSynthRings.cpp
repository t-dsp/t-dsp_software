// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.

#include "AudioSynthRings.h"

#include <math.h>
#include <string.h>

#ifndef AUDIO_SAMPLE_RATE_EXACT
#define AUDIO_SAMPLE_RATE_EXACT 44100.0f
#endif

static inline float clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

AudioSynthRings::AudioSynthRings() : AudioStream(0, nullptr) {
    // Deliberately EMPTY of DaisySP work — see begin(). Constructing this object
    // only registers it in the audio update list; the DSP is initialized later.
}

void AudioSynthRings::begin() {
    const float sr = AUDIO_SAMPLE_RATE_EXACT;
    _modal.Init(sr);
    _string.Init(sr);
    // Sensible defaults — a bright-ish, medium-decay modal bar.
    _modal.SetStructure(0.5f);  _string.SetStructure(0.5f);
    _modal.SetBrightness(0.5f); _string.SetBrightness(0.5f);
    _modal.SetDamping(0.5f);    _string.SetDamping(0.5f);
    _modal.SetAccent(0.5f);     _string.SetAccent(0.5f);
    _modal.SetFreq(220.0f);     _string.SetFreq(220.0f);
    _modal.SetSustain(false);   _string.SetSustain(false);
}

void AudioSynthRings::setMode(uint8_t mode) {
    if (mode >= kNumModes) mode = ModeModal;
    _mode = mode;
}

void AudioSynthRings::setFreqHz(float hz) {
    if (hz < 1.0f) hz = 1.0f;
    _modal.SetFreq(hz);
    _string.SetFreq(hz);
}
void AudioSynthRings::setStructure(float v)  { v = clamp01(v); _modal.SetStructure(v);  _string.SetStructure(v); }
void AudioSynthRings::setBrightness(float v) { v = clamp01(v); _modal.SetBrightness(v); _string.SetBrightness(v); }
void AudioSynthRings::setDamping(float v)    { v = clamp01(v); _modal.SetDamping(v);    _string.SetDamping(v); }
void AudioSynthRings::setAccent(float v)     { v = clamp01(v); _modal.SetAccent(v);     _string.SetAccent(v); }

void AudioSynthRings::strike() {
    _pendingTrig  = true;
    _active       = true;   // arm the voice — Process() runs until it rings out
    _silentBlocks = 0;
}
void AudioSynthRings::setSustain(bool on) {
    _sustained = on;
    if (on) { _active = true; _silentBlocks = 0; }
    _modal.SetSustain(on);
    _string.SetSustain(on);
}
void AudioSynthRings::silence() {
    _sustained = false;
    _modal.SetSustain(false);
    _string.SetSustain(false);
    // Let it ring out naturally; the idle gate deactivates once it's quiet.
}

const char *AudioSynthRings::modeName(uint8_t mode) {
    switch (mode) {
        case ModeModal:  return "Modal";
        case ModeString: return "String";
        default:         return "";
    }
}

void AudioSynthRings::update(void) {
    // Idle gate: if the voice isn't ringing (and nothing pending), emit silence
    // WITHOUT running the resonator — this is what keeps per-voice CPU off the
    // books when a note isn't sounding.
    if (!_active && !_pendingTrig) {
        audio_block_t *z = allocate();
        if (!z) return;
        memset(z->data, 0, sizeof(z->data));
        transmit(z, 0);
        release(z);
        return;
    }

    audio_block_t *out = allocate();
    if (!out) { _pendingTrig = false; return; }

    daisysp::ModalVoice  &modal  = _modal;
    daisysp::StringVoice &string = _string;

    float blockPeak = 0.0f;
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        // Fire the exciter on the first sample after a strike(); one-shot.
        const bool trig = (i == 0) && _pendingTrig;
        float s = (_mode == ModeString) ? string.Process(trig) : modal.Process(trig);
        float a = fabsf(s);
        if (a > blockPeak) blockPeak = a;
        // DaisySP output is ~[-1, 1]. Scale to int16 with clamp.
        int32_t v = (int32_t)(s * 32767.0f);
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        out->data[i] = (int16_t)v;
    }
    _pendingTrig = false;

    // Deactivate once the voice has been near-silent for ~0.25 s (and isn't being
    // sustained). ~1e-4 ≈ -80 dBFS. Re-armed by the next strike().
    if (!_sustained && blockPeak < 1.0e-4f) {
        if (_silentBlocks < 0xFFFF) ++_silentBlocks;
        if (_silentBlocks > 86) _active = false;   // 86 blocks * 128 / 48k ≈ 0.23 s
    } else {
        _silentBlocks = 0;
    }

    transmit(out, 0);
    release(out);
}
