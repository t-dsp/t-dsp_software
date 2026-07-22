// test_region.cpp — desktop unit tests for the dfd core (DESIGN §10).
//
// Plain assert-style main(), no test framework. Runs entirely on the host backend
// (MemorySource + StdAllocator) so the delicate head->body handoff, loop, EOF, jump and
// allocator-fallback logic are proven with NO hardware in the loop. Build+run via
// test/run_tests.py (MSVC) or any C++17 compiler: `cl /EHsc /std:c++17 /I..\include test_region.cpp`.
//
// Conventions: everything is interleaved int16 samples; stereo unless noted. The test source is
// a per-index ramp so any gap/overlap/underrun at the head->body seam shows up as a value miss.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

#include "dfd/Region.h"
#include "dfd/Resampler.h"
#include "dfd/Voice.h"
#include "dfd/Pool.h"
#include "dfd/backends/host/MemorySource.h"
#include "dfd/backends/host/StdAllocator.h"

using namespace dfd;

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(cond, msg) do { g_checks++; if (!(cond)) { g_fail++; \
    printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } } while (0)

// A deterministic, per-interleaved-index value so contiguity is exactly verifiable.
static int16_t rampVal(uint32_t i) { return (int16_t)((int32_t)(i % 60000u) - 30000); }

static std::vector<int16_t> buildRamp(uint32_t samples) {
    std::vector<int16_t> v(samples);
    for (uint32_t i = 0; i < samples; i++) v[i] = rampVal(i);
    return v;
}

// Play a whole non-looping region, servicing between blocks like a real loop(), collecting
// interleaved output. `serviceCalls` per block simulates loop() running faster than audio.
static std::vector<int16_t> playAll(Region& r, uint32_t start, uint32_t len, uint8_t ch,
                                    uint16_t blockFrames, int serviceCalls) {
    r.play(start, len, false, 0);
    std::vector<int16_t> out;
    std::vector<int16_t> block((size_t)blockFrames * ch);
    int guard = 0;
    while (r.active() && guard++ < 100000) {
        bool any = r.read(block.data(), blockFrames);
        // Only keep frames that were real (region deactivates mid-block; trailing is zero-fill).
        // We reconstruct length from framesPlayed instead — simpler: append all, trim later.
        for (auto s : block) out.push_back(s);
        (void)any;
        for (int s = 0; s < serviceCalls; s++) r.service();
    }
    return out;
}

// ---- Test 1: head->body contiguity (no gap/overlap at H) ---------------------------------
static void test_contiguity() {
    printf("[test] head->body contiguity\n");
    const uint8_t ch = 2;
    const uint32_t frames = 100, len = frames * ch;              // 200 interleaved samples
    auto data = buildRamp(len);
    MemorySource src(data.data(), len, ch);
    StdAllocator alloc;
    RegionConfig cfg{ /*head*/ 64, /*ring*/ 48, /*history*/ 16 };  // body 64..199 >> ring: must stream
    { Region r(alloc, cfg); r.setSource(src);
      auto out = playAll(r, 0, len, ch, 16, 8);
      CHECK(out.size() >= len, "produced at least the region length");
      bool exact = true;
      for (uint32_t i = 0; i < len; i++) if (out[i] != rampVal(i)) { exact = false;
          printf("  mismatch at %u: got %d want %d\n", i, out[i], rampVal(i)); break; }
      CHECK(exact, "every sample across the head->body seam is exact (no gap/overlap)");
      CHECK(r.underruns() == 0, "no underruns when well-serviced");
    }
    CHECK(alloc.liveAllocations() == 0, "Region freed its buffers (no leak)");
}

