// YmfmSink — MidiSink adapter for the ymfm OPM engine (AudioSynthYmfmOPM).
//
// Filters incoming MIDI to a single configured channel (or omni when
// listenChannel == 0) and forwards notes to the OPM's 8-voice allocator.
// OPM here is a single-timbre engine (one patch across all channels), like the
// Dexed backend; per-note MPE expression is not modelled — pitch bend / mod /
// pressure fall through to the MidiSink base-class no-ops for now.
#pragma once

#include <stdint.h>
#include <AudioSynthYmfmOPM.h>
#include <MidiSink.h>

class YmfmSink : public tdsp::MidiSink {
public:
    explicit YmfmSink(AudioSynthYmfmOPM *opm) : _opm(opm) {}

    // 0 = omni (all channels). 1..16 = single channel. Out-of-range -> omni.
    void setListenChannel(uint8_t channel) { _listenChannel = (channel <= 16) ? channel : 0; }
    uint8_t listenChannel() const { return _listenChannel; }

    void onNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) override {
        if (!listens(channel)) return;
        _opm->noteOn(note, velocity);
    }
    void onNoteOff(uint8_t channel, uint8_t note, uint8_t /*velocity*/) override {
        if (!listens(channel)) return;
        _opm->noteOff(note);
    }
    void onAllNotesOff(uint8_t channel) override {
        if (channel != 0 && !listens(channel)) return;   // channel 0 == panic
        _opm->allNotesOff();
    }

private:
    AudioSynthYmfmOPM *_opm;
    uint8_t            _listenChannel = 1;   // main.cpp overrides to 0 (omni)
    bool listens(uint8_t ch) const { return _listenChannel == 0 || _listenChannel == ch; }
};
