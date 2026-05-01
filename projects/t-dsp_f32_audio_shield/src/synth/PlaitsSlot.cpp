// PlaitsSlot.cpp — see header for design notes.
//
// Implementation is intentionally thin: voice allocation, MPE routing,
// macro-knob handling, and the LPG envelope all live inside PlaitsSink
// (lib/TDspPlaits). This file just gates the per-slot gain and provides
// a single place for the OSC layer to call into.

#include <Arduino.h>

#include <OpenAudio_ArduinoLibrary.h>

#include "PlaitsSlot.h"
#include "../osc/X32FaderLaw.h"

namespace tdsp_synth {

PlaitsSlot::PlaitsSlot(const AudioContext &ctx) : _ctx(ctx) {}

void PlaitsSlot::begin() {
    // PlaitsSink's constructor already seeded each voice into a defined
    // silent state. begin() exists for symmetry with MpeSlot/SamplerSlot
    // and to push the initial gain into the audio graph before audio
    // is unmuted.
    applyGain();
}

void PlaitsSlot::setActive(bool active) {
    if (_active == active) return;
    if (_active && !active) {
        _ctx.sink->onAllNotesOff(0);
    }
    _active = active;
    applyGain();
}

void PlaitsSlot::panic() {
    _ctx.sink->onAllNotesOff(0);
}

void PlaitsSlot::setVolume(float v) {
    _volume = tdsp::x32::quantizeFader(v);
    applyGain();
}

void PlaitsSlot::setOn(bool on) {
    _on = on;
    applyGain();
}

void PlaitsSlot::setMasterChannel(uint8_t ch) {
    // Defensive: clear voices on the old channel before swapping so a
    // stale held voice can't sit silent forever.
    _ctx.sink->onAllNotesOff(0);
    _ctx.sink->setMasterChannel(ch);
}

uint8_t PlaitsSlot::masterChannel() const {
    return _ctx.sink->masterChannel();
}

void PlaitsSlot::applyGain() {
    const float linear =
        (_active && _on)
            ? tdsp::x32::faderToLinear(_volume)
            : 0.0f;
    _ctx.gain->setGain(linear);
}

}  // namespace tdsp_synth
