// dfd/Region.h — the ONE primitive: a streamed region (DESIGN §1, §3, §4).
//
// A region [start, start+length) is split into a RESIDENT HEAD [start, start+H) held in
// fast RAM and a STREAMED BODY [start+H, start+length) held in a sliding ring that service()
// keeps ahead of the playhead. Because the head is always in RAM, note-starts / loop-wraps /
// restarts never wait on storage — that single inequality (H > worst-case storage stall) is
// the whole guarantee (DESIGN §1).
//
// The delicate part is the head->body handoff (DESIGN §4): the body ring is preloaded
// STARTING AT start+H (never 0), so head sample H-1 is followed by body sample H with no gap
// and no double-read. Prove that once (desktop tests) and looping / stutter are compositions.
//
// UNITS: everything here is in interleaved int16 SAMPLES (a stereo frame is 2 samples). The
// buffers are channel-agnostic memcpy targets; channels() only matters when read() splits a
// frame into L/R. All sizes in RegionConfig are interleaved-sample counts.
//
// THREADING (DESIGN §6): single-producer (service, non-realtime) / single-consumer (read,
// audio path). read()/readFrame() do only index math + copy over already-resident buffers —
// no allocation, no Source access. service() is the sole caller of Source::read().
//
// Portable core: includes only <cstdint>/<cstddef> + the two injected interfaces.
#pragma once
#include <cstdint>
#include <cstddef>
#include "Source.h"
#include "Allocator.h"

namespace dfd {

struct RegionConfig {
    uint32_t headSamples;    // H — resident head capacity. MUST exceed the worst-case storage
                             //     stall (DESIGN §7 sizes it). Interleaved samples.
    uint32_t ringSamples;    // body ring capacity (per region). >= a few audio blocks past H.
    uint32_t historySamples; // trailing look-behind retained in the ring (backward stutter, §5).
};

class Region {
public:
    // Buffers are allocated ONCE here (off the audio path). The Source is injected later via
    // setSource() — this supports the persistent-handle rebinding a consumer needs (one voice
    // replayed from many files without reallocating its ring; DESIGN §11). alloc must outlive
    // this Region.
    Region(Allocator& alloc, const RegionConfig& cfg)
        : _alloc(&alloc), _cfg(cfg) {
        _head = _alloc->alloc(_cfg.headSamples, /*preferFast=*/true);
        _ring = _alloc->alloc(_cfg.ringSamples, /*preferFast=*/true);
        // ZERO the buffers: allocators hand back uninitialized memory (plain malloc on a no-PSRAM
        // board), and any read of an as-yet-unwritten slot must be silence, not stale garbage
        // (which is broadband HISS). Cheap: once, off the audio path.
        if (_head) for (uint32_t i = 0; i < _cfg.headSamples; ++i) _head[i] = 0;
        if (_ring) for (uint32_t i = 0; i < _cfg.ringSamples; ++i) _ring[i] = 0;
    }

    ~Region() {
        if (_alloc) { _alloc->free(_head); _alloc->free(_ring); }
        _head = _ring = nullptr;
    }

    Region(const Region&) = delete;
    Region& operator=(const Region&) = delete;

    // Did both buffers allocate? (A voice that failed to alloc stays silent instead of crashing.)
    bool ok() const { return _head != nullptr && _ring != nullptr; }

    // Bind / rebind the storage source. Cheap; does no I/O. Call before play().
    void setSource(Source& s) { _src = &s; }
    Source* source() const { return _src; }

    // Fill the resident head for [start, start+H) with ONE Source read. Off the audio path.
    // Sets _headLen to what actually loaded (a region shorter than H is fully head-resident).
    void loadHead(uint32_t start) {
        if (!ok() || !_src) { _headLen = 0; return; }
        uint32_t want = _cfg.headSamples;
        if (start + want > _regionEnd) want = (start < _regionEnd) ? (_regionEnd - start) : 0;
        _headLen = _src->read(start, _head, want);
    }

