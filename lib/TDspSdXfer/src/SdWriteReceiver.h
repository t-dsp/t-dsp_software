// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.
//
// SdWriteReceiver — host->device file WRITE receiver over any byte Stream.
//
// The complement of the device->host '@READ'/streamFile primitive. It lets a
// host (a Web Serial page, a PowerShell tool) push a file straight onto the
// Teensy's SD card over USB CDC at full speed, with NO reflash and NO base64
// overhead — the payload is raw binary.
//
// Wire protocol
// -------------
//   Host -> Dev:  @WB=<id>\x1f<path>\x1f<bytes>[\x1f<crc32hex>]\n   ; begin
//   Dev  -> Host: @WOK=<id>\x1f<bytes>            ; file opened, stream may begin
//                 @WERR=<id>\x1f<reason>          ; parse / mkdir / open failure
//   Host -> Dev:  <bytes> raw octets, back-to-back ; NOT line-framed
//   Dev  -> Host: @WE=<id>\x1f<written>[\x1f<crc32hex>]   ; success
//                 @WERR=<id>\x1f<reason>                  ; short write / crc / timeout
//
// Fields are split by 0x1f (US), matching the '@READ' framing. <bytes> is the
// exact payload length; the receiver reads precisely that many bytes and then
// returns to normal line mode — anything the host pipelines after the payload
// (e.g. a following '@' command) is left for the caller's line assembler.
//
// Integration
// -----------
//   tdsp::SdWriteReceiver g_rx(SD);
//
//   // in your '@'-command dispatch:
//   if (!strncmp(line, "@WB=", 4)) { g_rx.begin(line + 4, reply); return true; }
//
//   // in loop(), BEFORE the line assembler consumes bytes:
//   if (g_rx.receiving()) g_rx.pump(Serial, Serial);   // route raw payload to SD
//   ...
//   g_rx.tick(Serial, millis());                       // stall watchdog
//
// The CRC32 is the standard IEEE-802.3 (zlib) variety: reflected, init
// 0xFFFFFFFF, xorout 0xFFFFFFFF, poly 0xEDB88320. If the begin frame omits the
// crc field the transfer is accepted without integrity checking.

#pragma once

#include <Arduino.h>
#include <SD.h>

namespace tdsp {

class SdWriteReceiver {
public:
    enum class State : uint8_t { Idle, Receiving };

    // fs is any Arduino FS (SD is an SDClass, which derives from FS).
    explicit SdWriteReceiver(FS& fs) : fs_(fs) {}

    // Inter-byte stall watchdog (ms). If the host goes quiet mid-transfer for
    // longer than this, the partial file is removed and @WERR timeout is sent.
    void setTimeout(uint32_t ms) { timeoutMs_ = ms; }

    bool     receiving() const { return state_ == State::Receiving; }
    State    state()     const { return state_; }
    uint32_t remaining() const { return total_ - written_; }

    // Parse a begin frame. `args` is the text AFTER "@WB=". Opens the target
    // file and replies @WOK (ready) or @WERR (failure). Returns true when the
    // frame was well-formed and the file opened (a zero-length transfer
    // finalizes here and returns true); false on a parse/open error.
    bool begin(const char* args, Print& reply) {
        if (file_) file_.close();
        state_ = State::Idle;

        // Split <id>\x1f<path>\x1f<bytes>[\x1f<crchex>] in place.
        char buf[192];
        strlcpy(buf, args, sizeof(buf));
        char* f[4] = {nullptr, nullptr, nullptr, nullptr};
        int nf = 0;
        f[nf++] = buf;
        for (char* p = buf; *p && nf < 4; ++p) {
            if (*p == '\x1f') { *p = 0; f[nf++] = p + 1; }
        }
        if (nf < 3) { reply.printf("@WERR=%u\x1fbad args\n", 0); return false; }

        id_        = (uint8_t)atoi(f[0]);
        const char* path = f[1];
        total_     = (uint32_t)strtoul(f[2], nullptr, 10);
        wantCrc_   = (nf >= 4 && f[3][0] != 0);
        expectCrc_ = wantCrc_ ? (uint32_t)strtoul(f[3], nullptr, 16) : 0;

        if (!path[0]) { reply.printf("@WERR=%u\x1fno path\n", id_); return false; }
        strlcpy(path_, path, sizeof(path_));

        mkdirp(path_);
        if (fs_.exists(path_)) fs_.remove(path_);   // fresh overwrite, not append
        file_ = fs_.open(path_, FILE_WRITE);
        if (!file_) { reply.printf("@WERR=%u\x1fopen failed\n", id_); return false; }

        written_    = 0;
        crc_        = 0xFFFFFFFFul;
        lastByteMs_ = millis();
        state_      = State::Receiving;
        reply.printf("@WOK=%u\x1f%lu\n", id_, (unsigned long)total_);

        if (total_ == 0) finalize(reply);   // empty file: done immediately
        return true;
    }

