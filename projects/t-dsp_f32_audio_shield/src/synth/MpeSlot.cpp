// MpeSlot.cpp — see header for design notes.
//
// Implementation is intentionally thin: voice allocation, MPE routing,
// envelope shape, filter cutoff/resonance, and the LFO live inside
// MpeVaSink (lib/TDspMPE). This file just gates the per-slot gain and
// provides a single place for the OSC layer to call into.

#include <Arduino.h>

#include <OpenAudio_ArduinoLibrary.h>

#include "MpeSlot.h"
#include "../osc/X32FaderLaw.h"

namespace tdsp_synth {

MpeSlot::MpeSlot(const AudioContext &ctx) : _ctx(ctx) {}

void MpeSlot::begin() {
    // MpeVaSink's constructor already seeded each voice into a defined
    // silent state (see MpeVaSink.cpp). Nothing else to do here today;
    // begin() exists for symmetry with MultisampleSlot::begin() so a
    // future state-restore path has a hook to call into.
    applyGain();
}

void MpeSlot::setActive(bool active) {
    if (_active == active) return;
    if (_active && !active) {
        // Switching away — release every voice cleanly so a hung note
        // can't survive the gain ramp.
        _ctx.sink->onAllNotesOff(0);
    }
    _active = active;
    applyGain();
}

void MpeSlot::panic() {
    // Channel 0 is the panic convention in MidiSink.h: release every
    // voice regardless of channel filter.
    _ctx.sink->onAllNotesOff(0);
}

void MpeSlot::setVolume(float v) {
    _volume = tdsp::x32::quantizeFader(v);
    applyGain();
}

void MpeSlot::setOn(bool on) {
    _on = on;
    applyGain();
}

void MpeSlot::setMasterChannel(uint8_t ch) {
    // Defensive: clear any voices that were owned by the old master
    // channel before swapping. MpeVaSink::setMasterChannel doesn't
    // panic on its own, and a stale held voice on the old master
    // could otherwise sit silent forever.
    _ctx.sink->onAllNotesOff(0);
    _ctx.sink->setMasterChannel(ch);
}

uint8_t MpeSlot::masterChannel() const {
    return _ctx.sink->masterChannel();
}

void MpeSlot::applyGain() {
    const float linear =
        (_active && _on)
            ? tdsp::x32::faderToLinear(_volume)
            : 0.0f;
    _ctx.gain->setGain(linear);
}

}  // namespace tdsp_synth