    // Start playing [start, start+length). loop wraps at start+length back to loopStart
    // (defaults to start). Clamps length to the source. Loads the head and primes the ring
    // (both off the audio path) so the first read() is glitch-free.
    void play(uint32_t start, uint32_t length, bool loop = false, uint32_t loopStart = 0) {
        // Quiesce FIRST, before touching any geometry. The audio ISR (read()) and this setup run
        // on different contexts; if update() lands while _start/_regionEnd/_bodyStart/_pos are only
        // half-rewritten, sampleAt() would index the head/ring with a mismatched anchor and emit
        // garbage (audible static). Holding _active=false across the whole rewrite — and only
        // arming it once the head is loaded AND the ring primed (bottom of this fn) — makes a
        // retrigger/steal/kit-change glitch-free regardless of whether the caller pre-stopped us.
        _active = false;
        if (!ok() || !_src) return;
        _ch = _src->channels(); if (_ch < 1) _ch = 1;

        uint32_t total = _src->totalSamples();
        if (start > total) start = total;
        if (start + length > total) length = total - start;

        _start     = start;
        _regionEnd = start + length;
        _loop      = loop;
        _loopStart = loop ? loopStart : start;
        if (_loopStart < _start)     _loopStart = _start;
        if (_loopStart >= _regionEnd) _loopStart = _start;

        loadHead(_start);
        _bodyStart = _start + _headLen;   // first sample that lives in the ring (contiguity at H)

        _pos = _start;
        // Empty ring anchored at bodyStart; primeRing() fills it before we return.
        _winStart = _winEnd = _bodyStart;
        primeRing();
        _active = true;   // arm only now — head loaded, ring primed, geometry fully consistent
    }

    // Seek the playhead. If `sample` is within the head it restarts-from-head instantly; if it
    // is in a body region already resident in the ring, that is instant too; otherwise the ring
    // is re-anchored and service() refetches (a cold jump = one read; DESIGN §3/§5).
    void jump(uint32_t sample) {
        if (!ok()) return;
        if (sample < _start) sample = _start;
        if (sample >= _regionEnd) { if (_loop) sample = _loopStart; else { _pos = _regionEnd; _active = false; return; } }
        _pos = sample;
        if (sample < _bodyStart) {
            // Landing in the head; the ring must be anchored at bodyStart for the handoff.
            if (!(_winStart <= _bodyStart && _bodyStart < _winEnd)) _winStart = _winEnd = _bodyStart;
        } else if (!(_winStart <= sample && sample < _winEnd)) {
            _winStart = _winEnd = sample;   // cold jump into the body: refetch from here
        }
        _active = true;   // arm last, once _pos and the ring window agree (see play())
    }

    // Silence this region immediately (audio path stops on the next block). No I/O.
    void stop() { _active = false; }

    bool active() const { return _active; }
    uint8_t channels() const { return _ch; }

    // ---- AUDIO PATH: pure index math + copy. No FS, no alloc. -------------------------------
    // Emit ONE frame (channels() samples) into dst and advance. Returns false once inactive
    // (end of a non-looping region) — the caller then zero-fills.
    bool readFrame(int16_t* dst) {
        if (!_active) return false;
        for (uint8_t c = 0; c < _ch; c++) dst[c] = sampleAt(_pos + c);
        advanceFrame();
        return true;
    }

    // Emit up to `frames` frames (interleaved) into dst; zero-fills the tail past end-of-region.
    // Returns true if it produced ANY audio this call.
    bool read(int16_t* dst, uint16_t frames) {
        uint16_t f = 0;
        for (; f < frames; f++) {
            if (!readFrame(dst + (uint32_t)f * _ch)) break;
        }
        for (uint32_t i = (uint32_t)f * _ch; i < (uint32_t)frames * _ch; i++) dst[i] = 0;
        return f > 0;
    }

    // ---- STREAM PATH: the sole caller of Source::read(). Bounded per call (DESIGN §6). ------
    // Keeps the ring filled ahead of the playhead and drops history behind it. Returns true if
    // it moved any data. Call regularly from a non-audio context (loop()/EventResponder).
    bool service() {
        if (!_active || !ok() || !_src) return false;
        bool did = fillRing(kServiceChunk);
        dropHistory();
        return did;
    }

    // ---- geometry (for steal policy / UI) --------------------------------------------------
    uint32_t framesTotal()  const { return _ch ? (_regionEnd - _start) / _ch : 0; }
    uint32_t framesPlayed() const { return (_ch && _pos > _start) ? (_pos - _start) / _ch : 0; }

    uint32_t underruns() const { return _underruns; }
    void     resetUnderruns() { _underruns = 0; }

private:
    static constexpr uint32_t kServiceChunk = 1024; // max samples fetched per service() call

    // Where does absolute sample `a` live? head buffer, or body ring, or (underrun) silence.
    int16_t sampleAt(uint32_t a) {
        if (a < _bodyStart) return _head[a - _start];              // resident head
        if (a >= _winStart && a < _winEnd)                         // body ring (window-resident)
            return _ring[(a - _bodyStart) % _cfg.ringSamples];
        _underruns++;                                              // ring dry: should not happen
        return 0;                                                  // when H > worst stall (DESIGN §4)
    }

