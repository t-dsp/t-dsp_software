// SPDX-License-Identifier: MIT
// Host-runnable tests for MpeVaSink's voice allocator and MIDI event
// handling. Compile + run on the dev machine — no Teensy needed:
//
//   g++ -std=c++17 -Wall -Wextra -O1 \
//       -I../src \
//       -I./mocks \
//       -I../../TDspMidi/src \
//       -o test_voice_allocator \
//       test_voice_allocator.cpp ../src/MpeVaSink.cpp
//   ./test_voice_allocator
//
// Exit code 0 = all pass; non-zero = at least one assertion failed.
// The test runner prints every test name with PASS / FAIL and a summary.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

#include "Audio.h"        // mock
#include "MpeVaSink.h"    // real source under test

// --- minimal assert macros --------------------------------------------
//
// Chose a hand-rolled mini-runner over GoogleTest / Unity because the
// test surface is tiny, there are no dependencies to manage on every
// dev machine, and `g++ one_file.cpp` is the whole toolchain story.

static int g_failures = 0;
static int g_checks   = 0;
static const char *g_currentTest = "(none)";

#define CHECK(cond) do {                                                   \
    ++g_checks;                                                            \
    if (!(cond)) {                                                         \
        ++g_failures;                                                      \
        std::fprintf(stderr,                                               \
            "  FAIL %s:%d in [%s]: %s\n",                                  \
            __FILE__, __LINE__, g_currentTest, #cond);                     \
    }                                                                      \
} while (0)

#define CHECK_EQ(a, b)   CHECK((a) == (b))
#define CHECK_NEAR(a, b, eps) CHECK(std::fabs((double)(a) - (double)(b)) < (eps))

struct TestCase {
    const char *name;
    void (*fn)();
};

static void runTest(const TestCase &tc) {
    g_currentTest = tc.name;
    const int startFailures = g_failures;
    tc.fn();
    const bool passed = (g_failures == startFailures);
    std::printf("  %s  %s\n", passed ? "PASS" : "FAIL", tc.name);
}

// --- fixture ---------------------------------------------------------
//
// Every test builds its own fresh sink + voice array so cross-test
// contamination is impossible. Four voices is the default; tests that
// need a different count construct their own.

struct Fixture {
    AudioSynthWaveform       oscs[4];
    AudioEffectEnvelope      envs[4];
    AudioFilterStateVariable filters[4];
    MpeVaSink::VoicePorts    ports[4];
    MpeVaSink                sink;

    Fixture()
        : ports{
            {&oscs[0], &envs[0], &filters[0]},
            {&oscs[1], &envs[1], &filters[1]},
            {&oscs[2], &envs[2], &filters[2]},
            {&oscs[3], &envs[3], &filters[3]},
          },
          sink(ports, 4)
    {}
};

// --- tests -----------------------------------------------------------

static void test_ctor_seeds_voices() {
    Fixture f;
    // Every voice's osc should have been begin()'d once with the default
    // waveform (saw = WAVEFORM_SAWTOOTH), amplitude set to 0 (silence
    // before first note), frequency at 440 Hz placeholder.
    for (int i = 0; i < 4; ++i) {
        CHECK_EQ(f.oscs[i].beginCalls, 1);
        CHECK_EQ(f.oscs[i].lastToneType, WAVEFORM_SAWTOOTH);
        CHECK_NEAR(f.oscs[i].lastAmplitude, 0.0f, 1e-6);
        CHECK_NEAR(f.oscs[i].lastFrequency, 440.0f, 1e-3);
        // Envelope A/R seeded (5 ms / 300 ms from defaults).
        CHECK_NEAR(f.envs[i].lastAttackMs, 5.0f, 1e-3);
        CHECK_NEAR(f.envs[i].lastReleaseMs, 300.0f, 1e-3);
    }
}

