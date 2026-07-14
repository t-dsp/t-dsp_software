// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.

#include "AudioSynthPlaits.h"

#include "plaits/dsp/dsp.h"   // kBlockSize, kMaxBlockSize

using plaits::Patch;
using plaits::Modulations;
using plaits::Voice;
using stmlib::BufferAllocator;

static inline float clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

AudioSynthPlaits::AudioSynthPlaits() : AudioStream(0, nullptr) {
    BufferAllocator allocator(shared_buffer_, sizeof(shared_buffer_));
    voice_.Init(&allocator);

    // Patch defaults — a bright, mid-length VA voice.
    patch_.engine     = 0;      // Virtual Analog
    patch_.note       = 48.0f;
    patch_.harmonics  = 0.5f;
    patch_.timbre     = 0.5f;
    patch_.morph      = 0.5f;
    patch_.frequency_modulation_amount = 0.0f;
    patch_.timbre_modulation_amount    = 0.0f;
    patch_.morph_modulation_amount     = 0.0f;
    patch_.decay      = 0.5f;
    patch_.lpg_colour = 0.5f;

    // Modulation routing: level = VCA (velocity/pressure), trigger = note gate.
    // level_patched routes LEVEL through the LPG as amplitude; trigger_patched
    // gives each note-on a rising-edge attack. All CV mod inputs stay at 0.
    modulations_.engine    = 0.0f;
    modulations_.note      = 0.0f;
    modulations_.frequency = 0.0f;
    modulations_.harmonics = 0.0f;
    modulations_.timbre    = 0.0f;
    modulations_.morph     = 0.0f;
    modulations_.trigger   = 0.0f;
    modulations_.level     = 0.0f;
    modulations_.frequency_patched = false;
    modulations_.timbre_patched    = false;
    modulations_.morph_patched     = false;
    modulations_.trigger_patched   = true;
    modulations_.level_patched     = true;
}

void AudioSynthPlaits::setEngineIndex(int e) {
    if (e < 0) e = 0;
    if (e >= kNumEngines) e = kNumEngines - 1;
    patch_.engine = e;
}

void AudioSynthPlaits::setNote(float midiNote)   { patch_.note = midiNote; }
void AudioSynthPlaits::setHarmonics(float v)     { patch_.harmonics = clamp01(v); }
void AudioSynthPlaits::setTimbre(float v)        { patch_.timbre = clamp01(v); }
void AudioSynthPlaits::setMorph(float v)         { patch_.morph = clamp01(v); }
void AudioSynthPlaits::setDecay(float v)         { patch_.decay = clamp01(v); }
void AudioSynthPlaits::setLpgColour(float v)     { patch_.lpg_colour = clamp01(v); }
void AudioSynthPlaits::setLevel(float v)         { modulations_.level = clamp01(v); }
void AudioSynthPlaits::gate(bool on)             { modulations_.trigger = on ? 1.0f : 0.0f; }

const char *AudioSynthPlaits::engineName(int e) {
    static const char *kNames[kNumEngines] = {
        "Virtual Analog", "Waveshaping", "FM",       "Granular",
        "Additive",       "Wavetable",   "Chord",    "Speech",
        "Swarm",          "Noise",       "Particle", "String",
        "Modal",          "Bass Drum",   "Snare",    "Hi-Hat",
    };
    if (e < 0 || e >= kNumEngines) return "";
    return kNames[e];
}

void AudioSynthPlaits::update(void) {
    audio_block_t *out = allocate();
    if (!out) return;

    // Render AUDIO_BLOCK_SAMPLES in kBlockSize (12) slices: matches Plaits'
    // native control-block cadence and stays within its 24-sample scratch.
    Voice::Frame frames[plaits::kBlockSize];
    size_t done = 0;
    while (done < AUDIO_BLOCK_SAMPLES) {
        size_t n = AUDIO_BLOCK_SAMPLES - done;
        if (n > plaits::kBlockSize) n = plaits::kBlockSize;
        voice_.Render(patch_, modulations_, frames, n);
        for (size_t i = 0; i < n; ++i) out->data[done + i] = frames[i].out;
        done += n;
    }

    transmit(out, 0);
    release(out);
}
