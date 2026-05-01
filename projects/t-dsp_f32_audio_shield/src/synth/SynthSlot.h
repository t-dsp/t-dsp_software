// SynthSlot.h — interface for a pluggable synth engine.
//
// A "slot" is one synth voice/engine the user can switch to. Exactly one
// slot is active at a time (see project memory feedback_synth_switch_not_layer
// — the user chose switch-don't-layer for UX simplicity). Inactive slots
// stay instantiated but produce no audio and receive no MIDI; switching
// is cheap (gain set to 0, MIDI rerouted).
//
// Each concrete engine (Dexed, mda EPiano, multisample sampler, SF2)
// provides an ISynthSlot adapter that:
//   * Exposes its MidiSink for the switcher to forward events to.
//   * Implements setActive() to ramp its output gain to 0 (panic held
//     notes when switched away from).
//   * Reports a stable id() string so the OSC contract can address the
//     slot uniformly: "/synth/0/...", "/synth/1/...", etc.
//
// Engines must be statically instantiated in main.cpp — Teensy Audio
// Library requires this for AudioConnection wiring. The slot abstraction
// only owns control-plane concerns, not the audio graph.

#pragma once

#include <stdint.h>

namespace tdsp { class MidiSink; }

namespace tdsp_synth {

class ISynthSlot {
public:
    virtual ~ISynthSlot() = default;

    // Stable short identifier ("dexed", "epiano", "sampler", "sf2",
    // "silent"). Used in the OSC tree and persisted config. Lowercase,
    // no spaces.
    virtual const char* id() const = 0;

    // Human-readable name shown in the UI ("Dexed FM", "EP Piano", ...).
    virtual const char* displayName() const = 0;

    // The MidiSink the switcher fans MIDI events into when this slot
    // is active. May be a singleton owned by the engine (e.g. DexedSink
    // wraps the AudioSynthDexed instance). Never null.
    virtual tdsp::MidiSink* midiSink() = 0;

    // Activation hook. The switcher calls setActive(false) on the
    // outgoing slot (engine should panic held notes and zero its gain
    // stage), then setActive(true) on the incoming slot (engine should
    // restore its gain to the configured fader/on state).
    virtual void setActive(bool active) = 0;

    // Hard panic — release every held note immediately. Called by
    // /synth/<n>/panic and as a defensive fallback during slot teardown.
    virtual void panic() = 0;
};

}  // namespace tdsp_synth
