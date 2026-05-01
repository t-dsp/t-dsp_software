// AcidSlot.cpp — see header for design notes.
//
// Implementation is intentionally thin: voice state, last-note priority,
// slide-when-overlapping, accent, and the software filter envelope all
// live inside AcidSink (lib/TDspAcid). This file gates the per-slot
// gain, forwards setActive/panic to onAllNotesOff, and runs a per-loop
// tick into the engine so its filter-envelope decay and pitch glide
// advance.

#include <Arduino.h>

#include <OpenAudio_ArduinoLibrary.h>

#include "AcidSlot.h"
#include "../osc/X32FaderLaw.h"

namespace tdsp_synth {

AcidSlot::AcidSlot(const AudioContext &ctx) : _ctx(ctx) {}

void AcidSlot::begin() {
    // AcidSink's constructor already seeded the voice into a defined
    // silent state (osc amplitude 0, filter at base cutoff, env idle).
    // begin() exists for symmetry with the other slots so a future
    // state-restore path has a hook to call into.
    applyGain();
}

void AcidSlot::setActive(bool active) {
    if (_active == active) return;
    if (_active && !active) {
        // Switching away — release the held note cleanly so a hung
        // voice can't survive the gain ramp.
        _ctx.sink->onAllNotesOff(0);
    }
    _active = active;
    applyGain();
}

void AcidSlot::panic() {
    // Channel 0 is the panic convention in MidiSink.h: release the
    // voice regardless of channel filter.
    _ctx.sink->onAllNotesOff(0);
}

void AcidSlot::tick(uint32_t nowMs) {
    // The engine ticks even when this slot is inactive — onAllNotesOff
    // above clears _held so the early-return inside AcidSink::tick
    // makes this near-free.
    _ctx.sink->tick(nowMs);
}

void AcidSlot::setVolume(float v) {
    _volume = tdsp::x32::quantizeFader(v);
    applyGain();
}

void AcidSlot::setOn(bool on) {
    _on = on;
    applyGain();
}

void AcidSlot::setListenChannel(uint8_t ch) {
    _ctx.sink->setMidiChannel(ch);
}

uint8_t AcidSlot::listenChannel() const {
    return _ctx.sink->midiChannel();
}

void AcidSlot::applyGain() {
    const float linear =
        (_active && _on)
            ? tdsp::x32::faderToLinear(_volume)
            : 0.0f;
    _ctx.gain->setGain(linear);
}

}  // namespace tdsp_synth
