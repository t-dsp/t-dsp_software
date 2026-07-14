// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project. Wraps Mutable Instruments Plaits (MIT, Emilie Gillet).
//
// AudioSynthPlaits — one authentic Plaits macro-oscillator voice as a Teensy
// Audio Library node (mono, int16, output on channel 0).
//
// Why chunked rendering
// ---------------------
// Plaits renders at a native 48 kHz (== our system rate, so no resampling),
// but its internal per-block scratch is only kMaxBlockSize = 24 samples, and
// its control envelopes advance once per Render() call sized to kBlockSize=12.
// Calling Voice::Render() with AUDIO_BLOCK_SAMPLES (128) directly — as some
// ports do — overruns out_buffer_[24]. update() therefore renders in
// kBlockSize (12) slices, which also keeps the LPG/decay timing identical to
// the hardware module.
//
// Polyphony/MPE live one level up in Plaits2Sink, which owns an array of these
// nodes (one Plaits Voice per note). This class is deliberately single-voice.

#pragma once

#include <stdint.h>

#include <Arduino.h>       // core macros (F_CPU_ACTUAL, IRQ_SOFTWARE) AudioStream.h needs
#include <AudioStream.h>

#include "plaits/dsp/voice.h"
#include "stmlib/utils/buffer_allocator.h"

class AudioSynthPlaits : public AudioStream {
public:
    // Number of synthesis engines Plaits registers (see voice.cpp Init):
    // 0 VA, 1 Waveshaping, 2 FM, 3 Granular, 4 Additive, 5 Wavetable,
    // 6 Chord, 7 Speech, 8 Swarm, 9 Noise, 10 Particle, 11 String,
    // 12 Modal, 13 Bass Drum, 14 Snare, 15 Hi-Hat.
    static constexpr int kNumEngines = 16;

    AudioSynthPlaits();

    // --- Patch (per-voice, all persist across notes) ---------------------
    void setEngineIndex(int e);              // 0..kNumEngines-1
    int  engineIndex() const { return patch_.engine; }
    void setNote(float midiNote);            // fractional MIDI note -> pitch
    void setHarmonics(float v);              // 0..1
    void setTimbre(float v);                 // 0..1
    void setMorph(float v);                  // 0..1
    void setDecay(float v);                  // 0..1 (LPG decay length)
    void setLpgColour(float v);              // 0..1 (LPG "colour"/brightness)

    // --- Per-note expression --------------------------------------------
    // level acts as the VCA (velocity/MPE-pressure). trigger gates note
    // articulation (rising edge fires the attack; falling starts release).
    void setLevel(float v);                  // 0..1
    void gate(bool on);                      // note held?

    static const char *engineName(int e);

    virtual void update(void) override;

private:
    plaits::Patch       patch_;
    plaits::Modulations modulations_;
    plaits::Voice       voice_;
    // Persistent working memory the Plaits engines carve up in Init(). Sized
    // per the upstream Teensy port; the largest engine fits well under this.
    char                shared_buffer_[16384];
};
