// LoopEvent.h — one captured MIDI event, positioned within a loop.
//
// The recorder captures at the tdsp::MidiSink seam (downstream of the arp), so
// values are already NORMALIZED the way a sink wants them: pitch bend is float
// semitones, timbre/pressure/modwheel are 0..1. We keep them compact:
//   * bend  -> int16 semitones*256 (±128 semitones, 1/256 st resolution)
//   * 0..1  -> uint8 value*255 (d2)
//   * sustain-> d2 0/1
// so an event is a flat 8 bytes and a whole clip lives in a fixed static array
// (no heap — this platform has no PSRAM and a tight DTCM budget).
//
// `tick` is a 24-PPQN offset from the loop's downbeat, always in [0, loopTicks).
// A note held across the loop seam has its note-OFF stored at the WRAPPED tick
// (release beat - loopBeats), so the tail "rings" into the top of the loop.
#pragma once
#include <stdint.h>

namespace tdsp {

enum LoopEvType : uint8_t {
    LNoteOff   = 0,   // d1=note, d2=release velocity
    LAllOff    = 1,   // channel-wide release (d1/d2 unused)
    LPitchBend = 2,   // bend = semitones*256
    LTimbre    = 3,   // d2 = value*255 (CC#74)
    LPressure  = 4,   // d2 = value*255 (channel pressure)
    LModWheel  = 5,   // d2 = value*255 (CC#1)
    LSustain   = 6,   // d2 = 0/1 (CC#64)
    LProgram   = 7,   // d1 = program
    LNoteOn    = 8,   // d1=note, d2=velocity 1..127
};

// Same-tick ordering: releases + expression BEFORE attacks, so a wrapped
// note-off never lands after the next iteration's note-on at an equal tick.
static inline uint8_t loopTypeRank(uint8_t type) {
    return (type == LNoteOn) ? 2 : (type == LNoteOff || type == LAllOff) ? 0 : 1;
}

struct LoopEvent {
    uint16_t tick;     // 24-PPQN offset within the loop [0, loopTicks)
    uint8_t  type;     // LoopEvType
    uint8_t  channel;  // 1..16 (MidiSink convention)
    uint8_t  d1;       // note / program
    uint8_t  d2;       // velocity / value*255 / sustain
    int16_t  bend;     // semitones*256 (LPitchBend only)
};  // 8 bytes

// A recorded loop: fixed-capacity, kept sorted by (tick, type-rank).
struct LoopClip {
    static constexpr uint16_t kMaxEvents = 1024;   // 8 KB per clip

    LoopEvent ev[kMaxEvents];
    uint16_t  count       = 0;
    uint32_t  loopTicks   = 0;   // loop length in 24-PPQN ticks (loopBeats*24)
    uint8_t   beatsPerBar = 4;
    uint8_t   bars        = 4;

    bool hasData()  const { return count > 0 && loopTicks > 0; }
    bool full()     const { return count >= kMaxEvents; }
    void clear()          { count = 0; loopTicks = 0; }

    static uint32_t keyOf(const LoopEvent &e) {
        return ((uint32_t)e.tick << 3) | loopTypeRank(e.type);
    }

    // Insert keeping the array sorted by (tick, rank). Cheap in the common
    // case (event at/after the end -> O(1) push); binary search otherwise.
    // Returns false if the clip is full (event dropped).
    bool insert(const LoopEvent &e) {
        if (count >= kMaxEvents) return false;
        const uint32_t key = keyOf(e);
        if (count == 0 || keyOf(ev[count - 1]) <= key) {
            ev[count++] = e;                       // fast path: append at end
            return true;
        }
        uint16_t lo = 0, hi = count;               // first element with key > this one
        while (lo < hi) {
            uint16_t mid = (uint16_t)((lo + hi) >> 1);
            if (keyOf(ev[mid]) <= key) lo = (uint16_t)(mid + 1); else hi = mid;
        }
        for (uint16_t i = count; i > lo; --i) ev[i] = ev[i - 1];
        ev[lo] = e;
        count++;
        return true;
    }
};

}  // namespace tdsp
