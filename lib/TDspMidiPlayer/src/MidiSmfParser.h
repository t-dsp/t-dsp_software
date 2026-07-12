// MidiSmfParser.h — runtime Standard MIDI File parser -> MidiFileEvent stream.
//
// Descendant of the mix-kit's sd_midi.h, upgraded for the synth-agnostic
// player. Where the old parser merged every track down to note+velocity on a
// single channel and threw away channel 10 (drums), CC, pitch-bend, and
// program-change, this one PRESERVES all of them:
//
//   * per-note MIDI channel (0..15) is kept, so a multi-timbral backend can
//     route each channel to its own patch (or a drum sink for channel 10);
//   * program-change (0xC0) events are emitted inline, in tick order, so the
//     backend can honor the file's instrument choices;
//   * CC (0xB0) and pitch-bend (0xE0) are emitted too;
//   * channel 10 (index 9, drums) is KEPT in the stream — filtering is a
//     playback POLICY (MidiFilePlayer::setChannelMask), not a parse decision.
//
// Still: merges all tracks, honors the tempo map, and is defensively
// bounds-checked so a truncated/garbage file returns -1 instead of faulting.
// parseSmf() is pure (buffer in, events out) with no SD dependency; the
// optional loadSmfFile() helper is compiled only where <SD.h> is included
// before this header.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include "MidiFileEvent.h"

