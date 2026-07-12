// MidiFileEvent.h — one packed event of a parsed/transcoded MIDI song.
//
// This is the synth-agnostic interchange format the player consumes. Unlike
// the older `SongEv {dms,note,vel}` (note+velocity only, single-timbre), this
// carries the MIDI CHANNEL and PROGRAM CHANGES, plus the common continuous
// controllers, so a multi-timbral backend (per-channel patches, drums) can be
// driven faithfully — and so program-change / velocity / pitch-bend survive the
// trip from file to synth. See lib/TDspMidiPlayer/README.md.
//
// The player maps `channel` (0-based here) onto tdsp::MidiSink's 1-based
// channel convention at dispatch time.
#pragma once
#include <stdint.h>

namespace tdsp {

enum MidiFileEventKind : uint8_t {
    kNoteOff       = 0,   // data1 = note,       data2 = release velocity
    kNoteOn        = 1,   // data1 = note,       data2 = velocity (1..127)
    kProgramChange = 2,   // data1 = program (0..127)
    kControlChange = 3,   // data1 = controller, data2 = value (mapped by the player)
    kPitchBend     = 4,   // data1 = LSB (0..127), data2 = MSB (0..127); center = 8192
    kRest          = 5,   // padding for gaps > 60000 ms; only deltaMs is meaningful
};

// 6 bytes, naturally aligned (no padding). At ~24k events that is ~144 KB, so
// keep the play buffer in OCRAM (DMAMEM) off the DTCM budget.
struct MidiFileEvent {
    uint16_t deltaMs;   // ms since the previous event
    uint8_t  kind;      // MidiFileEventKind
    uint8_t  channel;   // 0..15 (0-based); ignored for kRest
    uint8_t  data1;
    uint8_t  data2;
};

} // namespace tdsp
