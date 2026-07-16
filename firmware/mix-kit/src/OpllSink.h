// OpllSink — MidiSink adapter for the OPLL (YM2413) engine (AudioSynthYmfmOPLL).
//
// Like Opl3Sink, this does NOT channel-filter or route: the OPLL engine is a small
// self-contained GM synth (9-voice pool, per-MIDI-channel program mapped onto the 15
// built-in instruments, channel-10 rhythm section), so channel-addressed MIDI is
// forwarded straight through and the engine does its own allocation.
//
// See lib/TDspYmfm/AudioSynthYmfmOPLL.h for the engine API contract.
#pragma once
#include <stdint.h>
#include <AudioSynthYmfmOPLL.h>
#include <MidiSink.h>

class OpllSink : public tdsp::MidiSink {
public:
    explicit OpllSink(AudioSynthYmfmOPLL *opll) : _opll(opll) {}

    void onNoteOn (uint8_t ch, uint8_t note, uint8_t vel) override { _opll->noteOn(ch, note, vel); }
    void onNoteOff(uint8_t ch, uint8_t note, uint8_t)     override { _opll->noteOff(ch, note); }
    void onProgramChange(uint8_t ch, uint8_t prog)        override { _opll->programChange(ch, prog); }
    void onPitchBend(uint8_t ch, float semitones)         override { _opll->pitchBend(ch, semitones); }
    void onPressure (uint8_t ch, float v)                 override { _opll->channelPressure(ch, v); }  // MPE Z -> volume
    void onModWheel (uint8_t /*ch*/, float /*v*/)         override {}   // no-op (OPLL has no mod routing here)
    // onTimbre intentionally NOT overridden: OPLL can't do per-note timbre (fixed ROM
    // voices + a single shared user-voice bank), so CC#74 is a deliberate no-op.
    void onSustain  (uint8_t ch, bool on)                 override { _opll->controlChange(ch, 64, on ? 127 : 0); }
    void onAllNotesOff(uint8_t /*ch*/)                    override { _opll->allNotesOff(); }

private:
    AudioSynthYmfmOPLL *_opll;
};
