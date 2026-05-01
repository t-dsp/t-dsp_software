// NeuroSlot.cpp — see header for design notes.
//
// Implementation is intentionally thin: voice handling, portamento
// glide, filter / envelope / LFO shaping all live inside NeuroSink
// (lib/TDspNeuro). This file just gates the per-slot gain and hands
// the OSC layer a single place to call into.

#include <Arduino.h>

#include <OpenAudio_ArduinoLibrary.h>

#include "NeuroSlot.h"
#include "../osc/X32FaderLaw.h"

namespace tdsp_synth {

NeuroSlot::NeuroSlot(const AudioContext &ctx) : _ctx(ctx) {}

void NeuroSlot::begin() {
    // Seed the engine with the legacy cold-boot defaults so a fresh
    // boot sounds the same as the legacy project. OSC writes / preset
    // recalls overwrite any of these on demand.
    _ctx.sink->setMidiChannel(0);
    _ctx.sink->setAttack       (0.005f);
    _ctx.sink->setRelease      (0.200f);
    _ctx.sink->setDetuneCents  (7.0f);
    _ctx.sink->setSubLevel     (0.6f);
    _ctx.sink->setOsc3Level    (0.7f);
    _ctx.sink->setFilterCutoff   (600.0f);
    _ctx.sink->setFilterResonance(2.5f);
    _ctx.sink->setLfoRate    (0.0f);
    _ctx.sink->setLfoDepth   (0.5f);
    _ctx.sink->setLfoDest    (NeuroSink::LfoOff);
    _ctx.sink->setLfoWaveform(1);   // triangle — smoothest wobble
    _ctx.sink->setPortamentoMs(0.0f);
    applyGain();
}

void NeuroSlot::setActive(bool active) {
    if (_active == active) return;
    if (_active && !active) {
        // Switching away — release the held note cleanly so a hung
        // voice can't survive the gain ramp.
        _ctx.sink->onAllNotesOff(0);
    }
    _active = active;
    applyGain();
}

void NeuroSlot::panic() {
    // Channel 0 is the panic convention in MidiSink.h: release every
    // voice regardless of channel filter.
    _ctx.sink->onAllNotesOff(0);
}

void NeuroSlot::setVolume(float v) {
    _volume = tdsp::x32::quantizeFader(v);
    applyGain();
}

void NeuroSlot::setOn(bool on) {
    _on = on;
    applyGain();
}

void NeuroSlot::setListenChannel(uint8_t ch) {
    // Defensive: panic any held note before swapping the channel
    // filter. Otherwise a key held on the old channel would sit
    // sounding forever (NeuroSink only fires release on noteOff).
    _ctx.sink->onAllNotesOff(0);
    _ctx.sink->setMidiChannel(ch);
}

uint8_t NeuroSlot::listenChannel() const {
    return _ctx.sink->midiChannel();
}

void NeuroSlot::applyGain() {
    const float linear =
        (_active && _on)
            ? tdsp::x32::faderToLinear(_volume)
            : 0.0f;
    _ctx.gain->setGain(linear);
}

}  // namespace tdsp_synth