    // Feed raw payload bytes. Consumes at most remaining() of them (so any
    // trailing pipelined command bytes are left untouched) and returns the
    // number consumed. Finalizes automatically once the declared length is hit.
    size_t feed(const uint8_t* data, size_t n, Print& reply) {
        if (state_ != State::Receiving) return 0;
        uint32_t rem  = total_ - written_;
        size_t   take = (n < rem) ? n : (size_t)rem;
        if (take) {
            size_t w = file_.write(data, take);
            crc_ = crc32_update(crc_, data, w);
            written_    += w;
            lastByteMs_  = millis();
            if (w != take) { abortXfer(reply, "short write"); return w; }
        }
        if (written_ >= total_) finalize(reply);
        return take;
    }

    // Convenience: while receiving, drain exactly the payload bytes available on
    // `in` into the file. Never over-reads past the payload, so a following
    // command stays in the stream for the caller's line assembler.
    void pump(Stream& in, Print& reply) {
        uint8_t buf[512];
        while (state_ == State::Receiving) {
            uint32_t rem = total_ - written_;
            int avail = in.available();
            if (avail <= 0 || rem == 0) break;
            size_t want = (rem < (uint32_t)avail) ? (size_t)rem : (size_t)avail;
            if (want > sizeof(buf)) want = sizeof(buf);
            size_t got = in.readBytes((char*)buf, want);
            if (!got) break;
            feed(buf, got, reply);
        }
    }

    // Call every loop(): enforces the inter-byte stall watchdog.
    void tick(Print& reply, uint32_t nowMs) {
        if (state_ == State::Receiving && (nowMs - lastByteMs_) > timeoutMs_)
            abortXfer(reply, "timeout");
    }

    // Utility: CRC32 (same params as above) of an existing SD file. Handy for a
    // host-side round-trip verify (write, then ask the device to checksum it).
    static bool fileCrc32(FS& fs, const char* path, uint32_t& outCrc, uint32_t& outBytes) {
        File f = fs.open(path);
        if (!f || f.isDirectory()) { if (f) f.close(); return false; }
        uint32_t crc = 0xFFFFFFFFul, total = 0;
        uint8_t buf[512];
        for (;;) {
            int n = f.read(buf, sizeof(buf));
            if (n <= 0) break;
            crc = crc32_update(crc, buf, (size_t)n);
            total += (uint32_t)n;
        }
        f.close();
        outCrc   = crc ^ 0xFFFFFFFFul;
        outBytes = total;
        return true;
    }

private:
    void finalize(Print& reply) {
        file_.flush();
        file_.close();
        state_ = State::Idle;
        uint32_t crc = crc_ ^ 0xFFFFFFFFul;
        if (wantCrc_ && crc != expectCrc_) {
            fs_.remove(path_);
            reply.printf("@WERR=%u\x1fcrc %08lx!=%08lx\n",
                         id_, (unsigned long)crc, (unsigned long)expectCrc_);
            return;
        }
        if (wantCrc_)
            reply.printf("@WE=%u\x1f%lu\x1f%08lx\n",
                         id_, (unsigned long)written_, (unsigned long)crc);
        else
            reply.printf("@WE=%u\x1f%lu\n", id_, (unsigned long)written_);
    }

    void abortXfer(Print& reply, const char* why) {
        if (file_) file_.close();
        fs_.remove(path_);
        state_ = State::Idle;
        reply.printf("@WERR=%u\x1f%s\n", id_, why);
    }

    // mkdir -p every parent component of `path`.
    void mkdirp(const char* path) {
        char tmp[sizeof(path_)];
        strlcpy(tmp, path, sizeof(tmp));
        for (char* q = tmp + 1; *q; ++q) {
            if (*q == '/') {
                *q = 0;
                if (!fs_.exists(tmp)) fs_.mkdir(tmp);
                *q = '/';
            }
        }
    }

    static uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t n) {
        static uint32_t table[256];
        static bool built = false;
        if (!built) {
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1) ? (0xEDB88320ul ^ (c >> 1)) : (c >> 1);
                table[i] = c;
            }
            built = true;
        }
        for (size_t i = 0; i < n; ++i)
            crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
        return crc;
    }

    FS&      fs_;
    File     file_;
    char     path_[128]  = {0};
    uint32_t total_      = 0;
    uint32_t written_    = 0;
    uint32_t crc_        = 0xFFFFFFFFul;
    uint32_t expectCrc_  = 0;
    bool     wantCrc_    = false;
    uint8_t  id_         = 0;
    uint32_t lastByteMs_ = 0;
    uint32_t timeoutMs_  = 5000;
    State    state_      = State::Idle;
};

}  // namespace tdsp
