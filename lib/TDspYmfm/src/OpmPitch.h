// OpmPitch.h — convert a (possibly fractional) MIDI note to an OPM key code +
// key fraction. Pure, dependency-free (no Arduino), so it is unit-testable on a
// host and shared by the engine's note-on and its per-note MPE pitch bend.
//
// OPM pitch is set per channel by two registers:
//   0x28+ch  KC  = (octave<<4) | note-code   — octave 0..7, note-code below
//   0x30+ch  KF  = fraction<<2               — 6-bit fine fraction of a semitone
//
// The note-code uses 12 of 16 values per octave, skipping 3/7/11/15, so the
// sequence low->high is 0,1,2, 4,5,6, 8,9,10, 12,13,14. A fractional MIDI note
// (e.g. from a pitch bend) puts the whole-semitone part in KC and the remaining
// fraction in KF (64 steps per semitone), which is exactly what per-note MPE bend
// needs. (Global concert-pitch alignment is a one-constant trim; monotonic and
// ~A440 is what matters, and this delivers that.)

#pragma once
#include <stdint.h>

namespace tdsp {
namespace ymfmopm {

// MIDI semitone (0=C..11=B) -> OPM key-code low nibble.
static const uint8_t kOpmNote[12] = { 0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14 };

// Encode `note` (MIDI, may be fractional) into OPM key code `kc` (7-bit) and key
// fraction `kf` (0..63, i.e. the value BEFORE the <<2 shift into register 0x30).
inline void encodeOpmPitch(float note, uint8_t &kc, uint8_t &kf) {
    if (note < 0.0f)   note = 0.0f;
    if (note > 127.0f) note = 127.0f;

    int   base = (int)note;             // note >= 0, so this is floor()
    float frac = note - (float)base;    // 0..1 within the semitone

    int oct  = base / 12 - 1;           // MIDI 60 -> OPM octave 4
    int semi = base % 12;

    if (oct < 0) { oct = 0; semi = 0;  frac = 0.0f; }   // clamp below range
    if (oct > 7) { oct = 7; semi = 11; frac = 0.0f; }   // clamp above range

    kc = (uint8_t)((oct << 4) | kOpmNote[semi]);

    int f = (int)(frac * 64.0f);
    if (f < 0)  f = 0;
    if (f > 63) f = 63;
    kf = (uint8_t)f;
}

} // namespace ymfmopm
} // namespace tdsp
