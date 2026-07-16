// SPDX-License-Identifier: MIT
// Host-runnable tests for tdsp::Clock::positionBeats() — the absolute,
// monotonic beat-position accessor that the tick-synced loop players read.
// Compile + run on the dev machine — no Teensy needed (Clock has no Arduino
// dependency, only <stdint.h>):
//
//   g++ -std=c++17 -I../src -o tpb test_position_beats.cpp ../src/Clock.cpp
//   ./tpb
//
// Exit code 0 = all pass; non-zero = at least one assertion failed.
//
// Focus areas (see planning/tick-sync-playback/PLAN.md §2, §10):
//   * position == 0 exactly right after Start (no first-beat jump)
//   * smooth, MONOTONIC ramp across tick AND beat edges (the tearing risk)
//   * 1 beat of wall time -> ~1.0 beats; 8 beats -> ~8.0 (tempo accuracy)
//   * frozen when stopped
//   * loop wrap fmod(position, loopBeats) is drift-free for fractional
//     lengths (3.0 for 6/8, 3.5 for 7/8) over many wraps

#include <cstdio>
#include <cstdint>
#include <cmath>

#include "Clock.h"   // real source under test

// --- minimal assert runner (same shape as lib/TDspMPE/test) -----------
static int g_failures = 0;
static int g_checks   = 0;
static const char *g_currentTest = "(none)";

#define CHECK(cond) do {                                                   \
    ++g_checks;                                                            \
    if (!(cond)) {                                                         \
        ++g_failures;                                                      \
        std::fprintf(stderr, "  FAIL %s:%d in [%s]: %s\n",                 \
                     __FILE__, __LINE__, g_currentTest, #cond);            \
    }                                                                      \
} while (0)

#define CHECK_NEAR(a, b, eps) do {                                         \
    ++g_checks;                                                            \
    double _da = (a), _db = (b), _de = (eps);                              \
    if (std::fabs(_da - _db) > _de) {                                      \
        ++g_failures;                                                      \
        std::fprintf(stderr,                                               \
            "  FAIL %s:%d in [%s]: |%.6f - %.6f| > %.6f\n",                \
            __FILE__, __LINE__, g_currentTest, _da, _db, _de);            \
    }                                                                      \
} while (0)

#define TEST(name) g_currentTest = name; std::printf("  test: %s\n", name)

// --- harness helpers --------------------------------------------------

// Bring a Clock up in Internal mode at `bpm`, zeroed at Start. `now` is the
// wall-clock micros cursor the caller advances.
static void startClock(tdsp::Clock &c, float bpm, uint32_t &now) {
    c.setSource(tdsp::Clock::Internal);
    c.setInternalBpm(bpm);
    c.onMidiStart();      // zero tick count, arm the downbeat
    now = 1'000'000;      // arbitrary non-zero epoch
    c.update(now);        // seeds _lastTickMicros; positionBeats() == 0 here
}

// Step the clock forward to `targetUs` in `stepUs` chunks (simulating loop()
// calling update() repeatedly), asserting positionBeats() never decreases.
// Returns the final position.
static double advanceMonotonic(tdsp::Clock &c, uint32_t &now,
                               uint32_t targetUs, uint32_t stepUs) {
    double prev = c.positionBeats();
    while (now < targetUs) {
        uint32_t next = now + stepUs;
        if (next > targetUs) next = targetUs;
        now = next;
        c.update(now);
        double p = c.positionBeats();
        CHECK(p >= prev - 1e-9);     // monotonic (allow FP noise)
        prev = p;
    }
    return prev;
}

// --- tests ------------------------------------------------------------

static void test_zero_after_start() {
    TEST("position is exactly 0 right after Start");
    tdsp::Clock c; uint32_t now;
    startClock(c, 120.0f, now);
    CHECK(c.positionBeats() == 0.0);        // no forward interpolation yet
    CHECK(c.beatCount() == 0);
}

