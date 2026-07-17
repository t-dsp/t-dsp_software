// AudioLoopWav.h — save a captured loop to SD as a 16-bit PCM .wav.
//
// No WAV *writer* exists anywhere in this repo (only parsers), so we author the
// 44-byte RIFF/WAVE header by hand, then stream the loop body. Kept in a separate
// header so the core AudioLooper doesn't force <SD.h> on consumers that never save.
//
// Call from FOREGROUND only, with the loop Playing or Idle (NOT Recording/Overdub —
// the buffer must be stable). SD.begin() is the caller's responsibility. Teensy is
// little-endian, matching WAV, so samples stream out raw.
#pragma once
#include <Arduino.h>
#include <SD.h>
#include "AudioLooper.h"

namespace tdsp {

inline bool saveWavFile(const char *path, const AudioLooper &lp) {
    const uint32_t frames = lp.loopFrames();
    const uint16_t ch     = lp.channels();
    const uint32_t rate   = (uint32_t)(lp.sampleRate() + 0.5f);
    const int16_t *buf    = lp.buffer();
    if (!path || !buf || frames == 0 || ch == 0) return false;

    const uint32_t dataBytes  = frames * ch * 2u;         // 16-bit
    const uint32_t byteRate   = rate * ch * 2u;
    const uint16_t blockAlign = (uint16_t)(ch * 2u);

    if (SD.exists(path)) SD.remove(path);                 // fresh overwrite
    File f = SD.open(path, FILE_WRITE);
    if (!f) return false;

    uint8_t h[44];
    auto put32 = [&](int o, uint32_t v) { h[o]=v; h[o+1]=v>>8; h[o+2]=v>>16; h[o+3]=v>>24; };
    auto put16 = [&](int o, uint16_t v) { h[o]=v; h[o+1]=v>>8; };
    memcpy(h + 0,  "RIFF", 4);   put32(4,  36u + dataBytes);
    memcpy(h + 8,  "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);   put32(16, 16u);          // PCM fmt chunk size
    put16(20, 1);                                          // audioFormat = PCM
    put16(22, ch);               put32(24, rate);
    put32(28, byteRate);         put16(32, blockAlign);
    put16(34, 16);                                         // bitsPerSample
    memcpy(h + 36, "data", 4);   put32(40, dataBytes);
    if (f.write(h, 44) != 44) { f.close(); return false; }

    // Stream the body in chunks (avoid a huge single write).
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t left = dataBytes;
    while (left) {
        uint32_t n = left > 4096u ? 4096u : left;
        if (f.write(p, n) != n) { f.close(); return false; }
        p += n; left -= n;
    }
    f.close();
    return true;
}

}  // namespace tdsp