static void test_note_on_allocates_first_voice() {
    Fixture f;
    f.sink.onNoteOn(/*channel=*/2, /*note=*/69, /*velocity=*/100);
    // Voice 0 should get the note. All voices were idle; allocator
    // picks the lowest start_time, tied at 0 — tie-break is index order.
    CHECK_EQ(f.envs[0].noteOnCount, 1);
    // A4 = 440 Hz.
    CHECK_NEAR(f.oscs[0].lastFrequency, 440.0f, 1e-3);
    // Amplitude = velocity/127 × volumeScale × (0.5 + 0.5×pressure),
    // pressure starts at 0, volumeScale=1 → 100/127 × 1 × 0.5 = ~0.394.
    CHECK_NEAR(f.oscs[0].lastAmplitude, (100.0f / 127.0f) * 0.5f, 1e-3);
}

static void test_master_channel_note_ignored() {
    Fixture f;
    // Default master = 1. Notes on channel 1 must NOT allocate a voice.
    f.sink.onNoteOn(1, 60, 100);
    for (int i = 0; i < 4; ++i) {
        CHECK_EQ(f.envs[i].noteOnCount, 0);
    }
}

static void test_configurable_master_channel() {
    Fixture f;
    f.sink.setMasterChannel(5);
    CHECK_EQ(f.sink.masterChannel(), 5);
    f.sink.onNoteOn(5, 60, 100);
    for (int i = 0; i < 4; ++i) {
        CHECK_EQ(f.envs[i].noteOnCount, 0);
    }
    // Channel 1 is now a member channel and should allocate.
    f.sink.onNoteOn(1, 60, 100);
    CHECK_EQ(f.envs[0].noteOnCount, 1);
}

static void test_four_simultaneous_voices_fill_all_slots() {
    Fixture f;
    f.sink.onNoteOn(2, 60, 100);
    f.sink.onNoteOn(3, 62, 100);
    f.sink.onNoteOn(4, 64, 100);
    f.sink.onNoteOn(5, 65, 100);
    for (int i = 0; i < 4; ++i) {
        CHECK_EQ(f.envs[i].noteOnCount, 1);
    }
}

static void test_note_off_matches_channel_and_note() {
    Fixture f;
    f.sink.onNoteOn(2, 60, 100);
    f.sink.onNoteOn(3, 60, 100);  // same note, different channel
    // Release only the channel-3 instance. Voice 1 should get noteOff.
    f.sink.onNoteOff(3, 60, 0);
    CHECK_EQ(f.envs[0].noteOffCount, 0);
    CHECK_EQ(f.envs[1].noteOffCount, 1);
}

static void test_note_off_wrong_channel_is_noop() {
    Fixture f;
    f.sink.onNoteOn(2, 60, 100);
    f.sink.onNoteOff(5, 60, 0);  // note was on ch 2, off on ch 5
    CHECK_EQ(f.envs[0].noteOffCount, 0);
}

static void test_lru_reuse_of_released_voice() {
    Fixture f;
    // Fill all voices, then release voice 0 → voice 1 → voice 2,
    // leaving voice 3 still held. Next note-on should pick voice 0
    // (oldest released = oldest start_time).
    f.sink.onNoteOn(2, 60, 100);
    f.sink.onNoteOn(3, 61, 100);
    f.sink.onNoteOn(4, 62, 100);
    f.sink.onNoteOn(5, 63, 100);
    f.sink.onNoteOff(2, 60, 0);
    f.sink.onNoteOff(3, 61, 0);
    f.sink.onNoteOff(4, 62, 0);
    // A 5th note should take voice 0 (oldest released start_time).
    f.sink.onNoteOn(6, 72, 100);
    CHECK_EQ(f.envs[0].noteOnCount, 2);  // first note-on + re-trigger
}

