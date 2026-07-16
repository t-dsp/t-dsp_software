// SF2Sink — MidiSink adapter for the GM SF2 sample engine (Sf2GmEngine).
//
// Like Opl3Sink, this does NOT channel-filter or route to banks: Sf2GmEngine is a
// self-contained General-MIDI synth (voice pool, per-MIDI-channel program, drums on
// channel 10), so we forward channel-addressed MIDI straight through and let the
// engine do GM allocation.
#pragma once
#include <stdint.h>
#include <Sf2GmEngine.h>
#include <MidiSink.h>

class SF2Sink : public tdsp::MidiSink {
public:
    explicit SF2Sink(Sf2GmEngine *sf2) : _sf2(sf2) {}

    void onNoteOn (uint8_t ch, uint8_t note, uint8_t vel) override { _sf2->noteOn(ch, note, vel); }
    void onNoteOff(uint8_t ch, uint8_t note, uint8_t)     override { _sf2->noteOff(ch, note); }
    void onProgramChange(uint8_t ch, uint8_t prog)        override { _sf2->programChange(ch, prog); }
    void onPitchBend(uint8_t ch, float semitones)         override { _sf2->pitchBend(ch, semitones); }
    void onModWheel (uint8_t ch, float v)                 override { _sf2->controlChange(ch, 1,  to7(v)); }
    void onSustain  (uint8_t ch, bool on)                 override { _sf2->controlChange(ch, 64, on ? 127 : 0); }
    void onAllNotesOff(uint8_t ch) override {
        if (ch == 0) _sf2->allNotesOff();               // panic
        else         _sf2->controlChange(ch, 123, 0);   // release just this channel
    }

private:
    Sf2GmEngine *_sf2;
    static uint8_t to7(float v) { return (uint8_t)((v < 0 ? 0 : v > 1 ? 1 : v) * 127.0f + 0.5f); }
};
