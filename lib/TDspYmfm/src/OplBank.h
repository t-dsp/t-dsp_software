// OplBank.h — parser for the Doom GENMIDI (".op2") OPL instrument bank into
// register-ready patch structs, mirroring OpmBank.h's pure/no-I/O shape.
//
// The OP2 lump (magic "#OPL_II#") holds 175 fixed 36-byte instrument records
// followed by a 175-entry name table (32 bytes each). Indices 0..127 are the GM
// melodic programs; indices 128..174 are the percussion kit (MIDI notes 35..81,
// index = 128 + note - 35). Each record carries two 16-byte "voices" (the second
// is a detuned double-voice used when the double-voice flag is set); each voice is
// two OPL operators (modulator + carrier) plus a feedback/connection byte and a
// signed note offset.
//
// We pre-fold each operator's six GENMIDI fields into the five OPL register bytes
// the engine writes (0x20/0x40/0x60/0x80/0xE0), combining the key-scale-level and
// output-level bytes into the single 0x40 register. The parser does no I/O — hand
// it the whole lump in RAM (or the baked const array). WOPL is not handled here.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace tdsp {
namespace ymfmopl {

static constexpr int kOplNameLen     = 32;   // GENMIDI name field width (null-padded)
static constexpr int kOp2NumInst     = 175;  // fixed instrument count in an .op2 lump
static constexpr int kNumMelodic     = 128;  // GM programs 0..127
static constexpr int kNumPercussion  = 47;   // indices 128..174 (MIDI notes 35..81)

// One OPL 2-operator voice, pre-folded to register-ready bytes.
//   mod[]/car[] index -> OPL register:  0=0x20  1=0x40  2=0x60  3=0x80  4=0xE0
// The 0x40 byte already merges key-scale-level (bits 6-7) with total-level
// (bits 0-5); the engine re-derives the carrier TL at note-on for velocity.
struct OplPatch {
    uint8_t mod[5];        // modulator register bytes
    uint8_t car[5];        // carrier register bytes
    uint8_t feedbackConn;  // GENMIDI feedback/connection byte -> low nibble of 0xC0
    int16_t noteOffset;    // added to the played note for this voice
};

// One GENMIDI instrument record (melodic program or percussion note).
struct OplInstrument {
    char     name[kOplNameLen];  // null-terminated (from the name table)
    uint16_t flags;              // bit0 = fixed pitch, bit2 = double-voice
    uint8_t  fineTune;           // second-voice detune (128 = none)
    uint8_t  fixedNote;          // MIDI note for fixed-pitch/percussion voices
    OplPatch voice[2];           // primary + double voice
};

// Parse a Doom GENMIDI (.op2) lump into out[] (up to maxInst instruments).
// Returns the number parsed (kOp2NumInst on success) or -1 on a format error
// (bad magic or short buffer).
int parseOp2Bank(const uint8_t *data, size_t len, OplInstrument *out, int maxInst);

} // namespace ymfmopl
} // namespace tdsp
