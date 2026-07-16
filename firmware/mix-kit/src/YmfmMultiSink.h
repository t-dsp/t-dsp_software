// YmfmMultiSink — MidiSink adapter for the multitimbral OPM manager.
//
// Unlike YmfmSink, this does NOT channel-filter — routing each channel to its
// own OPM bank (and honoring the song's Program Change per channel) is the whole
// point. The manager (YmfmOpmMulti) owns the channel->bank map and the
// program->voice table.
#pragma once
#include <stdint.h>
#include <YmfmOpmMulti.h>
#include <MidiSink.h>

class YmfmMultiSink : public tdsp::MidiSink {
public:
    explicit YmfmMultiSink(tdsp::ymfmopm::YmfmOpmMulti *m) : _m(m) {}
    void onNoteOn (uint8_t ch, uint8_t note, uint8_t vel) override { _m->noteOn(ch, note, vel); }
    void onNoteOff(uint8_t ch, uint8_t note, uint8_t)     override { _m->noteOff(ch, note); }
    void onProgramChange(uint8_t ch, uint8_t prog)        override { _m->programChange(ch, prog); }
    void onAllNotesOff(uint8_t)                           override { _m->allNotesOff(); }
private:
    tdsp::ymfmopm::YmfmOpmMulti *_m;
};