// ---- Test 2: the audio path never touches the Source (trigger is source-free) -------------
static void test_audio_path_source_free() {
    printf("[test] audio path is source-free over the head\n");
    const uint8_t ch = 2;
    const uint32_t frames = 100, len = frames * ch;
    auto data = buildRamp(len);
    MemorySource src(data.data(), len, ch);
    StdAllocator alloc;
    RegionConfig cfg{ 64, 48, 16 };
    Region r(alloc, cfg); r.setSource(src);
    r.play(0, len, false, 0);                                    // primes head+ring (source reads)
    uint32_t baseline = src.readCount();
    // Read exactly the head (32 frames = 64 samples) WITHOUT servicing.
    std::vector<int16_t> block(32 * ch);
    r.read(block.data(), 32);
    CHECK(src.readCount() == baseline, "no Source::read() during head-resident (attack) playback");
    bool exact = true;
    for (uint32_t i = 0; i < 64; i++) if (block[i] != rampVal(i)) exact = false;
    CHECK(exact, "head samples read correctly with zero disk I/O");
}

// ---- Test 3: loop correctness (loopStart in head, body exceeds ring) ----------------------
static void test_loop() {
    printf("[test] loop wrap (loopStart in head, body >> ring)\n");
    const uint8_t ch = 2;
    const uint32_t frames = 100, len = frames * ch;             // loop [0,200)
    auto data = buildRamp(len);
    MemorySource src(data.data(), len, ch);
    StdAllocator alloc;
    RegionConfig cfg{ 64, 48, 16 };
    Region r(alloc, cfg); r.setSource(src);
    r.play(0, len, true, 0);
    std::vector<int16_t> block(16 * ch);
    std::vector<int16_t> out;
    for (int b = 0; b < 40; b++) {                              // 40*16=640 frames > 3 loops
        r.read(block.data(), 16);
        for (auto s : block) out.push_back(s);
        for (int s = 0; s < 8; s++) r.service();
    }
    bool exact = true;
    for (uint32_t i = 0; i < out.size(); i++) {
        int16_t want = rampVal(i % len);                        // loop repeats data[0..199]
        if (out[i] != want) { exact = false;
            printf("  loop mismatch at %u (loop-pos %u): got %d want %d\n", i, i % len, out[i], want); break; }
    }
    CHECK(exact, "looped output repeats the region exactly across wraps");
    CHECK(r.underruns() == 0, "no underruns across loop wraps when serviced");
}

// ---- Test 4: EOF / short-file (region claims more than the source holds) -------------------
static void test_eof_short() {
    printf("[test] EOF / short-file clamps cleanly\n");
    const uint8_t ch = 2;
    const uint32_t frames = 40, len = frames * ch;             // source only 80 samples
    auto data = buildRamp(len);
    MemorySource src(data.data(), len, ch);
    StdAllocator alloc;
    RegionConfig cfg{ 32, 32, 8 };
    Region r(alloc, cfg); r.setSource(src);
    // Ask to play 500 frames though only 40 exist -> play() must clamp to the source length.
    r.play(0, 500 * ch, false, 0);
    CHECK(r.framesTotal() == frames, "region length clamped to the source");
    auto out = playAll(r, 0, len, ch, 16, 8);
    bool exact = true;
    for (uint32_t i = 0; i < len; i++) if (out[i] != rampVal(i)) exact = false;
    CHECK(exact, "all real samples play, no read past EOF");
    CHECK(!r.active(), "region ended cleanly at EOF");
    CHECK(r.underruns() == 0, "no underruns at EOF");
}

// A source whose fully-resident case (region <= head) needs no body at all.
static void test_fully_resident() {
    printf("[test] fully-resident region (len <= head) never streams\n");
    const uint8_t ch = 2;
    const uint32_t frames = 20, len = frames * ch;             // 40 samples
    auto data = buildRamp(len);
    MemorySource src(data.data(), len, ch);
    StdAllocator alloc;
    RegionConfig cfg{ 128, 32, 8 };                            // head bigger than the whole region
    Region r(alloc, cfg); r.setSource(src);
    r.play(0, len, false, 0);                                  // one loadHead read, no body
    uint32_t baseline = src.readCount();                       // capture AFTER the head load
    std::vector<int16_t> block(8 * ch);
    std::vector<int16_t> out;
    int guard = 0;
    while (r.active() && guard++ < 1000) {
        r.read(block.data(), 8);
        for (auto s : block) out.push_back(s);
        for (int s = 0; s < 4; s++) r.service();               // must be no-ops (nothing to stream)
    }
    CHECK(src.readCount() == baseline, "no body streaming for a head-resident region");
    bool exact = true;
    for (uint32_t i = 0; i < len; i++) if (out[i] != rampVal(i)) exact = false;
    CHECK(exact, "fully-resident playback is exact");
}

