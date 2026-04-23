// SPDX-License-Identifier: MIT
// Host-only mock of the Teensy Audio primitives MpeVaSink talks to.
// Records every call so test cases can assert on state instead of
// listening to a speaker.
//
// Only the methods MpeVaSink actually uses are mocked. If a future
// change to MpeVaSink calls something new on these objects, this
// header needs a matching stub — the host build will fail to link
// and point at the missing method.

#pragma once

#include <stdint.h>

// WAVEFORM_* constants — values match the real Teensy Audio library so
// tests can assert on expected tone_type even through the mock.
#define WAVEFORM_SINE              0
#define WAVEFORM_SAWTOOTH          1
#define WAVEFORM_SQUARE            2
#define WAVEFORM_TRIANGLE          3

class AudioSynthWaveform {
public:
    // Recorded state — tests read these directly.
    short beginCalls     = 0;
    short lastToneType   = -1;
    int   amplitudeCalls = 0;
    float lastAmplitude  = -1.0f;
    int   frequencyCalls = 0;
    float lastFrequency  = -1.0f;

    void begin(short t_type)          { ++beginCalls;     lastToneType  = t_type; }
    void amplitude(float n)           { ++amplitudeCalls; lastAmplitude = n; }
    void frequency(float f)           { ++frequencyCalls; lastFrequency = f; }
};

class AudioEffectEnvelope {
public:
    int   attackCalls   = 0;
    float lastAttackMs  = -1.0f;
    int   releaseCalls  = 0;
    float lastReleaseMs = -1.0f;
    int   decayCalls    = 0;
    float lastDecayMs   = -1.0f;
    int   sustainCalls  = 0;
    float lastSustain   = -1.0f;
    int   noteOnCount   = 0;
    int   noteOffCount  = 0;

    void attack (float ms)            { ++attackCalls;  lastAttackMs  = ms; }
    void release(float ms)            { ++releaseCalls; lastReleaseMs = ms; }
    void decay  (float ms)            { ++decayCalls;   lastDecayMs   = ms; }
    void sustain(float level)         { ++sustainCalls; lastSustain   = level; }
    void noteOn ()                    { ++noteOnCount; }
    void noteOff()                    { ++noteOffCount; }
};

class AudioFilterStateVariable {
public:
    int   frequencyCalls = 0;
    float lastFrequency  = -1.0f;
    int   resonanceCalls = 0;
    float lastResonance  = -1.0f;
    int   octaveControlCalls = 0;
    float lastOctaveControl  = -1.0f;

    void frequency   (float hz)       { ++frequencyCalls;     lastFrequency    = hz; }
    void resonance   (float q)        { ++resonanceCalls;     lastResonance    = q; }
    void octaveControl(float n)       { ++octaveControlCalls; lastOctaveControl = n; }
};
