// SilentSlot.h — placeholder ISynthSlot used for empty slots.
//
// Until Phase 1+ adds real engines (mda EPiano, multisample sampler,
// SF2 player), slots 1..3 are filled with SilentSlot instances. They:
//   * Discard every MIDI event delivered to them (default MidiSink
//     methods are all no-ops).
//   * Have no audio output (they're not wired into the audio graph).
//   * Make /synth/active 1..3 select-and-silence, which is the
//     simplest way to verify the switcher's MIDI-routing and Dexed-mute
//     behavior in isolation from real-engine integration work.
//
// When a real engine ships, replace the corresponding SilentSlot
// instance in main.cpp with the engine's adapter; no other code needs
// to change.

#pragma once

#include <MidiSink.h>

#include "SynthSlot.h"

namespace tdsp_synth {

class SilentSlot : public ISynthSlot {
public:
    SilentSlot(const char *idStr, const char *displayStr)
        : _id(idStr), _display(displayStr) {}

    const char*      id()          const override { return _id; }
    const char*      displayName() const override { return _display; }
    tdsp::MidiSink*  midiSink()          override { return &_sink; }

    void setActive(bool /*active*/) override {}
    void panic() override {}

private:
    const char     *_id;
    const char     *_display;
    tdsp::MidiSink  _sink;  // base class — every override is a no-op
};

}  // namespace tdsp_synth
