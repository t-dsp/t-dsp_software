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

## Status & what changed since this doc was written

**The single-bank path (Steps 1–3) is essentially done** — `src/YmfmSink.h`,
the `TDSP_SYNTH_YMFM` branch in `main.cpp`, and the `teensy41_ymfm` env (with
`lib_deps =` empty + `build_src_filter … -<DexedVoiceBank.cpp>`) are all in
place. Remaining: build/run it (Step 4), and optionally pick up the two new
presets below.

The `lib/TDspYmfm` library has since grown three things the integration can use:

- **Two more presets** — `kFmBass`, `kBellVibes` (now four total). Add them to the
  `kVoices[]` table so `V` cycles a fuller instrument set.
- **`AudioSynthYmfmOPM::activeVoices()`** — live voice count; handy in the
  heartbeat.
- **An idle gate** — a silent OPM bank now stops calling the chip and costs
  ≈nothing. No action needed; it's why idle CPU is ~0 and why running *several*
  banks (below) is cheap. Measured: 4 banks idle ≈ 0.1 % CPU, all 4 sounding
  ≈ 43 %, one bank's 8-note chord ≈ 53 %.
- **`YmfmOpmMulti`** — a multitimbral manager (see the optional upgrade at the
  bottom). This is also the substrate a future MPE sink would build on.

The single-bank backend is mono-timbral/omni (one patch across all channels),
exactly like the Dexed backend — a fine default. The player's songs are
multi-channel with per-channel programs, though, so if you want the OPM to play
them as a **multi-instrument** module, take the multitimbral upgrade at the end.

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

## Optional: multitimbral (YmfmOpmMulti) — one instrument per MIDI channel

Turns the OPM backend into a multi-part module: N independent banks (each a full
OPM chip + patch + 8 voices), routed by MIDI channel, so the player's
multi-channel songs play with a different instrument per part. Hardware-verified
in `spike_midi_ymfm_opm` (4 banks). This *replaces* the single-bank `TDSP_SYNTH_YMFM`
block; everything else in `main.cpp` still stands.

**A. add `src/YmfmMultiSink.h`** — a routing sink (note: it does NOT channel-filter
like `YmfmSink`; routing-by-channel is the whole point):

```cpp
#pragma once
#include <stdint.h>
#include <YmfmOpmMulti.h>
#include <MidiSink.h>

class YmfmMultiSink : public tdsp::MidiSink {
public:
    explicit YmfmMultiSink(tdsp::ymfmopm::YmfmOpmMulti *m) : _m(m) {}
    void onNoteOn (uint8_t ch, uint8_t note, uint8_t vel) override { _m->noteOn(ch, note, vel); }
    void onNoteOff(uint8_t ch, uint8_t note, uint8_t)     override { _m->noteOff(ch, note); }
    void onProgramChange(uint8_t ch, uint8_t prog)        override { _m->programChange(ch, prog); }
    void onAllNotesOff(uint8_t)                           override { _m->allNotesOff(); }
private:
    tdsp::ymfmopm::YmfmOpmMulti *_m;
};
```

**B. the `TDSP_SYNTH_YMFM` block** — 4 banks sub-mixed into the player's `outL/outR`
slot 0 (so the engine-independent graph is untouched):

```cpp
  #include <AudioSynthYmfmOPM.h>
  #include <YmfmOpmMulti.h>
  #include "YmfmMultiSink.h"

  constexpr int kBanks = 4;
  AudioSynthYmfmOPM g_b0, g_b1, g_b2, g_b3;
  AudioMixer4       g_synMixL, g_synMixR;                 // sub-mix of the 4 banks
  AudioConnection   sb0L(g_b0,0,g_synMixL,0), sb0R(g_b0,1,g_synMixR,0);
  AudioConnection   sb1L(g_b1,0,g_synMixL,1), sb1R(g_b1,1,g_synMixR,1);
  AudioConnection   sb2L(g_b2,0,g_synMixL,2), sb2R(g_b2,1,g_synMixR,2);
  AudioConnection   sb3L(g_b3,0,g_synMixL,3), sb3R(g_b3,1,g_synMixR,3);
  AudioConnection   syL(g_synMixL,0,outL,0), syR(g_synMixR,0,outR,0);  // -> player mixer slot 0

  AudioSynthYmfmOPM *g_banks[kBanks] = { &g_b0, &g_b1, &g_b2, &g_b3 };
  tdsp::ymfmopm::YmfmOpmMulti g_multi(g_banks, kBanks);
  YmfmMultiSink   g_ymfmSink(&g_multi);
  tdsp::MidiSink *g_synthSink = &g_ymfmSink;

  static const tdsp::ymfmopm::OpmVoice *kVoices[] = {
      &tdsp::ymfmopm::kAdditiveOrgan, &tdsp::ymfmopm::kElectricPiano,
      &tdsp::ymfmopm::kFmBass,        &tdsp::ymfmopm::kBellVibes };
  static const int kNumVoices = sizeof(kVoices)/sizeof(kVoices[0]);

  static void synthBegin() {
      g_multi.begin();
      for (int i = 0; i < kBanks; i++) g_multi.setBankVoice(i, *kVoices[i]);
      g_multi.setVoiceTable(kVoices, kNumVoices);   // song Program Change -> voice
      g_synMixL.gain(0,0.7f); g_synMixL.gain(1,0.7f); g_synMixL.gain(2,0.7f); g_synMixL.gain(3,0.7f);
      g_synMixR.gain(0,0.7f); g_synMixR.gain(1,0.7f); g_synMixR.gain(2,0.7f); g_synMixR.gain(3,0.7f);
  }
  static void synthNextInstrument() { /* per-channel via Program Change; no global cycle */ }
  static const char *synthName() { return "ymfm OPM x4"; }
```

**C. three `setup()` tweaks:**
- **`AudioMemory(80)`** (was 40) — four banks each grab 2 blocks/update plus the
  sub-mix.
- Channel→bank map defaults to `(ch-1) % 4`; call `g_multi.setChannelBank(ch, bank)`
  to customize. `setVoiceTable` means song Program-Change events pick voices.
- The player defaults to `kMaskNoDrums` (skips ch 10). Multitimbral can handle more
  parts — either keep skipping drums, or `g_player.setChannelMask(kMaskAll)` and
  route a percussive bank to ch 10.

The GPL-free `platformio.ini` env from Step 3 is unchanged — `YmfmOpmMulti.h`
resolves through the same `lib_extra_dirs`.

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
