# Wire the ymfm OPM engine into spike_midi_player (`-D TDSP_SYNTH_YMFM`)

**Goal:** replace the `#error` in `main.cpp`'s `TDSP_SYNTH_YMFM` branch with the
real ymfm backend, so `pio run -e teensy41_ymfm` builds and plays. The engine
already exists and is hardware-verified as a standalone spike
(`projects/spike_midi_ymfm_opm/`, library `lib/TDspYmfm`).

The engine is `AudioSynthYmfmOPM` (Yamaha YM2151 / OPM, 8-voice, **stereo**:
output 0 = L, 1 = R). It is **BSD-3-Clause** — keep Dexed (GPL v3) out of this
build (step 3).

Nothing outside the `SYNTH BACKEND` block in `main.cpp` changes: the parser,
player, SD catalog, codec, and MIDI handlers are engine-independent.

---

## Step 1 — add `src/YmfmSink.h`

A thin `tdsp::MidiSink` adapter (mirrors the existing `DexedSink.h`). The OPM
engine is mono-timbral/omni, so this just channel-filters and forwards note
events. Pitch bend / mod / pressure are no-ops for now (OPM per-note bend is a
later refinement — leave the base-class defaults).

```cpp
// YmfmSink — MidiSink adapter for the ymfm OPM engine (AudioSynthYmfmOPM).
//
// Filters incoming MIDI to a single configured channel (or omni when
// listenChannel == 0) and forwards notes to the OPM's 8-voice allocator.
// OPM here is a single-timbre engine (one patch across all channels), like the
// Dexed backend; per-note MPE expression is not modelled.
#pragma once

#include <stdint.h>
#include <AudioSynthYmfmOPM.h>
#include <MidiSink.h>

class YmfmSink : public tdsp::MidiSink {
public:
    explicit YmfmSink(AudioSynthYmfmOPM *opm) : _opm(opm) {}

    // 0 = omni (all channels). 1..16 = single channel. Out-of-range -> omni.
    void setListenChannel(uint8_t channel) { _listenChannel = (channel <= 16) ? channel : 0; }
    uint8_t listenChannel() const { return _listenChannel; }

    void onNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) override {
        if (!listens(channel)) return;
        _opm->noteOn(note, velocity);
    }
    void onNoteOff(uint8_t channel, uint8_t note, uint8_t /*velocity*/) override {
        if (!listens(channel)) return;
        _opm->noteOff(note);
    }
    void onAllNotesOff(uint8_t channel) override {
        if (channel != 0 && !listens(channel)) return;   // channel 0 == panic
        _opm->allNotesOff();
    }

private:
    AudioSynthYmfmOPM *_opm;
    uint8_t            _listenChannel = 1;   // main.cpp overrides to 0 (omni)
    bool listens(uint8_t ch) const { return _listenChannel == 0 || _listenChannel == ch; }
};
```

## Step 2 — replace the `#error` branch in `main.cpp`

Swap the whole `#if defined(TDSP_SYNTH_YMFM) … #error … ` block (currently
lines ~63–70) for the working backend. It must satisfy the same contract the
Dexed branch does: an engine wired into `outL/outR` slot 0, `g_synthSink`, and
`synthBegin` / `synthNextInstrument` / `synthName`.