static void test_one_beat_of_time() {
    TEST("120 BPM: 500 ms of wall time -> ~1.0 beat");
    tdsp::Clock c; uint32_t now;
    startClock(c, 120.0f, now);
    // 120 BPM -> a quarter note = 500 ms.
    advanceMonotonic(c, now, now + 500'000, 1000 /*1 ms loop*/);
    CHECK_NEAR(c.positionBeats(), 1.0, 0.02);
}

static void test_eight_beats_monotonic() {
    TEST("120 BPM: 8 beats, strictly monotonic across every tick/beat edge");
    tdsp::Clock c; uint32_t now;
    startClock(c, 120.0f, now);
    // 8 beats = 4 s. Step in 250 us chunks so we sample WITHIN ticks
    // (perTick ~= 20833 us) and land on both tick and beat boundaries.
    double end = advanceMonotonic(c, now, now + 4'000'000, 250);
    CHECK_NEAR(end, 8.0, 0.03);
}

static void test_interpolation_midbeat() {
    TEST("position interpolates smoothly mid-beat (~0.5 at 250 ms)");
    tdsp::Clock c; uint32_t now;
    startClock(c, 120.0f, now);
    advanceMonotonic(c, now, now + 250'000, 500);
    CHECK_NEAR(c.positionBeats(), 0.5, 0.03);
}

static void test_frozen_when_stopped() {
    TEST("position is frozen while stopped");
    tdsp::Clock c; uint32_t now;
    startClock(c, 120.0f, now);
    advanceMonotonic(c, now, now + 1'000'000, 1000);   // ~2 beats
    double atStop = c.positionBeats();
    c.onMidiStop();
    // Time marches on, but a stopped clock must not advance position.
    for (int i = 0; i < 50; ++i) { now += 10'000; c.update(now); }
    CHECK_NEAR(c.positionBeats(), atStop, 1e-9);
}

static void test_tempo_scales_position() {
    TEST("240 BPM covers twice the beats of 120 BPM in the same wall time");
    uint32_t now;
    tdsp::Clock slow; startClock(slow, 120.0f, now);
    advanceMonotonic(slow, now, now + 1'000'000, 500);   // 1 s @120 -> 2 beats
    double slowPos = slow.positionBeats();

    tdsp::Clock fast; startClock(fast, 240.0f, now);
    advanceMonotonic(fast, now, now + 1'000'000, 500);   // 1 s @240 -> 4 beats
    double fastPos = fast.positionBeats();

    CHECK_NEAR(slowPos, 2.0, 0.03);
    CHECK_NEAR(fastPos, 4.0, 0.05);
    CHECK_NEAR(fastPos, slowPos * 2.0, 0.1);
}

// The whole point of Option B: fmod against an exact fractional loop length
// wraps with zero cumulative drift, whatever the meter. Verify the wrap phase
// of two different loop lengths stays coherent over many bars.
static void test_fmod_wrap_driftfree() {
    TEST("fmod(position, loopBeats) drift-free for 4.0 and 3.5 (7/8) lengths");
    tdsp::Clock c; uint32_t now;
    startClock(c, 120.0f, now);

    const double loopA = 4.0;   // 4/4, 1 bar
    const double loopB = 3.5;   // 7/8, 1 bar (fractional — the old int rule broke this)

    // Advance ~64 beats (16 bars of 4/4, ~18.28 bars of 7/8) and, at the very
    // end, check the wrap phase equals fmod of the absolute position exactly.
    // Because position is computed absolutely (not accumulated per wrap), the
    // phase carries no rounding history — that IS the drift-free guarantee.
    double pos = advanceMonotonic(c, now, now + 32'000'000, 500);  // 64 beats
    CHECK_NEAR(pos, 64.0, 0.2);

    double phaseA = std::fmod(pos, loopA);
    double phaseB = std::fmod(pos, loopB);
    CHECK(phaseA >= 0.0 && phaseA < loopA);
    CHECK(phaseB >= 0.0 && phaseB < loopB);
    // 64.0 / 4.0 = 16 exact -> phaseA ~ 0. 64.0 / 3.5 = 18.2857 -> phaseB ~ 1.0.
    CHECK_NEAR(phaseA, 0.0, 0.2);
    CHECK_NEAR(phaseB, 1.0, 0.2);
}

int main() {
    std::printf("Clock::positionBeats() tests\n");
    test_zero_after_start();
    test_one_beat_of_time();
    test_eight_beats_monotonic();
    test_interpolation_midbeat();
    test_frozen_when_stopped();
    test_tempo_scales_position();
    test_fmod_wrap_driftfree();

    std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    if (g_failures == 0) std::printf("ALL PASS\n");
    return g_failures == 0 ? 0 : 1;
}
