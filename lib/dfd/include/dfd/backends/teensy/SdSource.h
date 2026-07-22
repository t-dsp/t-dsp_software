// dfd/backends/teensy/SdSource.h — a dfd::Source over a Teensy SD `File`.
//
// Wraps ONE 16-bit PCM WAV file. It parses the WAV header ONCE at open() (recording the data
// offset, channel count and data length) and thereafter serves Region body/head reads with a
// plain seek+read. It NEVER opens or closes the underlying handle by default — the consumer
// keeps a persistent per-sample File open across triggers (the drum "skip" fix; DrumSampler
// per-note handles) and the trigger path costs no FAT lookup.
//
// Teensy backend: free to include Arduino/SD. Keeps the dfd core (include/dfd/*.h) clean.
#pragma once
#include <Arduino.h>
#include <SD.h>
#include "dfd/Source.h"

namespace dfd {

// Diagnostic: how many times a mid-file SD read came back SHORT and had to be retried (each such
// event would have truncated the sample without the retry loop below). Inline global (C++17) so the
// consumer can print it; a nonzero value at runtime confirms the short-read/truncation cause.
inline volatile uint32_t g_dfdShortReads = 0;

class SdSource : public Source {
public:
    SdSource() {}

    // Adopt an already-open, caller-owned File and parse its WAV header. Reusable across
    // triggers (the handle stays open; we only seek+read it). Returns true on a valid 16-bit
    // PCM WAV. The File reference must outlive this SdSource / every read().
    bool open(File& f) {
        _file = &f;
        _ok = false;
        _dataStart = 0; _dataSamples = 0; _channels = 0;
        if (!f) return false;
        f.seek(0);

        uint8_t hdr[12];
        if (f.read(hdr, 12) != 12) return false;
        if (hdr[0]!='R'||hdr[1]!='I'||hdr[2]!='F'||hdr[3]!='F') return false;
        if (hdr[8]!='W'||hdr[9]!='A'||hdr[10]!='V'||hdr[11]!='E') return false;

        // Walk chunks: "fmt " (channels/bits) then "data" (offset + length). Skip anything else
        // (LIST/fact/etc). Chunk = 4-byte id + 4-byte little-endian size + payload (word-padded).
        uint16_t bits = 0;
        uint32_t pos = 12;
        while (true) {
            uint8_t ch[8];
            f.seek(pos);
            if (f.read(ch, 8) != 8) break;
            uint32_t sz = (uint32_t)ch[4] | ((uint32_t)ch[5]<<8) | ((uint32_t)ch[6]<<16) | ((uint32_t)ch[7]<<24);
            uint32_t body = pos + 8;
            if (ch[0]=='f'&&ch[1]=='m'&&ch[2]=='t'&&ch[3]==' ') {
                uint8_t fmt[16];
                if (f.read(fmt, (sz < 16) ? sz : 16) < 16) return false;
                _channels = (uint16_t)fmt[2] | ((uint16_t)fmt[3]<<8);
                bits      = (uint16_t)fmt[14] | ((uint16_t)fmt[15]<<8);
            } else if (ch[0]=='d'&&ch[1]=='a'&&ch[2]=='t'&&ch[3]=='a') {
                _dataStart   = body;
                _dataSamples = sz / 2;              // int16 samples, interleaved
                break;
            }
            pos = body + sz + (sz & 1);             // advance past payload (+pad byte if odd)
        }
        if (bits != 16 || _channels < 1 || _channels > 2 || _dataSamples == 0) return false;
        _ok = true;
        return true;
    }

    bool ok() const { return _ok; }

    uint32_t totalSamples() const override { return _dataSamples; }
    uint8_t  channels()     const override { return (uint8_t)_channels; }

    // Seek + read `count` interleaved int16 samples at `offsetSamples`. NON-realtime (service()).
    // LOOPS until the full request is satisfied: SdFat::read legitimately returns SHORT mid-file
    // (cluster boundary / contention), and the caller (Region::fillRing) treats a short read as
    // EOF and CLAMPS the region — which would chop a long sample mid-decay (an audible cut/click).
    // Only a real zero/negative read (true EOF or error) ends the loop early.
    uint32_t read(uint32_t offsetSamples, int16_t* dst, uint32_t count) override {
        if (!_ok || !_file || offsetSamples >= _dataSamples) return 0;
        uint32_t avail = _dataSamples - offsetSamples;
        if (count > avail) count = avail;
        _file->seek(_dataStart + (uint64_t)offsetSamples * 2);
        uint8_t* p = (uint8_t*)dst;
        uint32_t need = count * 2, done = 0;
        while (done < need) {
            int got = _file->read(p + done, need - done);
            if (got <= 0) break;                    // true EOF / error only
            done += (uint32_t)got;
            if (done < need) g_dfdShortReads++;     // came back short mid-file -> retrying
        }
        return done / 2;
    }

private:
    File*    _file = nullptr;
    bool     _ok = false;
    uint32_t _dataStart = 0;     // byte offset of PCM data
    uint32_t _dataSamples = 0;   // interleaved int16 count
    uint16_t _channels = 0;
};

} // namespace dfd
