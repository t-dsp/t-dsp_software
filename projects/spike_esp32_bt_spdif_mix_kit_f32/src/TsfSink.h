// TsfSink — MidiSink adapter for the TinySoundFont GM engine.
//
// TSF is a self-contained General-MIDI synth with a channel API, so we forward
// channel-addressed MIDI straight to tsf_channel_*. Channels are 0-based in TSF
// (MIDI 1..16 -> 0..15); channel 10 is the drum channel (set up in synthBegin).
//
// Rendering runs in the audio ISR (AudioSynthTsf::update); TSF is not thread-safe,
// so every mutation here is wrapped in AudioNoInterrupts. Voices + channels are
// pre-created in synthBegin so note-on never reallocates under the lock.
#pragma once
#include <stdint.h>
#include <Audio.h>       // AudioNoInterrupts / AudioInterrupts
#include <tsf.h>
#include <MidiSink.h>

class TsfSink : public tdsp::MidiSink {
public:
    explicit TsfSink(tsf **t) : _t(t) {}

    void onNoteOn(uint8_t ch, uint8_t note, uint8_t vel) override {
        tsf *t = *_t; if (!t) return;
        AudioNoInterrupts(); tsf_channel_note_on(t, ch - 1, note, vel / 127.0f); AudioInterrupts();
    }
    void onNoteOff(uint8_t ch, uint8_t note, uint8_t) override {
        tsf *t = *_t; if (!t) return;
        AudioNoInterrupts(); tsf_channel_note_off(t, ch - 1, note); AudioInterrupts();
    }
    void onProgramChange(uint8_t ch, uint8_t prog) override {
        tsf *t = *_t; if (!t) return;
        AudioNoInterrupts(); tsf_channel_set_presetnumber(t, ch - 1, prog, ch == 10 ? 1 : 0); AudioInterrupts();
    }
    void onPitchBend(uint8_t ch, float semitones) override {
        tsf *t = *_t; if (!t) return;                 // pitchRange is set to 12 semis in synthBegin
        int wheel = (int)((semitones + 12.0f) / 24.0f * 16383.0f + 0.5f);
        wheel = wheel < 0 ? 0 : wheel > 16383 ? 16383 : wheel;
        AudioNoInterrupts(); tsf_channel_set_pitchwheel(t, ch - 1, wheel); AudioInterrupts();
    }
    void onModWheel(uint8_t ch, float v) override {
        tsf *t = *_t; if (!t) return;
        AudioNoInterrupts(); tsf_channel_midi_control(t, ch - 1, 1, to7(v)); AudioInterrupts();
    }
    void onSustain(uint8_t ch, bool on) override {
        tsf *t = *_t; if (!t) return;
        AudioNoInterrupts(); tsf_channel_midi_control(t, ch - 1, 64, on ? 127 : 0); AudioInterrupts();
    }
    void onAllNotesOff(uint8_t ch) override {
        tsf *t = *_t; if (!t) return;
        AudioNoInterrupts();
        if (ch == 0) tsf_note_off_all(t);             // panic
        else         tsf_channel_note_off_all(t, ch - 1);
        AudioInterrupts();
    }

private:
    tsf **_t;
    static uint8_t to7(float v) { return (uint8_t)((v < 0 ? 0 : v > 1 ? 1 : v) * 127.0f + 0.5f); }
};