namespace tdsp {
namespace smf {

static inline uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static inline uint16_t be16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }

// Variable-length quantity at d[*i]; advances *i. Stops at `end` for bounds.
static inline uint32_t readVar(const uint8_t *d, size_t end, size_t *i) {
    uint32_t v = 0;
    for (int k = 0; k < 4 && *i < end; ++k) {
        uint8_t c = d[(*i)++];
        v = (v << 7) | (c & 0x7F);
        if (!(c & 0x80)) break;
    }
    return v;
}

// Intermediate absolute-tick event (before delta encoding + sorting).
struct TickEv { uint32_t tick; uint8_t kind; uint8_t ch; uint8_t d1; uint8_t d2; };
struct Tempo  { uint32_t tick; uint32_t uspq; };

// Same-tick emission order: program change first (so a note at the same tick
// plays the new patch), then CC/bend, then note-off, then note-on (so a
// same-tick retrigger releases before it re-strikes).
static inline int sortRank(uint8_t kind) {
    switch (kind) {
        case kProgramChange: return 0;
        case kControlChange: return 1;
        case kPitchBend:     return 2;
        case kNoteOff:       return 3;
        case kNoteOn:        return 4;
        default:             return 5;
    }
}

// Parse buf[0..len) into out[0..maxOut). Returns event count, or -1 on error.
// Heap-allocates a scratch array sized to the note/controller count.
static int parseSmf(const uint8_t *d, size_t len, MidiFileEvent *out, int maxOut) {
    if (len < 14 || memcmp(d, "MThd", 4) != 0) return -1;
    uint16_t div = be16(d + 12);
    if (div == 0 || (div & 0x8000)) return -1;      // 0 or SMPTE division: unsupported
    const uint32_t tpq = div;

    // Locate MTrk chunks.
    const int MAXTRK = 32;
    size_t tS[MAXTRK], tE[MAXTRK]; int nt = 0;
    for (size_t p = 14; p + 8 <= len && nt < MAXTRK; ) {
        uint32_t tl = be32(d + p + 4);
        if (memcmp(d + p, "MTrk", 4) != 0) break;
        size_t e = p + 8 + tl;
        if (e > len) e = len;                       // clamp a lying length
        tS[nt] = p + 8; tE[nt] = e; nt++;
        p = e;
    }
    if (nt == 0) return -1;

    // Pass 1: tempo map across all tracks (set-tempo meta FF 51 03).
    const int MAXTEMPO = 128;
    Tempo tempos[MAXTEMPO]; int ntempo = 0;
    for (int t = 0; t < nt; ++t) {
        size_t i = tS[t], e = tE[t]; uint32_t tick = 0; uint8_t status = 0;
        while (i < e) {
            tick += readVar(d, e, &i);
            if (i >= e) break;
            uint8_t b0 = d[i];
            if (b0 & 0x80) { status = b0; i++; }
            if (status == 0xFF) {
                if (i >= e) break;
                uint8_t meta = d[i++]; uint32_t ml = readVar(d, e, &i);
                if (meta == 0x51 && ml == 3 && i + 3 <= e && ntempo < MAXTEMPO) {
                    tempos[ntempo].tick = tick;
                    tempos[ntempo].uspq = ((uint32_t)d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
                    ntempo++;
                }
                i += ml;
            } else if (status == 0xF0 || status == 0xF7) {
                uint32_t sl = readVar(d, e, &i); i += sl;
            } else {
                uint8_t hi = status & 0xF0;
                i += (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
            }
        }
    }
    if (ntempo == 0) { tempos[0] = {0, 500000}; ntempo = 1; }       // default 120 bpm
    std::stable_sort(tempos, tempos + ntempo, [](const Tempo &a, const Tempo &b) { return a.tick < b.tick; });

    auto tickToMs = [&](uint32_t tk) -> double {
        double ms = 0; uint32_t last = 0, uspq = tempos[0].uspq;
        for (int a = 0; a < ntempo; ++a) {
            if (tempos[a].tick >= tk) break;
            ms += (double)(tempos[a].tick - last) * ((double)uspq / tpq) / 1000.0;
            last = tempos[a].tick; uspq = tempos[a].uspq;
        }
        return ms + (double)(tk - last) * ((double)uspq / tpq) / 1000.0;
    };

    // Pass 2a: count channel-voice events we will keep (note/CC/bend/program,
    // all 16 channels). Meta and sysex are skipped.
    int total = 0;
    for (int t = 0; t < nt; ++t) {
        size_t i = tS[t], e = tE[t]; uint8_t status = 0;
        while (i < e) {
            readVar(d, e, &i); if (i >= e) break;
            uint8_t b0 = d[i]; if (b0 & 0x80) { status = b0; i++; }
            if (status == 0xFF) { if (i >= e) break; i++; uint32_t ml = readVar(d, e, &i); i += ml; }
            else if (status == 0xF0 || status == 0xF7) { uint32_t sl = readVar(d, e, &i); i += sl; }
            else {
                uint8_t hi = status & 0xF0;
                if (hi == 0x80 || hi == 0x90 || hi == 0xB0 || hi == 0xC0 || hi == 0xE0) total++;
                i += (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
            }
        }
    }
    if (total <= 0) return -1;
    TickEv *ev = (TickEv *)malloc(sizeof(TickEv) * (size_t)total);
    if (!ev) return -1;

    // Pass 2b: collect events with absolute ticks.
    int ne = 0;
    for (int t = 0; t < nt && ne < total; ++t) {
        size_t i = tS[t], e = tE[t]; uint32_t tick = 0; uint8_t status = 0;
        while (i < e && ne < total) {
            tick += readVar(d, e, &i); if (i >= e) break;
            uint8_t b0 = d[i]; if (b0 & 0x80) { status = b0; i++; }
            if (status == 0xFF) { if (i >= e) break; i++; uint32_t ml = readVar(d, e, &i); i += ml; }
            else if (status == 0xF0 || status == 0xF7) { uint32_t sl = readVar(d, e, &i); i += sl; }
            else {
                uint8_t hi = status & 0xF0, ch = status & 0x0F;
                if (hi == 0xC0 || hi == 0xD0) {                 // 1 data byte
                    if (i + 1 > e) break;
                    uint8_t d1 = d[i]; i += 1;
                    if (hi == 0xC0) ev[ne++] = {tick, kProgramChange, ch, d1, 0};
                } else {                                        // 2 data bytes
                    if (i + 2 > e) break;
                    uint8_t d1 = d[i], d2 = d[i + 1]; i += 2;
                    if (hi == 0x90)      ev[ne++] = {tick, (uint8_t)(d2 ? kNoteOn : kNoteOff), ch, d1, d2};
                    else if (hi == 0x80) ev[ne++] = {tick, kNoteOff, ch, d1, d2};
                    else if (hi == 0xB0) ev[ne++] = {tick, kControlChange, ch, d1, d2};
                    else if (hi == 0xE0) ev[ne++] = {tick, kPitchBend, ch, d1, d2};
                    // 0xA0 (poly aftertouch) intentionally dropped.
                }
            }
        }
    }

    // Stable sort by (tick, sortRank) so same-tick ordering is deterministic.
    std::stable_sort(ev, ev + ne, [](const TickEv &a, const TickEv &b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        return sortRank(a.kind) < sortRank(b.kind);
    });

    // Emit delta-ms stream; split gaps > 60000 ms into kRest padding.
    int no = 0; uint32_t prevMs = 0;
    for (int k = 0; k < ne && no < maxOut; ++k) {
        uint32_t absMs = (uint32_t)(tickToMs(ev[k].tick) + 0.5);
        uint32_t dd = (absMs > prevMs) ? (absMs - prevMs) : 0; prevMs = absMs;
        while (dd > 60000 && no < maxOut) { out[no++] = {60000, kRest, 0, 0, 0}; dd -= 60000; }
        if (no < maxOut) out[no++] = {(uint16_t)dd, ev[k].kind, ev[k].ch, ev[k].d1, ev[k].d2};
    }
    free(ev);
    return no;
}

} // namespace smf
} // namespace tdsp
