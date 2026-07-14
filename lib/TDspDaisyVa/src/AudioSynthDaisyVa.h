// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project. Virtual-analog voice from DaisySP (Electrosmith, MIT).
//
// AudioSynthDaisyVa — one subtractive VA voice as a Teensy Audio node (mono
// int16, output on channel 0): two PolyBLEP oscillators (detunable) -> Moog
// ladder filter -> ADSR VCA. DaisySP modules process one float sample per call,
// so update() loops AUDIO_BLOCK_SAMPLES and scales to int16.
//
// Init is deferred to begin() (NOT the constructor): running DaisySP Init during
// global static construction races the SAI1 audio update ISR (learned building
// lib/TDspRings). An idle gate skips processing once the envelope has released.
//
// Polyphony/MPE live one level up in DaisyVaSink.

#pragma once

#include <stdint.h>

#include <Arduino.h>
#include <AudioStream.h>

#include "daisysp/Synthesis/oscillator.h"
#include "daisysp/Filters/ladder.h"
#include "daisysp/Control/adsr.h"

class AudioSynthDaisyVa : public AudioStream {
public:
    // Band-limited waveform choices (PolyBLEP). Index into the wave table below.
    enum Wave : uint8_t { WaveSaw = 0, WaveSquare = 1, WaveTri = 2, kNumWaves = 3 };

    AudioSynthDaisyVa();

    // Deferred DSP init — call from setup() after AudioMemory. See note above.
    void begin();

    // --- Voice controls (persist across notes) ---------------------------
    void setWaveform(uint8_t wave);
    void setFreqHz(float hz);                // base pitch (osc1); osc2 = detuned
    void setDetuneCents(float cents);        // osc2 detune from osc1
    void setCutoffHz(float hz);              // ladder cutoff
    void setResonance(float r);              // 0..1
    void setAttack(float s);
    void setDecay(float s);
    void setSustain(float level);            // 0..1
    void setRelease(float s);
    void setLevel(float v);                  // per-note VCA (velocity/pressure), 0..1

    // --- Gate ------------------------------------------------------------
    void noteOn();
    void noteOff();

    static const char *waveName(uint8_t wave);

    virtual void update(void) override;

private:
    daisysp::Oscillator   _osc1, _osc2;
    daisysp::LadderFilter _filter;
    daisysp::Adsr         _env;

    uint8_t _wave      = WaveSaw;
    float   _baseHz    = 220.0f;
    float   _detune    = 7.0f;      // cents
    float   _cutoffHz  = 4000.0f;
    float   _res       = 0.2f;
    float   _level     = 0.8f;
    bool    _gate      = false;

    // Idle gate — deactivate once released and silent (env at 0).
    bool     _active       = false;
    uint16_t _silentBlocks = 0;

    void applyDetune();
    static uint8_t toDaisyWave(uint8_t wave);
};