```cpp
#if defined(TDSP_SYNTH_YMFM)
  // ---- ymfm OPM backend (BSD-3-Clause) -------------------------------------
  #include <AudioSynthYmfmOPM.h>
  #include "YmfmSink.h"

  AudioSynthYmfmOPM g_opm;                        // stereo: output 0 = L, 1 = R
  AudioConnection   c_opmL(g_opm, 0, outL, 0);
  AudioConnection   c_opmR(g_opm, 1, outR, 0);    // OPM IS stereo — R -> outR (not fanned)
  YmfmSink          g_ymfmSink(&g_opm);
  tdsp::MidiSink   *g_synthSink = &g_ymfmSink;

  static const tdsp::ymfmopm::OpmVoice *kVoices[] = {
      &tdsp::ymfmopm::kAdditiveOrgan,
      &tdsp::ymfmopm::kElectricPiano,
  };
  static const int kNumVoices = sizeof(kVoices) / sizeof(kVoices[0]);
  static int       g_voiceIdx = 0;

  static void synthBegin() {
      g_opm.begin();                     // reset chip, size resampler, load default voice
      g_ymfmSink.setListenChannel(0);    // omni: one patch plays every channel
      g_opm.setVoice(*kVoices[g_voiceIdx]);
      // g_opm.setGain(2.0f);            // default already 2.0; raise/lower here if needed
  }
  static void synthNextInstrument() {
      g_voiceIdx = (g_voiceIdx + 1) % kNumVoices;
      g_opm.allNotesOff();
      g_opm.setVoice(*kVoices[g_voiceIdx]);
      Serial.printf("[opm] voice %d = %s\n", g_voiceIdx, kVoices[g_voiceIdx]->name);
  }
  static const char *synthName() { return "ymfm OPM"; }

#else  // ---- Dexed backend (default) -----------------------------------------
```

(Leave the entire `#else` Dexed block and its closing `#endif` exactly as-is.)

**Note on the audio graph:** the Dexed branch fans mono into both `outL`/`outR`
slot 0. OPM is genuinely stereo, so connect output 0 → `outL` and output 1 →
`outR`. The `outL.gain(0, 0.62f)` make-up in `setup()` is fine to keep; adjust
if you want the OPM louder/quieter relative to the Dexed target.

`synthBegin()` runs after `AudioMemory(40)` in `setup()` — required, because
`g_opm.begin()` touches the chip and the audio pool. No reordering needed.

## Step 3 — keep the ymfm build GPL-free in `platformio.ini`

The `[common]` section lists `synth_dexed` (GPL v3) in `lib_deps`, which would
otherwise be fetched/linked into **every** env. Override it to empty in the ymfm
env so only BSD ymfm is linked:

```ini
[env:teensy41_ymfm]
extends = common
build_flags = -std=gnu++17
    -D TDSP_HAS_I2C_MUX=1
    -D TDSP_SYNTH_YMFM=1
lib_deps =              ; <-- override [common]: no synth_dexed in the OPM build
```

`AudioSynthYmfmOPM.h` resolves via the existing `lib_extra_dirs = ../../lib`
(no new include paths needed — `lib/TDspYmfm/library.json` carries them).

## Step 4 — build & verify

```sh
pio run -e teensy41_ymfm            # should compile clean (was #error before)
pio run -e teensy41                 # confirm the Dexed default still builds
pio run -e teensy41_ymfm -t upload  # press PROGRAM if COM is busy; close the serial monitor first
```

Runtime check over serial (115200): `n` = test note, `V` = next voice, `W` =
play the baked song. Watch the heartbeat — `synth=ymfm OPM`, `outPeak` should go
non-zero on notes (single note ≈ 0.09, chord ≈ 0.34 with the default gain), CPU
~10–18 %.

## Notes / gotchas

- **Tuning:** the OPM note table targets ~A440 and tracks the keyboard
  monotonically; final concert-pitch alignment is a one-constant trim in
  `lib/TDspYmfm/src/AudioSynthYmfmOPM.cpp` (`kOpmNote` + the octave offset) if a
  keyboard reveals it's off. Confirm by ear.
- **Velocity** maps to operator total-level; for the additive default it's a
  clean loudness curve.
- **Drums:** OPM here is a single melodic engine like Dexed — leave the player's
  default `kMaskNoDrums`. A drum-mapped OPM voice would be a separate backend.
- **Pitch bend / mod / sustain** from live MIDI currently no-op on this sink
  (the base-class defaults). Add overrides to `YmfmSink` later if wanted; the
  engine would need per-note key-fraction updates for real bend.
