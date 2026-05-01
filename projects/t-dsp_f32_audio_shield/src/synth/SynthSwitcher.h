// SynthSwitcher.h — single-active-slot router for synth engines.
//
// Registers as the sole downstream of g_arpFilter. Forwards every MIDI
// event to the active slot's MidiSink and ignores the rest. On
// setActive(idx):
//   * Old active slot: setActive(false) — engine panics held notes,
//     drops its gain stage to 0.
//   * New active slot: setActive(true) — engine restores gain.
// Switch is intentionally hard (no soft gain ramp in this version).
// Adding a 50ms ramp is straightforward later if click artifacts
// appear in practice.
//
// The switcher itself is just a MidiSink adapter — it doesn't own
// audio nodes, gains, or engines. ISynthSlot implementations own those.

#pragma once

#include <stdint.h>

#include <MidiSink.h>

#include "SynthSlot.h"

namespace tdsp_synth {

class SynthSwitcher : public tdsp::MidiSink {
public:
    // Capped at 8 to fit a chained AudioMixer4_F32 pair on each channel
    // (4 + 4 = 8 inputs). Bumping further requires another mixer stage.
    static constexpr int kMaxSlots = 8;

    // Register a slot at index `idx`. Slots may be registered in any
    // order; unset entries stay nullptr and the switcher treats them
    // as silent.
    void setSlot(int idx, ISynthSlot *slot) {
        if (idx < 0 || idx >= kMaxSlots) return;
        _slots[idx] = slot;
    }

    ISynthSlot* slot(int idx) const {
        return (idx >= 0 && idx < kMaxSlots) ? _slots[idx] : nullptr;
    }

    int active() const { return _active; }
    bool isActiveSlot(int idx) const { return idx == _active; }

    // Switch to slot `idx`. Returns true on success, false on bad
    // index or null slot. No-op (and returns true) if already active.
    bool setActive(int idx) {
        if (idx < 0 || idx >= kMaxSlots) return false;
        if (!_slots[idx]) return false;
        if (idx == _active) return true;
        if (_active >= 0 && _slots[_active]) {
            _slots[_active]->setActive(false);
        }
        _active = idx;
        _slots[_active]->setActive(true);
        return true;
    }

    // Force every slot to mute (used during slot reconfig at boot, or
    // a global panic). After this _active = -1 and no slot receives
    // MIDI; call setActive() to bring one back online.
    void deactivateAll() {
        if (_active >= 0 && _slots[_active]) _slots[_active]->setActive(false);
        _active = -1;
    }

    // Panic the active slot only — leaves the routing alone, just
    // releases held notes.
    void panicActive() {
        if (_active >= 0 && _slots[_active]) _slots[_active]->panic();
    }

    // ---- MidiSink: forward to active slot's sink ----
    //
    // Inactive slots see nothing. Channel/velocity processing already
    // happened upstream in MidiRouter / ArpFilter; we just fan.

    void onNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) override {
        if (auto *s = activeSink()) s->onNoteOn(channel, note, velocity);
    }
    void onNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) override {
        if (auto *s = activeSink()) s->onNoteOff(channel, note, velocity);
    }
    void onPitchBend(uint8_t channel, float semitones) override {
        if (auto *s = activeSink()) s->onPitchBend(channel, semitones);
    }
    void onTimbre(uint8_t channel, float value) override {
        if (auto *s = activeSink()) s->onTimbre(channel, value);
    }
    void onPressure(uint8_t channel, float value) override {
        if (auto *s = activeSink()) s->onPressure(channel, value);
    }
    void onModWheel(uint8_t channel, float value) override {
        if (auto *s = activeSink()) s->onModWheel(channel, value);
    }
    void onSustain(uint8_t channel, bool on) override {
        if (auto *s = activeSink()) s->onSustain(channel, on);
    }
    void onProgramChange(uint8_t channel, uint8_t program) override {
        if (auto *s = activeSink()) s->onProgramChange(channel, program);
    }
    void onAllNotesOff(uint8_t channel) override {
        if (auto *s = activeSink()) s->onAllNotesOff(channel);
    }
    void onSysEx(const uint8_t *data, size_t length, bool last) override {
        if (auto *s = activeSink()) s->onSysEx(data, length, last);
    }

    // Transport / clock messages are broadcast to the active slot too
    // (some engines use them for tempo-locked LFOs).
    void onClock()    override { if (auto *s = activeSink()) s->onClock(); }
    void onStart()    override { if (auto *s = activeSink()) s->onStart(); }
    void onContinue() override { if (auto *s = activeSink()) s->onContinue(); }
    void onStop()     override { if (auto *s = activeSink()) s->onStop(); }

private:
    tdsp::MidiSink* activeSink() {
        if (_active < 0 || _active >= kMaxSlots) return nullptr;
        return _slots[_active] ? _slots[_active]->midiSink() : nullptr;
    }

    ISynthSlot *_slots[kMaxSlots] = {};
    int         _active = -1;  // -1 = no active slot (boot state, before setActive)
};

}  // namespace tdsp_synth
