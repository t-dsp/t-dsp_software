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
#include "ReplayGain.h"  // gmProgramTrim() — Tier-2 per-GM-program song norm

class TsfSink : public tdsp::MidiSink {
public:
    explicit TsfSink(tsf **t) : _t(t) {}

    // MPE vs normal-GM gate. In MPE mode CC74 is the timbre/slide axis (-> per-channel
    // lowpass cutoff); in normal GM playback CC74 is a file controller we must NOT let
    // slam the filter shut. Driven from applyMidiMode()/synthSetMpeMode() in main.cpp.
    void setMpe(bool m) { _mpe = m; }

    // Tier-2 ReplayGain: a 128-entry per-GM-program linear-gain table applied per-channel
    // (via tsf_channel_set_volume, which files never touch — CC7/CC11 aren't routed here)
    // on Program Change during normal GM playback. nullptr disables. See REPLAYGAIN.md.
    void setGmSongTrim(const float *table128) { _gmTrim = table128; }

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
        AudioNoInterrupts();
        tsf_channel_set_presetnumber(t, ch - 1, prog, ch == 10 ? 1 : 0);
        // Tier-2 song norm: level-match this channel to its program's loudness. Only in
        // normal GM playback (in MPE, channel volume is the pressure/Z axis) and not on the
        // drum channel (ch10). Unity table = transparent.
        if (_gmTrim && !_mpe && ch != 10)
            tsf_channel_set_volume(t, ch - 1, tdsp::gmProgramTrim(_gmTrim, prog));
        AudioInterrupts();
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
        // Channel pressure is the MPE Z-axis and maps to per-channel VOLUME. In NORMAL GM
        // playback a file's OWN mono aftertouch (routinely automated on brass/wind parts)
        // must NOT drive volume: a low aftertouch value turns the whole channel down to
        // near-silence and the part wobbles up/down with the curve. Only honor it in MPE.
        if (!_mpe) return;
        AudioNoInterrupts(); tsf_channel_set_volume(t, ch - 1, v); AudioInterrupts();
    }
    void onTimbre(uint8_t ch, float v) override {
        tsf *t = *_t; if (!t) return;
        // CC74 is the MPE timbre/slide (Y-axis) -> per-channel lowpass cutoff. In NORMAL GM
        // playback a file's OWN CC74 (Brightness, routinely automated on strings/brass) must
        // NOT be taken this way: our mapping clamps the channel filter shut (up to 4 octaves)
        // and cutoffCents is sticky, so one low CC74 silences the part for the rest of the song
        // AND across restarts. Only honor CC74-as-cutoff in MPE mode; GM leaves the patch open.
        if (!_mpe) return;
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
                tsf_channel_midi_control(t, c, 74, 127);  // recenter CC74 timbre -> lowpass patch-open
                // ^ cutoffCents is sticky (only zeroed at channel creation), so a part that
                //   closed its filter mid-song would otherwise stay silent across a restart.
            }
        } else {
            tsf_channel_note_off_all(t, ch - 1);
        }
        AudioInterrupts();
    }

private:
    tsf **_t;
    bool  _mpe = false;   // false = normal GM playback (ignore file CC74); true = MPE timbre axis
    const float *_gmTrim = nullptr;   // Tier-2 per-GM-program trim table (128), null = off
    static uint8_t to7(float v) { return (uint8_t)((v < 0 ? 0 : v > 1 ? 1 : v) * 127.0f + 0.5f); }
};