static void test_steal_lru_when_all_held() {
    Fixture f;
    f.sink.onNoteOn(2, 60, 100);  // voice 0, start_time=1
    f.sink.onNoteOn(3, 61, 100);  // voice 1, start_time=2
    f.sink.onNoteOn(4, 62, 100);  // voice 2, start_time=3
    f.sink.onNoteOn(5, 63, 100);  // voice 3, start_time=4
    // A 5th note with all held → steal LRU (voice 0, start_time=1).
    f.sink.onNoteOn(6, 72, 100);
    CHECK_EQ(f.envs[0].noteOnCount, 2);  // retriggered
    CHECK_EQ(f.envs[1].noteOnCount, 1);
    CHECK_EQ(f.envs[2].noteOnCount, 1);
    CHECK_EQ(f.envs[3].noteOnCount, 1);
}

static void test_pitch_bend_only_affects_matching_channel() {
    Fixture f;
    f.sink.onNoteOn(2, 69, 100);  // A4 on ch 2 → voice 0
    f.sink.onNoteOn(3, 69, 100);  // A4 on ch 3 → voice 1
    const float freq0Before = f.oscs[0].lastFrequency;
    const float freq1Before = f.oscs[1].lastFrequency;
    // +12 semitones on channel 2 — voice 0 should move to 880 Hz,
    // voice 1 should be untouched.
    f.sink.onPitchBend(2, 12.0f);
    CHECK_NEAR(f.oscs[0].lastFrequency, 880.0f, 1e-1);
    CHECK_NEAR(f.oscs[1].lastFrequency, freq1Before, 1e-3);
    (void)freq0Before;
}

static void test_pressure_scales_amplitude_in_half_to_full_range() {
    Fixture f;
    f.sink.onNoteOn(2, 60, 127);  // full velocity
    // Pressure 0 → base × (0.5 + 0) = base × 0.5.
    const float base = 127.0f / 127.0f;
    CHECK_NEAR(f.oscs[0].lastAmplitude, base * 0.5f, 1e-3);
    // Pressure 1 → base × 1.0.
    f.sink.onPressure(2, 1.0f);
    CHECK_NEAR(f.oscs[0].lastAmplitude, base * 1.0f, 1e-3);
    // Pressure 0.5 → base × 0.75.
    f.sink.onPressure(2, 0.5f);
    CHECK_NEAR(f.oscs[0].lastAmplitude, base * 0.75f, 1e-3);
}

static void test_all_notes_off_channel_zero_panics_everything() {
    Fixture f;
    f.sink.onNoteOn(2, 60, 100);
    f.sink.onNoteOn(3, 61, 100);
    f.sink.onAllNotesOff(0);  // panic convention
    CHECK_EQ(f.envs[0].noteOffCount, 1);
    CHECK_EQ(f.envs[1].noteOffCount, 1);
    CHECK_EQ(f.envs[2].noteOffCount, 1);
    CHECK_EQ(f.envs[3].noteOffCount, 1);
}

static void test_all_notes_off_specific_channel_spares_others() {
    Fixture f;
    f.sink.onNoteOn(2, 60, 100);  // voice 0
    f.sink.onNoteOn(3, 61, 100);  // voice 1
    f.sink.onAllNotesOff(2);
    CHECK_EQ(f.envs[0].noteOffCount, 1);
    CHECK_EQ(f.envs[1].noteOffCount, 0);
}

static void test_waveform_update_propagates_to_all_oscs() {
    Fixture f;
    f.sink.setWaveform(3);  // 3 = sine in our OSC numbering
    for (int i = 0; i < 4; ++i) {
        // beginCalls: 1 from ctor + 1 from setWaveform = 2.
        CHECK_EQ(f.oscs[i].beginCalls, 2);
        CHECK_EQ(f.oscs[i].lastToneType, WAVEFORM_SINE);
    }
    CHECK_EQ(f.sink.waveform(), 3);
}

static void test_waveform_out_of_range_clamps_to_saw() {
    Fixture f;
    f.sink.setWaveform(99);
    CHECK_EQ(f.sink.waveform(), 0);
    for (int i = 0; i < 4; ++i) {
        CHECK_EQ(f.oscs[i].lastToneType, WAVEFORM_SAWTOOTH);
    }
}