// ---- Test 5: jump() / restart-from-head is instant and correct ----------------------------
static void test_jump_restart() {
    printf("[test] jump() restart-from-head\n");
    const uint8_t ch = 2;
    const uint32_t frames = 100, len = frames * ch;
    auto data = buildRamp(len);
    MemorySource src(data.data(), len, ch);
    StdAllocator alloc;
    RegionConfig cfg{ 64, 48, 16 };
    Region r(alloc, cfg); r.setSource(src);
    r.play(0, len, false, 0);
    std::vector<int16_t> block(16 * ch);
    // Play a few blocks into the BODY, then jump back to the head start.
    for (int b = 0; b < 4; b++) { r.read(block.data(), 16); for (int s=0;s<8;s++) r.service(); }
    uint32_t before = src.readCount();
    r.jump(0);                                                 // restart from head
    // Immediately read the head — must be instant (from RAM) and correct, no source read.
    r.read(block.data(), 16);
    CHECK(src.readCount() == before, "restart-from-head does not read the Source");
    bool exact = true;
    for (uint32_t i = 0; i < 32; i++) if (block[i] != rampVal(i)) exact = false;
    CHECK(exact, "restart replays the head exactly from sample 0");
    // And the rest continues correctly with servicing.
    for (int s = 0; s < 16; s++) r.service();
    r.read(block.data(), 16);                                  // frames 16..31 (samples 32..63, still head)
    for (uint32_t i = 0; i < 32; i++) if (block[i] != rampVal(16*ch + i)) exact = false;
    CHECK(exact, "playback continues correctly after a restart");
    CHECK(r.underruns() == 0, "no underruns after restart");
}

// ---- Test 6: jump() into the BODY (cold jump = one refetch, then exact) -------------------
static void test_jump_body() {
    printf("[test] jump() into the body (cold jump)\n");
    const uint8_t ch = 2;
    const uint32_t frames = 100, len = frames * ch;
    auto data = buildRamp(len);
    MemorySource src(data.data(), len, ch);
    StdAllocator alloc;
    RegionConfig cfg{ 64, 48, 16 };
    Region r(alloc, cfg); r.setSource(src);
    r.play(0, len, false, 0);
    r.jump(80);                                                // frame 40 (in the body)
    for (int s = 0; s < 8; s++) r.service();                   // let the refetch land
    std::vector<int16_t> block(16 * ch);
    r.read(block.data(), 16);
    bool exact = true;
    for (uint32_t i = 0; i < 32; i++) if (block[i] != rampVal(80 + i)) { exact = false;
        printf("  body-jump mismatch at %u: got %d want %d\n", i, block[i], rampVal(80+i)); break; }
    CHECK(exact, "cold jump into the body plays the right samples after a refetch");
}

// ---- Test 7: allocator fallback + genuine OOM safety --------------------------------------
static void test_allocator() {
    printf("[test] allocator fallback + OOM safety\n");
    const uint8_t ch = 2;
    const uint32_t frames = 60, len = frames * ch;
    auto data = buildRamp(len);
    MemorySource src(data.data(), len, ch);

    // Fallback: fast pool 'unavailable' -> served from normal RAM, Region still ok() and exact.
    FallbackAllocator fb;
    RegionConfig cfg{ 32, 48, 16 };
    { Region r(fb, cfg); r.setSource(src);
      CHECK(r.ok(), "Region ok() when the allocator falls back to normal RAM");
      CHECK(fb.fallbacks() == 2, "both head and ring took the fallback path (preferFast honored)");
      auto out = playAll(r, 0, len, ch, 16, 8);
      bool exact = true; for (uint32_t i = 0; i < len; i++) if (out[i] != rampVal(i)) exact = false;
      CHECK(exact, "playback exact on the fallback allocator");
    }

    // Genuine OOM: alloc returns nullptr -> Region !ok(), play()/read() are safe no-ops.
    OomAllocator oom;
    Region r2(oom, cfg); r2.setSource(src);
    CHECK(!r2.ok(), "Region reports !ok() on genuine OOM");
    r2.play(0, len, false, 0);
    CHECK(!r2.active(), "play() is a safe no-op when !ok()");
    std::vector<int16_t> block(16 * ch, 123);
    bool any = r2.read(block.data(), 16);
    CHECK(!any, "read() is a safe no-op (no crash) when !ok()");
}

