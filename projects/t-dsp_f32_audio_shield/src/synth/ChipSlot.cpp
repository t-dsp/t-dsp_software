// ChipSlot.cpp — see header for design notes.
//
// Implementation is intentionally thin: voice allocation, pulse/triangle/
// noise mixing, the arpeggiator, and the ADSR envelope all live inside
// ChipSink (lib/TDspChip). This file just gates the per-slot gain and
// provides a single place for the OSC layer to call into.

#include <Arduino.h>

#include <OpenAudio_ArduinoLibrary.h>

#include "ChipSlot.h"
#include "../osc/X32FaderLaw.h"

namespace tdsp_synth {

ChipSlot::ChipSlot(const AudioContext &ctx) : _ctx(ctx) {}

void ChipSlot::begin() {
    // ChipSink's constructor already seeded the oscillators / mixer /
    // envelope into a defined silent state (see ChipSink.cpp). begin()
    // exists for symmetry with the other slots and to push the initial
    // gain into the audio graph before audio is unmuted.
    applyGain();
}

void ChipSlot::setActive(bool active) {
    if (_active == active) return;
    if (_active && !active) {
        // Switching away — release the held note cleanly so its
        // envelope decays through silent, not through a hard mute.
        _ctx.sink->onAllNotesOff(0);
    }
    _active = active;
    applyGain();
}

void ChipSlot::panic() {
    // Channel 0 is the panic convention in MidiSink.h: release the
    // monophonic voice regardless of the channel filter.
    _ctx.sink->onAllNotesOff(0);
}

void ChipSlot::setVolume(float v) {
    _volume = tdsp::x32::quantizeFader(v);
    applyGain();
}

void ChipSlot::setOn(bool on) {
    _on = on;
    applyGain();
}

void ChipSlot::setListenChannel(uint8_t ch) {
    // Defensive: panic any held note before switching channel filter
    // so a stale held voice on the old channel doesn't sit silent.
    _ctx.sink->onAllNotesOff(0);
    _ctx.sink->setMidiChannel(ch);
}

uint8_t ChipSlot::listenChannel() const {
    return _ctx.sink->midiChannel();
}

void ChipSlot::applyGain() {
    const float linear =
        (_active && _on)
            ? tdsp::x32::faderToLinear(_volume)
            : 0.0f;
    _ctx.gain->setGain(linear);
}

}  // namespace tdsp_synth
