// SupersawSlot.cpp — see header for design notes.
//
// Implementation is intentionally thin: voice handling, portamento
// glide, filter / envelope shaping all live inside SupersawSink
// (lib/TDspSupersaw). This file just gates the per-slot gain and
// hands the OSC layer a single place to call into.

#include <Arduino.h>

#include <OpenAudio_ArduinoLibrary.h>

#include "SupersawSlot.h"
#include "../osc/X32FaderLaw.h"

namespace tdsp_synth {

SupersawSlot::SupersawSlot(const AudioContext &ctx) : _ctx(ctx) {}

void SupersawSlot::begin() {
    // Seed the engine with the legacy cold-boot defaults so a fresh
    // boot sounds the same as the legacy project. OSC writes / preset
    // recalls overwrite any of these on demand.
    _ctx.sink->setMidiChannel(0);
    _ctx.sink->setDetuneCents(18.0f);
    _ctx.sink->setMixCenter(0.4f);
    _ctx.sink->setCutoff(9000.0f);
    _ctx.sink->setResonance(1.0f);
    _ctx.sink->setAttack(0.05f);
    _ctx.sink->setDecay(0.3f);
    _ctx.sink->setSustain(0.8f);
    _ctx.sink->setRelease(0.6f);
    _ctx.sink->setPortamentoMs(0.0f);
    applyGain();
}

void SupersawSlot::setActive(bool active) {
    if (_active == active) return;
    if (_active && !active) {
        // Switching away — release the held note cleanly so a hung
        // voice can't survive the gain ramp.
        _ctx.sink->onAllNotesOff(0);
    }
    _active = active;
    applyGain();
}

void SupersawSlot::panic() {
    // Channel 0 is the panic convention in MidiSink.h: release every
    // voice regardless of channel filter.
    _ctx.sink->onAllNotesOff(0);
}

void SupersawSlot::setVolume(float v) {
    _volume = tdsp::x32::quantizeFader(v);
    applyGain();
}

void SupersawSlot::setOn(bool on) {
    _on = on;
    applyGain();
}

void SupersawSlot::setListenChannel(uint8_t ch) {
    // Defensive: panic any held note before swapping the channel
    // filter. Otherwise a key held on the old channel would sit
    // sounding forever (SupersawSink only fires release on noteOff).
    _ctx.sink->onAllNotesOff(0);
    _ctx.sink->setMidiChannel(ch);
}

uint8_t SupersawSlot::listenChannel() const {
    return _ctx.sink->midiChannel();
}

void SupersawSlot::applyGain() {
    const float linear =
        (_active && _on)
            ? tdsp::x32::faderToLinear(_volume)
            : 0.0f;
    _ctx.gain->setGain(linear);
}

}  // namespace tdsp_synth
