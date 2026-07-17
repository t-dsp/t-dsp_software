// SPDX-License-Identifier: MIT
// Host-runnable tests for smf::initialLoopBeats() / snapLoopBeatsHalf() — the
// exact, meter-agnostic loop-length math the tick-synced player wraps on.
// parseSmf and friends are pure (buffer in, no SD/Arduino), so:
//
//   g++ -std=c++17 -I../src -o tlb test_loop_beats.cpp
//   ./tlb
//
// Exit 0 = all pass. See planning/tick-sync-playback/PLAN.md §2/§3.

#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>

#include "MidiSmfParser.h"   // real source under test (header-only, pure)

using tdsp::smf::initialLoopBeats;
using tdsp::smf::snapLoopBeatsHalf;

// --- mini assert runner ----------------------------------------------
static int g_failures = 0, g_checks = 0;
static const char *g_currentTest = "(none)";
#define CHECK_NEAR(a, b, eps) do {                                             \
    ++g_checks; double _da=(a),_db=(b),_de=(eps);                             \
    if (std::fabs(_da-_db) > _de) { ++g_failures;                             \
        std::fprintf(stderr,"  FAIL %s:%d [%s]: |%.6f-%.6f|>%.6f\n",         \
            __FILE__,__LINE__,g_currentTest,_da,_db,_de); }                   \
} while (0)
#define CHECK(c) do { ++g_checks; if(!(c)){ ++g_failures;                     \
    std::fprintf(stderr,"  FAIL %s:%d [%s]: %s\n",__FILE__,__LINE__,          \
        g_currentTest,#c); } } while (0)
#define TEST(n) g_currentTest=n; std::printf("  test: %s\n", n)

// --- tiny SMF builder ------------------------------------------------
static void putVar(std::vector<uint8_t> &v, uint32_t n) {
    uint8_t buf[4]; int c = 0;
    buf[c++] = n & 0x7F;
    while ((n >>= 7)) buf[c++] = (n & 0x7F) | 0x80;
    for (int k = c - 1; k >= 0; --k) v.push_back(buf[k]);   // big-endian VLQ
}
static void put32(std::vector<uint8_t> &v, uint32_t n) {
    v.push_back(n >> 24); v.push_back(n >> 16); v.push_back(n >> 8); v.push_back(n);
}
static void put16(std::vector<uint8_t> &v, uint16_t n) {
    v.push_back(n >> 8); v.push_back(n);
}

// A format-0 SMF, one track: a note-on at tick 0, then End-Of-Track whose
// absolute tick == endTick. initialLoopBeats() must return endTick / division.
static std::vector<uint8_t> makeSmf(uint16_t division, uint32_t endTick) {
    std::vector<uint8_t> trk;
    // delta 0: Note On ch1 C4 vel64
    putVar(trk, 0); trk.push_back(0x90); trk.push_back(60); trk.push_back(64);
    // delta endTick: End Of Track (FF 2F 00)
    putVar(trk, endTick); trk.push_back(0xFF); trk.push_back(0x2F); trk.push_back(0x00);

    std::vector<uint8_t> f;
    f.push_back('M'); f.push_back('T'); f.push_back('h'); f.push_back('d');
    put32(f, 6); put16(f, 0 /*fmt*/); put16(f, 1 /*ntrk*/); put16(f, division);
    f.push_back('M'); f.push_back('T'); f.push_back('r'); f.push_back('k');
    put32(f, (uint32_t)trk.size());
    f.insert(f.end(), trk.begin(), trk.end());
    return f;
}

static double loopBeatsOf(uint16_t div, uint32_t endTick) {
    auto f = makeSmf(div, endTick);
    return initialLoopBeats(f.data(), f.size());
}

// --- tests -----------------------------------------------------------
static void test_common_meters() {
    TEST("exact loop length for common meters (div=96)");
    CHECK_NEAR(loopBeatsOf(96, 96 * 4),  4.0, 1e-9);   // 4/4, 1 bar
    CHECK_NEAR(loopBeatsOf(96, 96 * 3),  3.0, 1e-9);   // 3/4, 1 bar (== 6/8 in quarters)
    CHECK_NEAR(loopBeatsOf(96, 96 * 5),  5.0, 1e-9);   // 5/4, 1 bar
    CHECK_NEAR(loopBeatsOf(96, 96 * 16), 16.0, 1e-9);  // 4/4, 4 bars
}

static void test_fractional_meters() {
    TEST("fractional loop length (the case the old int-round rule broke)");
    CHECK_NEAR(loopBeatsOf(96, 96 * 7 / 2), 3.5, 1e-9);   // 7/8, 1 bar  = 3.5 q-beats
    CHECK_NEAR(loopBeatsOf(96, 96 * 5 / 2), 2.5, 1e-9);   // 5/8, 1 bar  = 2.5 q-beats
    CHECK_NEAR(loopBeatsOf(480, 480 * 7 / 2), 3.5, 1e-9); // same at a finer PPQN
}

static void test_bad_input() {
    TEST("unparseable/empty -> 0.0 (caller falls back)");
    CHECK(initialLoopBeats(nullptr, 0) == 0.0);
    uint8_t junk[8] = {0};
    CHECK(initialLoopBeats(junk, sizeof junk) == 0.0);
    CHECK_NEAR(loopBeatsOf(0, 384), 0.0, 1e-9);           // division 0 -> reject
}

static void test_snap_half() {
    TEST("snapLoopBeatsHalf rounds lossy lengths to nearest 0.5");
    CHECK_NEAR(snapLoopBeatsHalf(3.98), 4.0, 1e-9);
    CHECK_NEAR(snapLoopBeatsHalf(4.02), 4.0, 1e-9);
    CHECK_NEAR(snapLoopBeatsHalf(3.49), 3.5, 1e-9);   // preserves 7/8
    CHECK_NEAR(snapLoopBeatsHalf(2.51), 2.5, 1e-9);   // preserves 5/8
    CHECK_NEAR(snapLoopBeatsHalf(0.0),  0.0, 1e-9);
    CHECK_NEAR(snapLoopBeatsHalf(0.1),  0.5, 1e-9);   // never collapses to 0
}

int main() {
    std::printf("initialLoopBeats() / snapLoopBeatsHalf() tests\n");
    test_common_meters();
    test_fractional_meters();
    test_bad_input();
    test_snap_half();
    std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    if (g_failures == 0) std::printf("ALL PASS\n");
    return g_failures == 0 ? 0 : 1;
}
