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

struct Tempo  { uint32_t tick; uint32_t uspq; };

// Same-tick emission order: program change first (so a note at the same tick
// plays the new patch), then CC/bend, then note-off, then note-on (so a
// same-tick retrigger releases before it re-strikes).
static inline int sortRank(uint8_t kind) {
    switch (kind) {
        case kProgramChange: return 0;
        case kControlChange: return 1;
        case kPitchBend:     return 2;
        case kChannelPressure: return 2;
        case kNoteOff:       return 3;
        case kNoteOn:        return 4;
        default:             return 5;
    }
}

// The file's INITIAL tempo in BPM — the set-tempo meta (FF 51 03) at the lowest
// tick across all tracks, or 120 if none. Lightweight (scans meta only); used by
// the mix-kit to lock a drum groove's tempo to a playing song (song = master).
static inline float initialBpm(const uint8_t *d, size_t len) {
    if (len < 14 || memcmp(d, "MThd", 4) != 0) return 120.0f;
    uint32_t bestTick = 0xFFFFFFFFu, bestUspq = 500000; // default 120 bpm
    for (size_t p = 14; p + 8 <= len; ) {
        if (memcmp(d + p, "MTrk", 4) != 0) break;
        uint32_t tl = be32(d + p + 4);
        size_t i = p + 8, e = i + tl; if (e > len) e = len;
        uint32_t tick = 0; uint8_t status = 0;
        while (i < e) {
            tick += readVar(d, e, &i); if (i >= e) break;
            uint8_t b0 = d[i]; if (b0 & 0x80) { status = b0; i++; }
            if (status == 0xFF) {
                if (i >= e) break;
                uint8_t meta = d[i++]; uint32_t ml = readVar(d, e, &i);
                if (meta == 0x51 && ml == 3 && i + 3 <= e && tick < bestTick) {
                    bestTick = tick;
                    bestUspq = ((uint32_t)d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
                }
                i += ml;
            } else if (status == 0xF0 || status == 0xF7) {
                uint32_t sl = readVar(d, e, &i); i += sl;
            } else {
                uint8_t hi = status & 0xF0; i += (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
            }
        }
        p = e;
    }
    if (bestUspq == 0) bestUspq = 500000;
    return 60000000.0f / (float)bestUspq;
}

// The file's INITIAL time signature as QUARTER-NOTE beats per bar (the Clock's
// beat is one quarter note). From the time-signature meta (FF 58 04 nn dd cc bb)
// at the lowest tick: beats = nn * 4 / (2^dd) — e.g. 4/4->4, 3/4->3, 2/4->2,
// 2/2->4, 6/8->3, 9/8->4 (rounds off? no — see below), 12/8->6. Returns 4
// (common time) if there is no meta, and also for meters that don't land on a
// whole number of quarter beats (e.g. 7/8, 5/8): those keep 4/4 bar edges rather
// than drift. Lightweight (scans meta only); used to set the master bar length so
// beatInBar()/barPhase()/the downbeat are correct for non-4/4 content.
static inline uint8_t initialBeatsPerBar(const uint8_t *d, size_t len) {
    if (len < 14 || memcmp(d, "MThd", 4) != 0) return 4;
    uint32_t bestTick = 0xFFFFFFFFu; uint8_t num = 0, denPow = 0;
    for (size_t p = 14; p + 8 <= len; ) {
        if (memcmp(d + p, "MTrk", 4) != 0) break;
        uint32_t tl = be32(d + p + 4);
        size_t i = p + 8, e = i + tl; if (e > len) e = len;
        uint32_t tick = 0; uint8_t status = 0;
        while (i < e) {
            tick += readVar(d, e, &i); if (i >= e) break;
            uint8_t b0 = d[i]; if (b0 & 0x80) { status = b0; i++; }
            if (status == 0xFF) {
                if (i >= e) break;
                uint8_t meta = d[i++]; uint32_t ml = readVar(d, e, &i);
                if (meta == 0x58 && ml >= 2 && i + 2 <= e && tick < bestTick) {
                    bestTick = tick; num = d[i]; denPow = d[i + 1];
                }
                i += ml;
            } else if (status == 0xF0 || status == 0xF7) {
                uint32_t sl = readVar(d, e, &i); i += sl;
            } else {
                uint8_t hi = status & 0xF0; i += (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
            }
        }
        p = e;
    }
    if (num == 0 || denPow > 7) return 4;                  // no meta / absurd denominator
    uint32_t den = 1u << denPow;                           // 2^dd (note value: 4=quarter, 8=eighth)
    uint32_t q = (uint32_t)num * 4u / den;                 // quarter-note beats per bar
    if (q * den != (uint32_t)num * 4u) return 4;           // not a whole # of quarters (7/8, 5/8)
    if (q < 1 || q > 32) return 4;
    return (uint8_t)q;
}

// The file's LOOP LENGTH in quarter-note beats (Clock beats): the highest
// absolute tick reached across all tracks — the end-of-track boundary — divided
// by the header's ticks-per-quarter (division). EXACT and FRACTIONAL: a 4/4 bar
// returns 4.0, a 6/8 bar 3.0, a 7/8 bar 3.5, four bars of 4/4 = 16.0. A
// tick-synced player wraps on this true musical length with zero rounding, so it
// never drifts — see planning/tick-sync-playback/PLAN.md §2/§3. Meter-agnostic
// (no time-sig needed; works on GMD grooves that omit it). Returns 0.0 when the
// file is unparseable/empty so the caller can fall back to an ms-derived length.
static inline double initialLoopBeats(const uint8_t *d, size_t len) {
    if (len < 14 || memcmp(d, "MThd", 4) != 0) return 0.0;
    uint16_t div = be16(d + 12);
    if (div == 0 || (div & 0x8000)) return 0.0;            // 0 or SMPTE division
    uint32_t maxTick = 0;
    for (size_t p = 14; p + 8 <= len; ) {
        if (memcmp(d + p, "MTrk", 4) != 0) break;
        uint32_t tl = be32(d + p + 4);
        size_t i = p + 8, e = i + tl; if (e > len) e = len;
        uint32_t tick = 0; uint8_t status = 0;
        while (i < e) {
            tick += readVar(d, e, &i); if (i >= e) break;
            uint8_t b0 = d[i]; if (b0 & 0x80) { status = b0; i++; }
            if (status == 0xFF) {
                if (i >= e) break;
                (void)d[i++];                              // meta type (unused)
                uint32_t ml = readVar(d, e, &i); i += ml;
            } else if (status == 0xF0 || status == 0xF7) {
                uint32_t sl = readVar(d, e, &i); i += sl;
            } else {
                uint8_t hi = status & 0xF0; i += (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
            }
        }
        if (tick > maxTick) maxTick = tick;
        p = e;
    }
    if (maxTick == 0) return 0.0;
    return (double)maxTick / (double)div;
}

// Snap a loop length derived from a LOSSY source (integer-ms rounding) to the
// nearest eighth note (0.5 beat) — absorbs tick-quantization noise while still
// permitting fractional bars (7/8 = 3.5, 5/8 = 2.5). Do NOT use on the exact
// parse path: initialLoopBeats() is already exact. See PLAN §2.
static inline double snapLoopBeatsHalf(double beats) {
    if (beats <= 0.0) return 0.0;
    double snapped = (double)(long)(beats * 2.0 + 0.5) / 2.0;   // round-to-nearest 0.5
    return snapped > 0.0 ? snapped : 0.5;
}

// Parse buf[0..len) into out[0..maxOut). Returns event count, or -1 on error.
// Streams the tracks with a k-way merge (no large heap scratch) — see below.
// NOTE: writes into out[] while reading d[], so d and out MUST NOT alias.
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

    // Streaming k-way merge across tracks. Every track is already monotonic in
    // absolute tick, so we keep one small cursor per track (O(nt) state, nt<=32)
    // and repeatedly emit the globally-earliest pending event — instead of
    // collecting ALL events into a heap array and sorting them. That old approach
    // needed a scratch block of 8*total bytes: ~121 KB for a dense piano sonata
    // (~15k events), which malloc() CANNOT satisfy on a no-PSRAM board whose OCRAM
    // heap is only ~80 KB after DMAMEM — so dense SD songs silently failed to load
    // (parse returned -1). The merge needs no large allocation; it streams straight
    // into out[] (hence d and out must not alias — the caller reads the file into a
    // separate buffer).
    //
    // Ordering: within a track, events keep their file order (which the exporter
    // already writes release-before-retrigger / program-before-note); across tracks
    // a same-tick tie breaks by sortRank() then track index. This differs from the
    // old whole-song stable_sort only for same-tick events INSIDE one track that the
    // file happened to write out of sortRank order (rare, and keeping the file's own
    // order is the safer reading — the sort could otherwise reorder a note-off ahead
    // of the note-on the file intended first).
    struct Cursor {
        size_t i, e;        // read position and track end
        uint32_t tick;      // absolute tick of the pending event
        uint8_t status;     // running status
        bool has;           // a decoded channel-voice event is pending?
        uint8_t kind, ch, d1, d2;
    };
    Cursor cur[MAXTRK];

    // Decode the next KEPT channel-voice event from track c (skipping meta, sysex
    // and poly-aftertouch), accumulating delta-ticks. Sets c.has=false at track end.
    auto advance = [&](Cursor &c) {
        while (c.i < c.e) {
            c.tick += readVar(d, c.e, &c.i);
            if (c.i >= c.e) break;
            uint8_t b0 = d[c.i]; if (b0 & 0x80) { c.status = b0; c.i++; }
            if (c.status == 0xFF) { if (c.i >= c.e) break; c.i++; uint32_t ml = readVar(d, c.e, &c.i); c.i += ml; }
            else if (c.status == 0xF0 || c.status == 0xF7) { uint32_t sl = readVar(d, c.e, &c.i); c.i += sl; }
            else {
                uint8_t hi = c.status & 0xF0, ch = c.status & 0x0F;
                if (hi == 0xC0 || hi == 0xD0) {                 // 1 data byte
                    if (c.i + 1 > c.e) break;
                    uint8_t d1 = d[c.i]; c.i += 1;
                    c.kind = (hi == 0xC0) ? kProgramChange : kChannelPressure;   // 0xD0 = MPE Z-axis
                    c.ch = ch; c.d1 = d1; c.d2 = 0; c.has = true; return;
                } else {                                        // 2 data bytes
                    if (c.i + 2 > c.e) break;
                    uint8_t d1 = d[c.i], d2 = d[c.i + 1]; c.i += 2;
                    if      (hi == 0x90) { c.kind = d2 ? kNoteOn : kNoteOff; c.ch = ch; c.d1 = d1; c.d2 = d2; c.has = true; return; }
                    else if (hi == 0x80) { c.kind = kNoteOff;       c.ch = ch; c.d1 = d1; c.d2 = d2; c.has = true; return; }
                    else if (hi == 0xB0) { c.kind = kControlChange; c.ch = ch; c.d1 = d1; c.d2 = d2; c.has = true; return; }
                    else if (hi == 0xE0) { c.kind = kPitchBend;     c.ch = ch; c.d1 = d1; c.d2 = d2; c.has = true; return; }
                    // 0xA0 (poly aftertouch) and anything else: consumed, keep scanning.
                }
            }
        }
        c.has = false;
    };

    for (int t = 0; t < nt; ++t) { cur[t] = {tS[t], tE[t], 0, 0, false, 0, 0, 0, 0}; advance(cur[t]); }

    // Emit the delta-ms stream; split gaps > 60000 ms into kRest padding.
    int no = 0, emitted = 0; uint32_t prevMs = 0;
    for (;;) {
        int m = -1;                                  // track whose pending event is earliest
        for (int t = 0; t < nt; ++t) {
            if (!cur[t].has) continue;
            if (m < 0) { m = t; continue; }
            if (cur[t].tick != cur[m].tick) { if (cur[t].tick < cur[m].tick) m = t; }
            else if (sortRank(cur[t].kind) < sortRank(cur[m].kind)) m = t;   // ties keep lower track index
        }
        if (m < 0) break;                            // all tracks drained
        uint32_t absMs = (uint32_t)(tickToMs(cur[m].tick) + 0.5);
        uint32_t dd = (absMs > prevMs) ? (absMs - prevMs) : 0; prevMs = absMs;
        while (dd > 60000 && no < maxOut) { out[no++] = {60000, kRest, 0, 0, 0}; dd -= 60000; }
        if (no >= maxOut) break;                      // out buffer full
        out[no++] = {(uint16_t)dd, cur[m].kind, cur[m].ch, cur[m].d1, cur[m].d2};
        emitted++;
        advance(cur[m]);
    }
    if (emitted == 0) return -1;
    return no;
}

} // namespace smf
} // namespace tdsp
