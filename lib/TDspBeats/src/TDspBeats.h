// TDspBeats — umbrella header.
//
// Step-sequencer core for T-DSP drum machines. Pattern grid + BPM/swing
// clock + per-step trigger callbacks. No Teensy Audio library dependency:
// the sketch owns the voices (AudioSynthSimpleDrum, AudioPlaySdWav,
// AudioPlayMemory, or anything else) and hands the sequencer a callback
// that fires them on step events.
//
// Usage pattern:
//
//   tdsp::beats::BeatSequencer g_beats;
//   void onBeatStep(void* ctx, int track, int step, float velocity) {
//       switch (track) {
//           case 0: kickDrum.noteOn(); break;
//           case 1: snareDrum.noteOn(); break;
//           case 2: hatWav.play(hatFile); break;
//           ...
//       }
//   }
//   setup() { g_beats.setOnStepFire(onBeatStep, nullptr); }
//   loop()  { g_beats.tick(micros()); }
//
// All timing is microsecond-domain from the caller's monotonic clock
// (micros() on Arduino). The sequencer does not sleep, block, or
// allocate.
#pragma once

#include "BeatSequencer.h"
