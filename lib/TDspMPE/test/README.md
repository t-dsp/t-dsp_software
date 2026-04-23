# lib/TDspMPE host tests

Host-runnable unit tests for `MpeVaSink`'s voice allocator + MIDI event
handling. These compile and run on the developer machine — no Teensy
needed. They cover the logic that's hardest to exercise manually with
a real keyboard: LRU selection, voice stealing, channel-filtered
note-off matching, pitch-bend isolation across channels, panic /
all-notes-off semantics.

## Why a separate host harness

The audio path needs hardware (DAC, USB host, sample rate). The
*allocator* does not — it's pure integer arithmetic on a small fixed
pool. Running those tests on the host:

* catches regressions in seconds instead of after a full `pio run` +
  upload + play-a-test-sequence-by-hand cycle,
* runs in CI cleanly with no Teensy simulator,
* makes bugs in the allocator reproducible without a MIDI controller.

The mock `Audio.h` in `mocks/` stubs `AudioSynthWaveform` and
`AudioEffectEnvelope` down to call-counters + last-value fields, so
tests can assert `f.oscs[0].lastFrequency == 440.0f` instead of
squinting at a scope.

## Running

### Unix / macOS / WSL

```bash
cd lib/TDspMPE/test
bash run_tests.sh
```

### Windows (Git Bash, MSYS, or cmd)

```cmd
cd lib\TDspMPE\test
run_tests.bat
```

The harness needs a host C++ compiler (`g++` with C++17) on `PATH`. On
Windows install MSYS2 / MinGW-w64 and put `mingw64\bin` on `PATH`, or
use WSL. The Teensy cross-compiler (`arm-none-eabi-g++`) does NOT
work — these tests need to produce a native executable, not an ARM
firmware image.

## Adding tests

Append a `test_*` function + its entry in the `tests[]` array in
`test_voice_allocator.cpp`. The `Fixture` struct gives you a fresh
4-voice sink per test; `CHECK(...)`, `CHECK_EQ(a, b)`, and
`CHECK_NEAR(a, b, eps)` are the assertion macros.

## Known gaps

These tests cover the allocator + MIDI event semantics. They do NOT
cover:

* Timing / envelope shape (lives in the real Teensy Audio library;
  not reproduced by the mock).
* Audio-rate behavior (aliasing, oscillator phase continuity, filter
  response).
* The main.cpp wiring — OSC handlers, preMix routing, AudioMemory
  sizing. Those are integration concerns; cover them with on-hardware
  smoke tests.

Phase 2d2 (filter) and 2d3 (LFO) will expand this test file with
cases for per-voice filter cutoff, CC#74 routing, and LFO destination
multiplexing as those features land.
