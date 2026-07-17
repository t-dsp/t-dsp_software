// OplBank.cpp — see OplBank.h. Allocation-free Doom GENMIDI (.op2) parser.

#include "OplBank.h"
#include <string.h>

// FLASHMEM keeps this cold parser out of ITCM on Teensy 4; harmless (no-op) when
// building host-side or on cores that don't define it.
#ifndef FLASHMEM
#define FLASHMEM
#endif

namespace tdsp {
namespace ymfmopl {

// GENMIDI 16-byte voice layout (per record, two of these):
//   [0] modulator 0x20 (trem/vib/sus/KSR/mult)
//   [1] modulator 0x60 (attack/decay)
//   [2] modulator 0x80 (sustain/release)
//   [3] modulator 0xE0 (waveform select)
//   [4] modulator key-scale-level (bits 6-7 of 0x40)
//   [5] modulator output-level    (bits 0-5 of 0x40)
//   [6] feedback/connection -> 0xC0
//   [7..12] carrier: same six fields in the same order
//   [13] unused
//   [14..15] int16 LE note offset
// Cold path: parsing runs once at begin() (or on an SD bank load), so keep it in
// flash rather than the scarce ITCM.
FLASHMEM static void parseVoice(const uint8_t *v, OplPatch &p) {
    // modulator
    p.mod[0] = v[0];                             // 0x20
    p.mod[1] = (uint8_t)((v[4] & 0xC0) | (v[5] & 0x3F));   // 0x40 = KSL | TL
    p.mod[2] = v[1];                             // 0x60
    p.mod[3] = v[2];                             // 0x80
    p.mod[4] = (uint8_t)(v[3] & 0x07);           // 0xE0 (waveform 0..7)
    // carrier
    p.car[0] = v[7];
    p.car[1] = (uint8_t)((v[11] & 0xC0) | (v[12] & 0x3F));
    p.car[2] = v[8];
    p.car[3] = v[9];
    p.car[4] = (uint8_t)(v[10] & 0x07);
    p.feedbackConn = v[6];
    p.noteOffset = (int16_t)((uint16_t)v[14] | ((uint16_t)v[15] << 8));
}

FLASHMEM int parseOp2Bank(const uint8_t *data, size_t len, OplInstrument *out, int maxInst) {
    if (!data || !out || maxInst <= 0) return -1;

    // 8-byte magic + 175*36 records + 175*32 names = 11908 bytes minimum.
    static const uint8_t kMagic[8] = { '#','O','P','L','_','I','I','#' };
    const size_t kRecs  = kOp2NumInst;
    const size_t kNeed  = 8 + kRecs * 36 + kRecs * 32;
    if (len < kNeed) return -1;
    if (memcmp(data, kMagic, 8) != 0) return -1;

    const uint8_t *rec  = data + 8;                    // instrument records
    const uint8_t *name = data + 8 + kRecs * 36;       // name table

    int count = (int)kRecs;
    if (count > maxInst) count = maxInst;

    for (int i = 0; i < count; i++) {
        const uint8_t *r = rec + (size_t)i * 36;
        OplInstrument &ins = out[i];

        ins.flags     = (uint16_t)((uint16_t)r[0] | ((uint16_t)r[1] << 8));
        ins.fineTune  = r[2];
        ins.fixedNote = r[3];
        parseVoice(r + 4,  ins.voice[0]);
        parseVoice(r + 20, ins.voice[1]);

        // Name string: 32 bytes, null-padded. Copy safely and terminate.
        const uint8_t *nm = name + (size_t)i * 32;
        memcpy(ins.name, nm, kOplNameLen);
        ins.name[kOplNameLen - 1] = 0;
    }
    return count;
}

} // namespace ymfmopl
} // namespace tdsp