    void advanceFrame() {
        _pos += _ch;
        if (_pos >= _regionEnd) {
            if (_loop) {
                _pos = _loopStart;
                // On wrap, re-anchor the ring to the next BODY sample we'll need if it is no
                // longer resident. When loopStart is in the head (the common case) the head
                // replays source-free while service() refetches [bodyStart, ...) behind it; when
                // loopStart is in the body and the loop exceeds the ring, this triggers a refetch
                // from the loop point (Phase 2 hides that seam with a crossfade head).
                uint32_t nextBody = (_loopStart >= _bodyStart) ? _loopStart : _bodyStart;
                if (nextBody < _regionEnd && !(_winStart <= nextBody && nextBody < _winEnd))
                    _winStart = _winEnd = nextBody;
            } else {
                _active = false;
            }
        }
    }

    // Fetch forward from _winEnd toward the region end, bounded to `budget` samples and to the
    // ring capacity. Handles the ring wrap by splitting the Source read into <=2 segments.
    bool fillRing(uint32_t budget) {
        uint32_t moved = 0;
        while (budget > 0) {
            if (_winEnd >= _regionEnd) break;                       // whole body fetched
            uint32_t windowFree = _cfg.ringSamples - (_winEnd - _winStart);
            if (windowFree == 0) break;                             // ring full ahead of playhead
            uint32_t want = budget;
            if (want > windowFree) want = windowFree;
            uint32_t toEnd = _regionEnd - _winEnd;
            if (want > toEnd) want = toEnd;
            uint32_t slot = (_winEnd - _bodyStart) % _cfg.ringSamples;
            uint32_t seg  = _cfg.ringSamples - slot;                // room to the ring's physical end
            if (seg > want) seg = want;
            uint32_t got = _src->read(_winEnd, _ring + slot, seg);
            _winEnd += got;
            moved   += got;
            budget  -= got;
            if (got < seg) {                                        // short read = source EOF
                if (_winEnd < _regionEnd) _regionEnd = _winEnd;     // clamp the region to real data
                if (_loop && _loopStart >= _regionEnd) _loopStart = _start;
                break;
            }
        }
        return moved > 0;
    }

    // Advance _winStart to reclaim ring space, but retain historySamples behind the playhead
    // and (when looping) the whole loop body if it fits the ring — never dropping ahead of the
    // playhead. Only ever moves _winStart forward.
    void dropHistory() {
        uint32_t curBody = (_pos > _bodyStart) ? _pos : _bodyStart;
        uint32_t histFloor = (_pos > _bodyStart + _cfg.historySamples) ? (_pos - _cfg.historySamples) : _bodyStart;
        uint32_t desired = histFloor;
        if (_loop) {
            uint32_t loopBody = (_loopStart >= _bodyStart) ? _loopStart : _bodyStart;
            // Keep the WHOLE loop body resident ONLY if it fits the ring; otherwise let the window
            // slide (streaming) and the wrap re-anchor + refetch (advanceFrame). Retaining a loop
            // body larger than the ring would pin _winStart and starve the far end (underrun).
            if ((_regionEnd - loopBody) <= _cfg.ringSamples && loopBody < desired) desired = loopBody;
        }
        uint32_t minStart = (_winEnd > _cfg.ringSamples) ? (_winEnd - _cfg.ringSamples) : 0;
        if (desired < minStart) desired = minStart;               // window can't exceed ring capacity
        if (desired > curBody)  desired = curBody;                // never drop unplayed audio
        if (desired > _winStart) _winStart = desired;
    }

    // Fill the ring as full as it will go (used at play()-prep, off the audio path).
    void primeRing() {
        while (fillRing(kServiceChunk)) {
            if (_winEnd >= _regionEnd) break;
            if (_cfg.ringSamples - (_winEnd - _winStart) == 0) break;
        }
    }

    Allocator*   _alloc = nullptr;
    Source*      _src   = nullptr;
    RegionConfig _cfg;
    int16_t*     _head  = nullptr;
    int16_t*     _ring  = nullptr;

    uint8_t  _ch = 1;
    uint32_t _start = 0, _regionEnd = 0;   // region bounds (absolute source samples)
    uint32_t _loopStart = 0; bool _loop = false;
    uint32_t _headLen = 0;                 // samples actually resident in the head
    uint32_t _bodyStart = 0;               // _start + _headLen (first ring-resident sample)
    uint32_t _pos = 0;                     // playhead (absolute source sample)
    uint32_t _winStart = 0, _winEnd = 0;   // body ring window [start,end) in absolute samples
    bool     _active = false;
    uint32_t _underruns = 0;
};

} // namespace dfd
