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
    // pitchRange is set to 48 semis in synthBegin (covers normal +-2 AND MPE +-48).
    void onPitchBend(uint8_t ch, float semitones) override {
        tsf *t = *_t; if (!t) return;
        int wheel = (int)((semitones + 48.0f) / 96.0f * 16383.0f + 0.5f);
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
    // MPE expression. Pressure (Z-axis) -> per-channel volume, which TSF applies to
    // LIVE voices, so a held note swells with finger pressure. Timbre (CC74, the
    // slide/Y-axis) -> per-channel lowpass cutoff: TSF's CC74 handler was patched
    // (tsf_channel_midi_control) to close each channel's filter as the slide drops,
    // and the render loop tracks it on held notes. At rest (normal MIDI: no pressure,
    // no CC74) volume stays 1.0 and the filter stays patch-open, so this is a no-op
    // outside MPE.
    void onPressure(uint8_t ch, float v) override {
        tsf *t = *_t; if (!t) return;
        AudioNoInterrupts(); tsf_channel_set_volume(t, ch - 1, v); AudioInterrupts();
    }
    void onTimbre(uint8_t ch, float v) override {
        tsf *t = *_t; if (!t) return;
        AudioNoInterrupts(); tsf_channel_midi_control(t, ch - 1, 74, to7(v)); AudioInterrupts();
    }
    void onAllNotesOff(uint8_t ch) override {
        tsf *t = *_t; if (!t) return;
        AudioNoInterrupts();
        if (ch == 0) {
            tsf_note_off_all(t);                      // panic all notes
            // Recenter per-channel MPE expression too. Pressure maps to channel VOLUME
            // (tsf_channel_set_volume); without this, a note that released at low pressure
            // leaves its channel quiet/silent, and the next song/instrument on that channel
            // plays attenuated -> the "worked great then faded out" drift, cured by reboot.
            for (int c = 0; c < 16; c++) {
                tsf_channel_set_volume(t, c, 1.0f);
                tsf_channel_set_pitchwheel(t, c, 8192);   // recenter bend
            }
        } else {
            tsf_channel_note_off_all(t, ch - 1);
        }
        AudioInterrupts();
    }

private:
    tsf **_t;
    static uint8_t to7(float v) { return (uint8_t)((v < 0 ? 0 : v > 1 ? 1 : v) * 127.0f + 0.5f); }
};
