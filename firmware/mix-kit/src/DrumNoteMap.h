// DrumNoteMap — playback-time percussion note remap (Roland/TD-11 -> GM).
//
// Why this exists
// ---------------
// GMD (Groove MIDI Dataset) grooves were recorded on Roland electronic kits and
// carry the Roland/TD-11 percussion map, which places the closed/open hi-hat
// *edge* articulations on MIDI notes 22 and 26 -- BELOW General MIDI's percussion
// range (35..81). On a GM engine (TSF/SF2/OPL3/OPLL) nothing is mapped there, so
// those notes are dropped silently and the groove loses its hi-hat (often the
// primary timekeeping voice). Measured: 36% of the staged pack hit this, and note
// 22 is the 4th-most-common note pack-wide. See planning/drum-note-map/DESIGN.md.
//
// The card is the source of truth
// -------------------------------
// We DO NOT rewrite the .mid files (that would be lossy + one-way -- once the
// edge-hat distinction is gone from disk no future engine can recover it). Instead
// we keep the Roland note numbers untouched on the SD card and make the map
// decision HERE, at playback, where we know which engine/font is active and what
// it can render.
//
// Topology (mirrors the ArpFilter shim pattern)
// ---------------------------------------------
//   g_drumPlayer (ch10) --> DrumNoteMapper --> <real drum sink>
// DrumNoteMapper is itself a tdsp::MidiSink. Only channel-10 note-on/note-off are
// touched; velocity, timing, program change, and every other event/channel pass
// through verbatim, so dynamics and ghosting are preserved in every mode.

#pragma once

#include <stdint.h>
#include <stddef.h>

#include "MidiSink.h"

namespace tdsp {

// Roland/TD-11 -> General Midi percussion reduce table. Identity everywhere
// except the out-of-GM-range articulations GMD actually uses. Only 22 and 26
// occur in the current pack; both fold to their GM hi-hat equivalents. Any
// future out-of-range Roland note gets one more line here (and NOWHERE else) --
// this is the single source of truth for the reduce, so an offline tool could
// share it if we ever want one.
//   22 = Closed Hi-Hat (edge) -> 42 (Closed Hi-Hat)
//   26 = Open   Hi-Hat (edge) -> 46 (Open   Hi-Hat)
static constexpr uint8_t kDrumReduceToGm(uint8_t note) {
    return note == 22 ? 42
         : note == 26 ? 46
         : note;   // already valid GM (35..81) or an unmapped extra we leave alone
}

class DrumNoteMapper : public MidiSink {
public:
    enum Mode : uint8_t {
        // The active font renders the Roland map natively (regions at 22/26 and
        // the rest of the TD-11 map). Emit notes as-is -> authentic. This is the
        // PSRAM "V-Drums font" end-state (DESIGN.md 3.3).
        Passthrough = 0,
        // The active font is plain GM (TimGM6mb via TSF/SF2, OPL3, OPLL rhythm).
        // Collapse the Roland extras to GM so the groove is audible on any GM
        // font. Default -- correct for every current engine.
        GmReduce,
    };

    // The single downstream drum sink (g_synthSink / g_drumTsfSink /
    // g_drumVoiceSink). Set once in setup() where g_drumPlayer.setSink() ran.
    void setDownstream(MidiSink *sink) { _downstream = sink; }
    MidiSink *downstream() const { return _downstream; }

    void setMode(Mode m) { _mode = m; }
    Mode mode() const { return _mode; }

    // Map a channel-10 note through the current mode; identity otherwise. Static
    // + public so an off-target unit test can exercise the table without a sink.
    uint8_t mapNote(uint8_t channel, uint8_t note) const {
        return (channel == 10 && _mode == GmReduce) ? kDrumReduceToGm(note) : note;
    }

    // -------- MidiSink overrides --------
    // Only note on/off are remapped; note-off maps identically to note-on (the
    // reduce is a pure per-note function) so nothing can hang.
    void onNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) override {
        if (_downstream) _downstream->onNoteOn(channel, mapNote(channel, note), velocity);
    }
    void onNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) override {
        if (_downstream) _downstream->onNoteOff(channel, mapNote(channel, note), velocity);
    }

    // Everything else is a straight passthrough -- expression, kit program
    // change, panics, clock/transport -- so the shim is transparent apart from
    // the two hi-hat notes.
    void onPitchBend(uint8_t channel, float semitones) override { if (_downstream) _downstream->onPitchBend(channel, semitones); }
    void onTimbre(uint8_t channel, float value) override { if (_downstream) _downstream->onTimbre(channel, value); }
    void onPressure(uint8_t channel, float value) override { if (_downstream) _downstream->onPressure(channel, value); }
    void onModWheel(uint8_t channel, float value) override { if (_downstream) _downstream->onModWheel(channel, value); }
    void onSustain(uint8_t channel, bool on) override { if (_downstream) _downstream->onSustain(channel, on); }
    void onProgramChange(uint8_t channel, uint8_t program) override { if (_downstream) _downstream->onProgramChange(channel, program); }
    void onAllNotesOff(uint8_t channel) override { if (_downstream) _downstream->onAllNotesOff(channel); }
    void onSysEx(const uint8_t *data, size_t length, bool last) override { if (_downstream) _downstream->onSysEx(data, length, last); }
    void onClock() override { if (_downstream) _downstream->onClock(); }
    void onStart() override { if (_downstream) _downstream->onStart(); }
    void onContinue() override { if (_downstream) _downstream->onContinue(); }
    void onStop() override { if (_downstream) _downstream->onStop(); }

private:
    MidiSink *_downstream = nullptr;
    Mode      _mode       = GmReduce;   // safe default: audible on every current engine
};

}  // namespace tdsp
