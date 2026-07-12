// Opl3Sink — MidiSink adapter for the GM OPL3 engine (AudioSynthYmfmOPL3).
//
// Unlike the OPM sinks, this does NOT channel-filter or route to banks: OPL3 is
// a self-contained General-MIDI synth (18-channel voice pool, per-MIDI-channel
// program, drums on channel 10), so we just forward channel-addressed MIDI
// straight through and let the engine do GM allocation.
//
// See lib/TDspYmfm/OPL3_DMXOPL_SPEC.md for the engine API contract.
#pragma once
#include <stdint.h>
#include <AudioSynthYmfmOPL3.h>
#include <MidiSink.h>

class Opl3Sink : public tdsp::MidiSink {
public:
    explicit Opl3Sink(AudioSynthYmfmOPL3 *opl) : _opl(opl) {}

    void onNoteOn (uint8_t ch, uint8_t note, uint8_t vel) override { _opl->noteOn(ch, note, vel); }
    void onNoteOff(uint8_t ch, uint8_t note, uint8_t)     override { _opl->noteOff(ch, note); }
    void onProgramChange(uint8_t ch, uint8_t prog)        override { _opl->programChange(ch, prog); }
    void onPitchBend(uint8_t ch, float semitones)         override { _opl->pitchBend(ch, semitones); }
    void onModWheel (uint8_t ch, float v)                 override { _opl->controlChange(ch, 1,  to7(v)); }
    void onSustain  (uint8_t ch, bool on)                 override { _opl->controlChange(ch, 64, on ? 127 : 0); }
    void onAllNotesOff(uint8_t /*ch*/)                    override { _opl->allNotesOff(); }

private:
    AudioSynthYmfmOPL3 *_opl;
    static uint8_t to7(float v) { return (uint8_t)((v < 0 ? 0 : v > 1 ? 1 : v) * 127.0f + 0.5f); }
};