static void test_attack_release_convert_seconds_to_ms() {
    Fixture f;
    f.sink.setAttack (0.050f);
    f.sink.setRelease(0.250f);
    for (int i = 0; i < 4; ++i) {
        CHECK_NEAR(f.envs[i].lastAttackMs,  50.0f,  1e-3);
        CHECK_NEAR(f.envs[i].lastReleaseMs, 250.0f, 1e-3);
    }
    CHECK_NEAR(f.sink.attack(),  0.050f, 1e-6);
    CHECK_NEAR(f.sink.release(), 0.250f, 1e-6);
}

static void test_velocity_zero_produces_silent_voice() {
    Fixture f;
    f.sink.onNoteOn(2, 60, 0);  // MIDI running-status idiom is usually
                                // handled by the router, but the sink
                                // should tolerate velocity=0 without
                                // blowing up.
    CHECK_NEAR(f.oscs[0].lastAmplitude, 0.0f, 1e-6);
}

static void test_out_of_range_channel_ignored() {
    Fixture f;
    f.sink.onNoteOn(0,  60, 100);  // 0 is not a valid MIDI channel
    f.sink.onNoteOn(17, 60, 100);  // beyond 16
    for (int i = 0; i < 4; ++i) {
        CHECK_EQ(f.envs[i].noteOnCount, 0);
    }
}

// --- filter tests (Phase 2d2) -----------------------------------------

static void test_filter_seeded_at_ctor() {
    Fixture f;
    // All voices should have received the default cutoff + resonance.
    for (int i = 0; i < 4; ++i) {
        CHECK_NEAR(f.filters[i].lastFrequency, 8000.0f, 1e-3);
        CHECK_NEAR(f.filters[i].lastResonance, 0.707f, 1e-3);
    }
}

static void test_filter_cutoff_propagates() {
    Fixture f;
    f.sink.setFilterCutoff(2000.0f);
    CHECK_NEAR(f.sink.filterCutoff(), 2000.0f, 1e-3);
    // applyFilter(i) multiplies by 2^(0.5*2 - 1) = 1 for the default
    // neutral timbre of 0.5, so lastFrequency should equal base cutoff.
    for (int i = 0; i < 4; ++i) {
        CHECK_NEAR(f.filters[i].lastFrequency, 2000.0f, 1e-2);
    }
}

static void test_filter_resonance_clamps_and_propagates() {
    Fixture f;
    f.sink.setFilterResonance(10.0f);          // clamped to 5.0
    CHECK_NEAR(f.sink.filterResonance(), 5.0f, 1e-3);
    for (int i = 0; i < 4; ++i) {
        CHECK_NEAR(f.filters[i].lastResonance, 5.0f, 1e-3);
    }
    f.sink.setFilterResonance(0.1f);           // clamped to 0.707
    CHECK_NEAR(f.sink.filterResonance(), 0.707f, 1e-3);
}

static void test_cc74_opens_filter_on_matching_channel_only() {
    Fixture f;
    f.sink.setFilterCutoff(1000.0f);
    f.sink.onNoteOn(2, 60, 100);  // voice 0 on ch 2
    f.sink.onNoteOn(3, 62, 100);  // voice 1 on ch 3
    // Base cutoff × 2^(0.5*2-1) = base × 1 = 1000 Hz at neutral timbre.
    CHECK_NEAR(f.filters[0].lastFrequency, 1000.0f, 1e-2);
    CHECK_NEAR(f.filters[1].lastFrequency, 1000.0f, 1e-2);
    // CC#74 on channel 2 full up → voice 0's cutoff × 2 = 2000 Hz,
    // voice 1 untouched.
    f.sink.onTimbre(2, 1.0f);
    CHECK_NEAR(f.filters[0].lastFrequency, 2000.0f, 1.0f);
    CHECK_NEAR(f.filters[1].lastFrequency, 1000.0f, 1e-2);
    // CC#74 on channel 3 to 0 → voice 1's cutoff × 0.5 = 500 Hz, voice 0 stable.
    f.sink.onTimbre(3, 0.0f);
    CHECK_NEAR(f.filters[1].lastFrequency, 500.0f, 1.0f);
}

