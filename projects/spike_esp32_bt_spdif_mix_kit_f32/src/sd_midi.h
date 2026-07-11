// sd_midi.h — runtime Standard MIDI File parser + SD song library.
//
// Loads a .mid straight off the Teensy 4.1 microSD slot and produces the same
// {deltaMs,note,vel} event stream that tools/midi2c.py bakes in — so songs can
// be added by copying .mid files to /songs on the card, NO firmware rebuild.
// Merges all tracks, honors the tempo map, and filters MIDI channel 10 (drums,
// meaningless on a melodic FM patch), matching the offline transcoder.
//
// Defensive: every read is bounds-checked so a truncated/garbage file returns
// -1 rather than hard-faulting the audio firmware. Parsing runs from the main
// loop (a song-select handler), never the audio ISR.
#pragma once
#include <Arduino.h>
#include <SD.h>
#include <algorithm>
#include "song_event.h"

namespace sdmidi {

static inline uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static inline uint16_t be16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }

// Variable-length quantity at d[*i]; advances *i. Stops at `end` to stay in bounds.
static uint32_t readVar(const uint8_t *d, size_t end, size_t *i) {
    uint32_t v = 0;
    for (int k = 0; k < 4 && *i < end; ++k) {
        uint8_t c = d[(*i)++];
        v = (v << 7) | (c & 0x7F);
        if (!(c & 0x80)) break;
    }
    return v;
}

struct TickEv { uint32_t tick; uint8_t note; uint8_t vel; };  // vel 0 = note-off
struct Tempo  { uint32_t tick; uint32_t uspq; };

// Parse buf[0..len) into out[0..maxOut). Returns event count, or -1 on error.
// Allocates a scratch event array on the heap (freed before returning).
static int parse(const uint8_t *d, size_t len, SongEv *out, int maxOut) {
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
    const int MAXTEMPO = 64;
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
    if (ntempo == 0) { tempos[0] = {0, 500000}; ntempo = 1; }       // default 120bpm
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

    // Pass 2a: count note events (skip channel 9 / MIDI ch10 = drums).
    int total = 0;
    for (int t = 0; t < nt; ++t) {
        size_t i = tS[t], e = tE[t]; uint8_t status = 0;
        while (i < e) {
            readVar(d, e, &i); if (i >= e) break;
            uint8_t b0 = d[i]; if (b0 & 0x80) { status = b0; i++; }
            if (status == 0xFF) { if (i >= e) break; i++; uint32_t ml = readVar(d, e, &i); i += ml; }
            else if (status == 0xF0 || status == 0xF7) { uint32_t sl = readVar(d, e, &i); i += sl; }
            else {
                uint8_t hi = status & 0xF0, ch = status & 0x0F;
                if ((hi == 0x80 || hi == 0x90) && ch != 9) total++;
                i += (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
            }
        }
    }
    if (total <= 0) return -1;
    TickEv *ev = (TickEv *)malloc(sizeof(TickEv) * total);
    if (!ev) return -1;

    // Pass 2b: collect the note events with absolute ticks.
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
                if (hi == 0x80 || hi == 0x90 || hi == 0xA0 || hi == 0xB0 || hi == 0xE0) {
                    if (i + 2 > e) break;
                    uint8_t d1 = d[i], d2 = d[i + 1]; i += 2;
                    if (ch != 9) {
                        if (hi == 0x90)      { ev[ne++] = {tick, d1, d2}; }        // note-on (vel 0 = off)
                        else if (hi == 0x80) { ev[ne++] = {tick, d1, (uint8_t)0}; } // note-off
                    }
                } else if (hi == 0xC0 || hi == 0xD0) { i += 1; }
                else { i += 1; }
            }
        }
    }

    // Sort by absolute tick (stable: keeps same-tick order within/across tracks).
    std::stable_sort(ev, ev + ne, [](const TickEv &a, const TickEv &b) { return a.tick < b.tick; });

    // Emit delta-ms event stream; split gaps > 60000ms into rest padding.
    int no = 0; uint32_t prevMs = 0;
    for (int k = 0; k < ne && no < maxOut; ++k) {
        uint32_t absMs = (uint32_t)(tickToMs(ev[k].tick) + 0.5);
        uint32_t dd = (absMs > prevMs) ? (absMs - prevMs) : 0; prevMs = absMs;
        while (dd > 60000 && no < maxOut) { out[no++] = {60000, 0, 0}; dd -= 60000; }
        if (no < maxOut) out[no++] = {(uint16_t)dd, ev[k].note, ev[k].vel};
    }
    free(ev);
    return no;
}

// Load /path off the SD card and parse it into out[0..maxOut). -1 on error.
static int loadFile(const char *path, SongEv *out, int maxOut) {
    File f = SD.open(path, FILE_READ);
    if (!f) return -1;
    size_t len = f.size();
    if (len < 14 || len > 512UL * 1024) { f.close(); return -1; }   // sanity cap 512KB
    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf) { f.close(); return -1; }
    size_t got = f.read(buf, len);
    f.close();
    int n = (got == len) ? parse(buf, len, out, maxOut) : -1;
    free(buf);
    return n;
}

} // namespace sdmidi
