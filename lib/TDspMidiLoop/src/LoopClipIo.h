// LoopClipIo.h — serialize / parse a LoopClip to the wire format the note editor uses.
//
// The byte layout is the contract shared with app/tdsp-control/src/loopClip.ts
// (planning/midi-editor/DESIGN.md §4.4). Little-endian on both ends (ARM + x86):
//
//   LoopClipHdr (12 bytes): 'T','L','C','1'  u16 count  u16 loopTicks  u8 bpb  u8 bars  u16 reserved
//   then count * LoopEvent (8 bytes each): u16 tick  u8 type  u8 channel  u8 d1  u8 d2  i16 bend
//
// The dump side streams bytes on the fly (no whole-clip buffer — DESIGN §4.1/§9.3); this
// header just provides the header codec + a byte-at-index accessor + a per-event reader so
// both directions share one definition of the format.
#pragma once
#include <stdint.h>
#include "LoopEvent.h"

namespace tdsp {

static constexpr uint32_t kLoopClipHdrBytes = 12;
static constexpr uint32_t kLoopEventBytes   = 8;

// Total serialized size of a clip.
inline uint32_t loopClipBytes(const LoopClip &c) {
    return kLoopClipHdrBytes + (uint32_t)c.count * kLoopEventBytes;
}

// Fill the 12-byte header for a clip.
inline void writeLoopClipHdr(const LoopClip &c, uint8_t hdr[12]) {
    hdr[0] = 'T'; hdr[1] = 'L'; hdr[2] = 'C'; hdr[3] = '1';
    hdr[4] = (uint8_t)(c.count & 0xff);      hdr[5] = (uint8_t)(c.count >> 8);
    hdr[6] = (uint8_t)(c.loopTicks & 0xff);  hdr[7] = (uint8_t)(c.loopTicks >> 8);
    hdr[8] = c.beatsPerBar; hdr[9] = c.bars; hdr[10] = 0; hdr[11] = 0;
}

// One byte of the serialized clip at absolute index `i` (header, then events). Lets the
// dump path stream a clip through a small framing window with no full-clip scratch buffer.
inline uint8_t loopClipByteAt(const LoopClip &c, uint32_t i) {
    if (i < kLoopClipHdrBytes) {
        uint8_t hdr[12]; writeLoopClipHdr(c, hdr); return hdr[i];
    }
    const uint32_t rel = i - kLoopClipHdrBytes;
    const LoopEvent &e = c.ev[rel / kLoopEventBytes];
    switch (rel % kLoopEventBytes) {
        case 0: return (uint8_t)(e.tick & 0xff);
        case 1: return (uint8_t)(e.tick >> 8);
        case 2: return e.type;
        case 3: return e.channel;
        case 4: return e.d1;
        case 5: return e.d2;
        case 6: return (uint8_t)((uint16_t)e.bend & 0xff);
        default: return (uint8_t)((uint16_t)e.bend >> 8);
    }
}

// Decode the 12-byte header. Returns false on bad magic. `count` may exceed the buffer or the
// cap — the caller validates those against the announced length / kMaxEvents.
inline bool readLoopClipHdr(const uint8_t *in, uint16_t &count, uint16_t &loopTicks,
                            uint8_t &bpb, uint8_t &bars) {
    if (in[0] != 'T' || in[1] != 'L' || in[2] != 'C' || in[3] != '1') return false;
    count     = (uint16_t)(in[4] | (in[5] << 8));
    loopTicks = (uint16_t)(in[6] | (in[7] << 8));
    bpb  = in[8];
    bars = in[9];
    return true;
}

// Read one 8-byte event from a raw event record (points at the event, not the header).
inline LoopEvent readLoopEvent(const uint8_t *p) {
    LoopEvent e;
    e.tick    = (uint16_t)(p[0] | (p[1] << 8));
    e.type    = p[2];
    e.channel = p[3];
    e.d1      = p[4];
    e.d2      = p[5];
    e.bend    = (int16_t)(uint16_t)(p[6] | (p[7] << 8));
    return e;
}

}  // namespace tdsp