static void test_cc74_while_no_voice_held_updates_channel_cache() {
    Fixture f;
    f.sink.setFilterCutoff(1000.0f);
    // Set CC#74 high BEFORE pressing any note on that channel. The
    // channel state caches the value; the next note-on inherits it.
    f.sink.onTimbre(4, 1.0f);
    f.sink.onNoteOn(4, 60, 100);
    // Voice 0 should open to ~2000 Hz immediately on note-on, not sit
    // at neutral until the next CC#74 update.
    CHECK_NEAR(f.filters[0].lastFrequency, 2000.0f, 1.0f);
}

static void test_cc74_ignores_released_voices() {
    Fixture f;
    f.sink.setFilterCutoff(1000.0f);
    f.sink.onNoteOn(2, 60, 100);
    f.sink.onNoteOff(2, 60, 0);
    const float before = f.filters[0].lastFrequency;
    const int   calls  = f.filters[0].frequencyCalls;
    f.sink.onTimbre(2, 1.0f);  // voice 0 is now released
    // The release tail should keep its last cutoff — re-opening a
    // dying note's filter sounds wrong.
    CHECK_EQ(f.filters[0].frequencyCalls, calls);
    CHECK_NEAR(f.filters[0].lastFrequency, before, 1e-6);
}

// --- LFO tests (Phase 2d3) --------------------------------------------

static void test_lfo_off_does_nothing_on_tick() {
    Fixture f;
    f.sink.onNoteOn(2, 60, 100);
    const int  freqCalls = f.oscs[0].frequencyCalls;
    const int  ampCalls  = f.oscs[0].amplitudeCalls;
    const int  filtCalls = f.filters[0].frequencyCalls;
    // Dest defaults to OFF. tick() should not touch anything.
    f.sink.tick(0);
    f.sink.tick(50);
    f.sink.tick(100);
    CHECK_EQ(f.oscs[0].frequencyCalls,    freqCalls);
    CHECK_EQ(f.oscs[0].amplitudeCalls,    ampCalls);
    CHECK_EQ(f.filters[0].frequencyCalls, filtCalls);
}

static void test_lfo_pitch_destination_modulates_frequency() {
    Fixture f;
    f.sink.onNoteOn(2, 69, 127);  // A4 = 440 Hz, full velocity
    f.sink.setLfoRate(2.0f);      // 500 ms period
    f.sink.setLfoDepth(0.5f);     // ±6 semitones at this depth
    f.sink.setLfoDest(MpeVaSink::LfoPitch);
    f.sink.setLfoWaveform(0);     // sine

    // At t=0 → phase 0 → sin(0)=0 → no offset → 440 Hz.
    f.sink.tick(0);
    CHECK_NEAR(f.oscs[0].lastFrequency, 440.0f, 1.0f);
    // At t=125 ms (quarter period) → phase 0.25 → sin(π/2)=+1 →
    // offset = +1 × 0.5 × 12 = +6 semitones → 440 × 2^(6/12) ≈ 622 Hz.
    f.sink.tick(125);
    CHECK_NEAR(f.oscs[0].lastFrequency, 622.25f, 2.0f);
    // At t=375 ms (three-quarter period) → phase 0.75 → sin(3π/2)=−1
    // → offset = −6 → 440 × 2^(−6/12) ≈ 311 Hz.
    f.sink.tick(375);
    CHECK_NEAR(f.oscs[0].lastFrequency, 311.13f, 2.0f);
}

