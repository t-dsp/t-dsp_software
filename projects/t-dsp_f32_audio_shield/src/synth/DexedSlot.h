// DexedSlot.h — ISynthSlot adapter for the existing AudioSynthDexed engine.
//
// Wraps the statically-allocated globals declared in main.cpp
// (AudioSynthDexed, DexedSink, AudioEffectGain_F32) without moving or
// re-instantiating them. The slot observes external state — g_dexedOn
// and g_dexedVolume — via const pointers passed at construction; OSC
// handlers in main.cpp continue to mutate that state and call
// DexedSlot::applyGain() to push a fresh gain value into the audio
// graph.
//
// Active gating: when the switcher deactivates this slot the engine is
// panicked and its gain stage drops to 0. The stored on/volume state is
// preserved across switches, so flipping back to Dexed restores the
// previous fader position automatically.

#pragma once

#include <synth_dexed.h>

#include "../DexedSink.h"
#include "../osc/X32FaderLaw.h"
#include "SynthSlot.h"

class AudioEffectGain_F32;  // forward-decl; full type comes from OpenAudio_ArduinoLibrary

namespace tdsp_synth {

class DexedSlot : public ISynthSlot {
public:
    // Construct with pointers to the engine, MIDI adapter, gain stage,
    // and observed on/volume state. Pointers must outlive this object
    // (they're file-scope statics in main.cpp).
    DexedSlot(AudioSynthDexed     *dexed,
              DexedSink           *sink,
              AudioEffectGain_F32 *gain,
              const bool          *onPtr,
              const float         *volX32Ptr)
        : _dexed(dexed), _sink(sink), _gain(gain),
          _onPtr(onPtr), _volX32Ptr(volX32Ptr) {}

    const char*      id()          const override { return "dexed"; }
    const char*      displayName() const override { return "Dexed FM"; }
    tdsp::MidiSink*  midiSink()          override { return _sink; }

    bool isActive() const { return _active; }

    void setActive(bool active) override {
        if (_active == active) return;
        if (_active && !active) {
            // Switching away — release everything before muting so a
            // hung note can't survive the gain ramp.
            _dexed->panic();
        }
        _active = active;
        applyGain();
    }

    void panic() override {
        _dexed->panic();
    }

    // Recompute the gain stage from the observed on/volume state and
    // active flag. Call after any change to g_dexedOn or g_dexedVolume.
    void applyGain();

private:
    AudioSynthDexed     *_dexed;
    DexedSink           *_sink;
    AudioEffectGain_F32 *_gain;
    const bool          *_onPtr;
    const float         *_volX32Ptr;
    bool                 _active = false;
};

}  // namespace tdsp_synth
