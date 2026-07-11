// OpmVoice.h — a plain-data OPM (YM2151) patch and a couple of starter presets.
//
// The YM2151 has 8 channels, each with 4 operators wired by one of 8 algorithms.
// A "voice" here is the per-channel + per-operator register set that defines a
// timbre, independent of pitch/velocity (those are applied per note-on). This is
// the format ymfm knows nothing about — ymfm is a register-level chip emulator;
// turning it into a playable instrument means writing these fields into the chip.
//
// Field ranges mirror the OPM register map (see ymfm_opm.h):
//   alg 0-7, feedback 0-7 (operator-1 self-feedback)
//   per op: dt1 0-7, mul 0-15, tl 0-127 (0=loudest), ks 0-3, ar 0-31,
//           ams 0/1 (LFO-AM enable), d1r 0-31, dt2 0-3, d2r 0-31, sl 0-15, rr 0-15
//
// Operator index 0..3 maps to register slots ch + 0/8/16/24. For the additive
// algorithm 7 (all four operators are carriers) the slot->M/C ordering doesn't
// matter, which is why the default preset uses alg 7 — it is guaranteed to make
// sound regardless of operator-order conventions. Richer FM presets that rely on
// modulator routing can be added once the signal path is proven.

#pragma once
#include <stdint.h>

namespace tdsp {
namespace ymfmopm {

struct OpmOp {
    uint8_t dt1, mul;   // 0x40: detune-1 (0-7), frequency multiple (0-15)
    uint8_t tl;         // 0x60: total level (0-127, 0 = loudest)
    uint8_t ks, ar;     // 0x80: key-scale (0-3), attack rate (0-31)
    uint8_t ams, d1r;   // 0xA0: LFO-AM enable (0/1), first decay rate (0-31)
    uint8_t dt2, d2r;   // 0xC0: detune-2 (0-3), second decay/sustain rate (0-31)
    uint8_t sl, rr;     // 0xE0: sustain level (0-15), release rate (0-15)
};

struct OpmVoice {
    const char *name;
    uint8_t alg;        // 0-7 operator connection algorithm
    uint8_t feedback;   // 0-7 operator-1 self-feedback
    OpmOp   op[4];      // register slots ch+0, ch+8, ch+16, ch+24
};

// --- Preset 0: "Additive Organ" ------------------------------------------------
// Algorithm 7: all four operators are carriers summed to the output. Two ops sit
// at dt1=0 and two at dt1=3/6 so the sum beats slightly (a gentle chorus), giving
// a fuller tone than a single sine while staying robust to operator ordering.
// D1R=0 holds at TL for the length of the key press (organ-style sustain).
static const OpmVoice kAdditiveOrgan = {
    "Additive Organ", /*alg=*/7, /*fb=*/0,
    {   //  dt1 mul   tl   ks ar   ams d1r   dt2 d2r   sl rr
        {   0,  1,  0x18,   0, 31,   0,  0,    0,  0,    0, 7 },
        {   3,  1,  0x18,   0, 31,   0,  0,    0,  0,    0, 7 },
        {   0,  2,  0x20,   0, 31,   0,  0,    0,  0,    0, 7 },
        {   6,  1,  0x1C,   0, 31,   0,  0,    0,  0,    0, 7 },
    }
};

// --- Preset 1: "E.Piano-ish" ---------------------------------------------------
// Algorithm 5: operator 1 (slot 0) modulates carriers, giving a struck-tine-like
// attack with a fast-decaying bright partial over a mellow body. This DOES depend
// on operator routing, so treat its exact timbre as approximate until validated
// on hardware; it is here to show a modulated (true FM) patch alongside the safe
// additive default.
static const OpmVoice kElectricPiano = {
    "E.Piano", /*alg=*/5, /*fb=*/5,
    {   //  dt1 mul   tl   ks ar   ams d1r   dt2 d2r   sl rr
        {   0,  1,  0x20,   1, 31,   0, 12,    0,  6,    3, 8 },  // modulator: bright, decays
        {   0,  1,  0x10,   1, 31,   0,  8,    0,  4,    2, 8 },  // carrier
        {   0, 14,  0x28,   1, 31,   0, 14,    0,  6,    4, 8 },  // modulator: metallic partial
        {   0,  1,  0x14,   1, 31,   0,  6,    0,  3,    2, 8 },  // carrier
    }
};

} // namespace ymfmopm
} // namespace tdsp