static void test_lfo_cutoff_destination_modulates_filter() {
    Fixture f;
    f.sink.setFilterCutoff(1000.0f);
    f.sink.onNoteOn(2, 60, 100);
    f.sink.setLfoRate(1.0f);      // 1000 ms period
    f.sink.setLfoDepth(1.0f);     // ±1 octave at this depth (full scale)
    f.sink.setLfoDest(MpeVaSink::LfoCutoff);
    f.sink.setLfoWaveform(2);     // saw

    // Saw: phase 0 → shape −1 → cutoff × 2^(−1) = 500 Hz.
    f.sink.tick(0);
    CHECK_NEAR(f.filters[0].lastFrequency, 500.0f, 2.0f);
    // Phase 0.5 → shape 0 → cutoff × 1 = 1000 Hz.
    f.sink.tick(500);
    CHECK_NEAR(f.filters[0].lastFrequency, 1000.0f, 2.0f);
    // Phase 0.999… → shape +1 → cutoff × 2 = 2000 Hz (approx).
    f.sink.tick(999);
    CHECK_NEAR(f.filters[0].lastFrequency, 2000.0f, 10.0f);
}

static void test_lfo_amp_destination_tremolos_amplitude() {
    Fixture f;
    f.sink.onNoteOn(2, 60, 127);  // full velocity, base_amp = 1.0
    f.sink.setLfoRate(1.0f);
    f.sink.setLfoDepth(0.5f);
    f.sink.setLfoDest(MpeVaSink::LfoAmp);
    f.sink.setLfoWaveform(3);     // square: ±1 alternating

    // Square first half → shape +1 → mult = 1 − 0.5 × (1 − 1) = 1 →
    // amp = 1.0 × (0.5 + 0.5×0) × 1 = 0.5 (pressure=0 at note-on).
    f.sink.tick(0);
    const float peak = f.oscs[0].lastAmplitude;
    CHECK_NEAR(peak, 0.5f, 1e-3);
    // Square second half → shape −1 → mult = 1 − 0.5 × (1 − 0) = 0.5
    // → amp = 0.5 × 0.5 = 0.25.
    f.sink.tick(600);
    CHECK_NEAR(f.oscs[0].lastAmplitude, 0.25f, 1e-3);
}

static void test_lfo_rate_zero_disables() {
    Fixture f;
    f.sink.onNoteOn(2, 60, 100);
    const int calls = f.oscs[0].frequencyCalls;
    f.sink.setLfoDepth(1.0f);
    f.sink.setLfoDest(MpeVaSink::LfoPitch);
    f.sink.setLfoRate(0.0f);  // zero rate is the kill-switch
    f.sink.tick(100);
    CHECK_EQ(f.oscs[0].frequencyCalls, calls);
}

static void test_lfo_depth_zero_disables() {
    Fixture f;
    f.sink.onNoteOn(2, 60, 100);
    const int calls = f.oscs[0].frequencyCalls;
    f.sink.setLfoRate(2.0f);
    f.sink.setLfoDest(MpeVaSink::LfoPitch);
    f.sink.setLfoDepth(0.0f);
    f.sink.tick(100);
    CHECK_EQ(f.oscs[0].frequencyCalls, calls);
}

static void test_lfo_dest_change_resets_contribution() {
    Fixture f;
    f.sink.setFilterCutoff(1000.0f);
    f.sink.onNoteOn(2, 69, 100);  // A4
    f.sink.setLfoRate(1.0f);
    f.sink.setLfoDepth(0.5f);
    f.sink.setLfoDest(MpeVaSink::LfoPitch);
    f.sink.tick(125);  // LFO is now actively bending pitch
    CHECK(f.oscs[0].lastFrequency != 440.0f);
    // Switch to OFF — voice frequency should snap back to natural 440 Hz.
    f.sink.setLfoDest(MpeVaSink::LfoOff);
    CHECK_NEAR(f.oscs[0].lastFrequency, 440.0f, 1.0f);
}

