// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project. Wraps DaisySP (Electrosmith, MIT) modal/string voices.
//
// AudioSynthRings — one Rings-style physical-modelling resonator voice as a
// Teensy Audio Library node (mono, int16, output on channel 0).
//
// Rings is a modal/sympathetic-string resonator: an excitation (mallet/pluck/
// noise) rings a bank of tuned modes. We use DaisySP's ModalVoice (modal bar/
// bell) and StringVoice (plucked/bowed string) — the same DSP lineage as
// Mutable's Rings/Elements — and switch between them per the MODE control.
//
// DaisySP voices process ONE float sample per Process() call, so update()
// renders AUDIO_BLOCK_SAMPLES samples in a loop and scales float -> int16.
// Polyphony/MPE live one level up in RingsSink (one resonator per note).

#pragma once

#include <stdint.h>

#include <Arduino.h>
#include <AudioStream.h>

#include "daisysp/PhysicalModeling/modalvoice.h"
#include "daisysp/PhysicalModeling/stringvoice.h"

class AudioSynthRings : public AudioStream {
public:
    enum Mode : uint8_t { ModeModal = 0, ModeString = 1, kNumModes = 2 };

    AudioSynthRings();

    // Initialize the DaisySP voices. MUST be called from setup() (after
    // AudioMemory), NOT the constructor: doing the DaisySP Init (delayline
    // clears, resonator setup) during global static construction races with
    // the SAI1 audio update ISR, which is already live from the TDM objects.
    void begin();

    // --- Voice controls (all persist across notes) -----------------------
    void setMode(uint8_t mode);
    uint8_t mode() const { return _mode; }
    void setFreqHz(float hz);                // resonator root frequency
    void setStructure(float v);              // 0..1 inharmonicity / mode spread
    void setBrightness(float v);             // 0..1 HF content
    void setDamping(float v);                // 0..1 decay length (1 = long ring)
    void setAccent(float v);                 // 0..1 excitation strength

    // --- Per-note gate ---------------------------------------------------
    void strike();                           // trigger the exciter (note-on)
    void setSustain(bool on);                // hold = keep exciting / ring
    void silence();                          // damp + stop sustaining

    static const char *modeName(uint8_t mode);

    virtual void update(void) override;

private:
    daisysp::ModalVoice  _modal;
    daisysp::StringVoice _string;
    uint8_t  _mode        = ModeModal;
    bool     _pendingTrig = false;           // fire exciter on next block's 1st sample
    // Idle gate: the DaisySP resonator is ~32% CPU/voice and runs every block
    // with no built-in idle detection. We skip Process() (and emit silence) once
    // the voice has been quiet for a while, so idle voices cost ~nothing and only
    // actively-ringing notes are paid for. Re-armed by strike()/sustain.
    bool     _active      = false;
    bool     _sustained   = false;
    uint16_t _silentBlocks = 0;
};