// ---- Test 8: Resampler rate 1.0 exact, rate 0.5 lengthens ---------------------------------
static void test_resampler() {
    printf("[test] resampler rate 1.0 exact, 0.5 lengthens\n");
    const uint8_t ch = 2;
    const uint32_t frames = 80, len = frames * ch;
    auto data = buildRamp(len);
    MemorySource src(data.data(), len, ch);
    StdAllocator alloc;
    RegionConfig cfg{ 32, 64, 16 };

    // rate 1.0 must be bit-exact (drums depend on this).
    { Region r(alloc, cfg); r.setSource(src); r.play(0, len, false, 0);
      Resampler rs; rs.setRate(1.0f); rs.reset();
      std::vector<int16_t> out; std::vector<int16_t> block(16*ch);
      int guard=0;
      while (r.active() && guard++ < 1000) {
          rs.read(r, block.data(), 16);
          for (auto s : block) out.push_back(s);
          for (int s=0;s<8;s++) r.service();
      }
      bool exact = true; for (uint32_t i = 0; i < len; i++) if (out[i] != rampVal(i)) exact = false;
      CHECK(exact, "rate 1.0 is a bit-exact passthrough");
    }
    // rate 0.5 -> roughly twice as many frames before the region drains.
    { Region r(alloc, cfg); r.setSource(src); r.play(0, len, false, 0);
      Resampler rs; rs.setRate(0.5f); rs.reset();
      std::vector<int16_t> block(16*ch); int outFrames = 0; int guard=0;
      while (r.active() && guard++ < 1000) {
          rs.read(r, block.data(), 16);
          outFrames += 16;
          for (int s=0;s<8;s++) r.service();
      }
      CHECK(outFrames > (int)frames + 20, "rate 0.5 stretches the region (more output frames)");
    }
}

// ---- Test 9: Pool idle->oldest stealing ---------------------------------------------------
static void test_pool() {
    printf("[test] pool pick + steal\n");
    const uint8_t ch = 2;
    const uint32_t frames = 200, len = frames * ch;
    auto data = buildRamp(len);
    MemorySource src(data.data(), len, ch);
    StdAllocator alloc;
    RegionConfig cfg{ 32, 32, 8 };
    Pool<3> pool(alloc, cfg);
    // Fill all three voices.
    uint8_t a = pool.pick(); pool.voice(a).setSource(src); pool.voice(a).play(0, len, true, 0);
    uint8_t b = pool.pick(); pool.voice(b).setSource(src); pool.voice(b).play(0, len, true, 0);
    uint8_t c = pool.pick(); pool.voice(c).setSource(src); pool.voice(c).play(0, len, true, 0);
    CHECK(a != b && b != c && a != c, "three fresh picks are distinct idle voices");
    // All busy (looping) -> next pick steals the OLDEST (voice a).
    uint8_t d = pool.pick();
    CHECK(d == a, "with all voices busy, pick() steals the oldest-started");
}

int main() {
    printf("dfd core desktop tests\n----------------------\n");
    test_contiguity();
    test_audio_path_source_free();
    test_loop();
    test_eof_short();
    test_fully_resident();
    test_jump_restart();
    test_jump_body();
    test_allocator();
    test_resampler();
    test_pool();
    printf("----------------------\n%d checks, %d failure(s)\n", g_checks, g_fail);
    if (g_fail) { printf("RESULT: FAIL\n"); return 1; }
    printf("RESULT: PASS\n");
    return 0;
}