static void test_lfo_does_not_touch_released_voices() {
    Fixture f;
    f.sink.onNoteOn(2, 60, 100);
    f.sink.onNoteOff(2, 60, 0);
    const int calls = f.oscs[0].frequencyCalls;
    f.sink.setLfoRate(5.0f);
    f.sink.setLfoDepth(1.0f);
    f.sink.setLfoDest(MpeVaSink::LfoPitch);
    for (uint32_t t = 0; t < 500; t += 25) f.sink.tick(t);
    // No frequency writes to a released voice — the tail should
    // continue un-modulated.
    CHECK_EQ(f.oscs[0].frequencyCalls, calls);
}

// --- Voice telemetry tests (Phase 2d4) --------------------------------

static void test_voice_snapshot_reflects_held_state() {
    Fixture f;
    f.sink.onNoteOn(3, 60, 100);
    MpeVaSink::VoiceSnapshot snap[4];
    const int n = f.sink.voiceSnapshot(snap, 4);
    CHECK_EQ(n, 4);
    CHECK(snap[0].held);
    CHECK_EQ(snap[0].channel, 3);
    CHECK_EQ(snap[0].note,    60);
    CHECK(!snap[1].held);
    CHECK(!snap[2].held);
    CHECK(!snap[3].held);
}

static void test_voice_snapshot_exposes_bend_and_lfo_combined() {
    Fixture f;
    f.sink.onNoteOn(2, 60, 100);
    f.sink.onPitchBend(2, 3.0f);  // +3 semi
    f.sink.setLfoRate(2.0f);
    f.sink.setLfoDepth(0.25f);
    f.sink.setLfoDest(MpeVaSink::LfoPitch);
    f.sink.tick(125);  // sine phase 0.25 → +1 → +0.25×12 = +3 semi LFO
    MpeVaSink::VoiceSnapshot snap[4];
    f.sink.voiceSnapshot(snap, 4);
    // Combined pitch = bend + LFO = +3 + +3 = +6 semi.
    CHECK_NEAR(snap[0].pitchSemi, 6.0f, 0.1f);
}

static void test_voice_snapshot_carries_pressure_and_timbre() {
    Fixture f;
    f.sink.onNoteOn(2, 60, 100);
    f.sink.onPressure(2, 0.8f);
    f.sink.onTimbre(2, 0.3f);
    MpeVaSink::VoiceSnapshot snap[4];
    f.sink.voiceSnapshot(snap, 4);
    CHECK_NEAR(snap[0].pressure, 0.8f, 1e-3);
    CHECK_NEAR(snap[0].timbre,   0.3f, 1e-3);
}

static void test_voice_snapshot_null_pointer_is_safe() {
    Fixture f;
    f.sink.onNoteOn(2, 60, 100);
    const int n = f.sink.voiceSnapshot(nullptr, 0);
    CHECK_EQ(n, 0);
}

static void test_filter_null_pointer_is_safe() {
    // Construct a sink where one voice has no filter wired (legacy
    // Phase 2d setup). The sink must not crash; filter writes for that
    // voice are silently skipped while other voices work.
    AudioSynthWaveform       oscs[2];
    AudioEffectEnvelope      envs[2];
    AudioFilterStateVariable filter1;
    MpeVaSink::VoicePorts    ports[2] = {
        {&oscs[0], &envs[0], nullptr},
        {&oscs[1], &envs[1], &filter1},
    };
    MpeVaSink sink(ports, 2);
    sink.setFilterCutoff(1000.0f);
    sink.onNoteOn(2, 60, 100);       // voice 0, no filter
    sink.onNoteOn(3, 62, 100);       // voice 1, filter present
    sink.onTimbre(2, 1.0f);
    sink.onTimbre(3, 1.0f);
    // No assertion on voice 0 other than "didn't crash"; voice 1
    // should have tracked the timbre change.
    CHECK_NEAR(filter1.lastFrequency, 2000.0f, 1.0f);
}

// --- main ------------------------------------------------------------

int main() {
    std::printf("MpeVaSink voice allocator tests\n");
    std::printf("===============================\n");

    const TestCase tests[] = {
        {"ctor_seeds_voices",                           test_ctor_seeds_voices},
        {"note_on_allocates_first_voice",               test_note_on_allocates_first_voice},
        {"master_channel_note_ignored",                 test_master_channel_note_ignored},
        {"configurable_master_channel",                 test_configurable_master_channel},
        {"four_simultaneous_voices_fill_all_slots",     test_four_simultaneous_voices_fill_all_slots},
        {"note_off_matches_channel_and_note",           test_note_off_matches_channel_and_note},
        {"note_off_wrong_channel_is_noop",              test_note_off_wrong_channel_is_noop},
        {"lru_reuse_of_released_voice",                 test_lru_reuse_of_released_voice},
        {"steal_lru_when_all_held",                     test_steal_lru_when_all_held},
        {"pitch_bend_only_affects_matching_channel",    test_pitch_bend_only_affects_matching_channel},
        {"pressure_scales_amplitude_in_half_to_full_range", test_pressure_scales_amplitude_in_half_to_full_range},
        {"all_notes_off_channel_zero_panics_everything",test_all_notes_off_channel_zero_panics_everything},
        {"all_notes_off_specific_channel_spares_others",test_all_notes_off_specific_channel_spares_others},
        {"waveform_update_propagates_to_all_oscs",      test_waveform_update_propagates_to_all_oscs},
        {"waveform_out_of_range_clamps_to_saw",         test_waveform_out_of_range_clamps_to_saw},
        {"attack_release_convert_seconds_to_ms",        test_attack_release_convert_seconds_to_ms},
        {"velocity_zero_produces_silent_voice",         test_velocity_zero_produces_silent_voice},
        {"out_of_range_channel_ignored",                test_out_of_range_channel_ignored},
        {"filter_seeded_at_ctor",                       test_filter_seeded_at_ctor},
        {"filter_cutoff_propagates",                    test_filter_cutoff_propagates},
        {"filter_resonance_clamps_and_propagates",      test_filter_resonance_clamps_and_propagates},
        {"cc74_opens_filter_on_matching_channel_only",  test_cc74_opens_filter_on_matching_channel_only},
        {"cc74_while_no_voice_held_updates_channel_cache", test_cc74_while_no_voice_held_updates_channel_cache},
        {"cc74_ignores_released_voices",                test_cc74_ignores_released_voices},
        {"filter_null_pointer_is_safe",                 test_filter_null_pointer_is_safe},
        {"lfo_off_does_nothing_on_tick",                test_lfo_off_does_nothing_on_tick},
        {"lfo_pitch_destination_modulates_frequency",   test_lfo_pitch_destination_modulates_frequency},
        {"lfo_cutoff_destination_modulates_filter",     test_lfo_cutoff_destination_modulates_filter},
        {"lfo_amp_destination_tremolos_amplitude",      test_lfo_amp_destination_tremolos_amplitude},
        {"lfo_rate_zero_disables",                      test_lfo_rate_zero_disables},
        {"lfo_depth_zero_disables",                     test_lfo_depth_zero_disables},
        {"lfo_dest_change_resets_contribution",         test_lfo_dest_change_resets_contribution},
        {"lfo_does_not_touch_released_voices",          test_lfo_does_not_touch_released_voices},
        {"voice_snapshot_reflects_held_state",          test_voice_snapshot_reflects_held_state},
        {"voice_snapshot_exposes_bend_and_lfo_combined",test_voice_snapshot_exposes_bend_and_lfo_combined},
        {"voice_snapshot_carries_pressure_and_timbre",  test_voice_snapshot_carries_pressure_and_timbre},
        {"voice_snapshot_null_pointer_is_safe",         test_voice_snapshot_null_pointer_is_safe},
    };

    const int nTests = (int)(sizeof(tests) / sizeof(tests[0]));
    for (int i = 0; i < nTests; ++i) runTest(tests[i]);

    std::printf("\n%d checks, %d failures across %d tests\n",
                g_checks, g_failures, nTests);
    return g_failures == 0 ? 0 : 1;
}
