# Phase 2 — no-PSRAM SD-streaming drum sampler (`TDSP_DRUM_SD`)

Goal: a drum engine that streams `/drums/<kit>/<note>.wav` one-shots off `BUILTIN_SDCARD`
per ch10 GM note — **no PSRAM** — so the no-PSRAM local board (SN 18402920, COM4) can play the
"…From Mars" drum kits. Built on `newdigate/teensy-variable-playback` (`AudioPlaySdResmp`,
already in `lib/`). Sibling of `DrumTsf.h`/`DrumVoice.h`; owns drum mix slot 2.

## Content layout (produced by `tools/build_mars_kits.py`, in `_sf2_build/drums/`)
```
/drums/<kit>/<gmnote>-<name>.wav      e.g. /drums/808_clean/36-Bass_Drum.wav
```
- A **kit** = one subfolder of `/drums`. The leading integer of each filename = the GM note.
- WAV = 48 kHz / 16-bit / stereo (matches AUDIO_SAMPLE_RATE_EXACT; the lib is int16-only).
- Copied to the card via **card reader** (`@WB` is unreliable on the Windows local board).

## Reference code to mirror (study before writing)
- `firmware/mix-kit/src/DrumTsf.h` — the drum-engine slot pattern: file-scope audio nodes +
  `AudioConnection_F32` into `outL/outR` slot 2, a `*Sink`, a `drum*Begin()`, `drumApplyKit`.
- `firmware/mix-kit/src/DrumVoice.h` — the mono no-PSRAM drum engine (int16 → I16toF32 → slot 2).
- `projects/t-dsp_f32_audio_shield/src/synth/MultisampleSlot.{h,cpp}` — voice pool + stealing +
  `AudioPlaySdResmp` usage (`playWav`/`setPlaybackRate`/`isPlaying`/`stop`) + `pollVoices()`.
  ADAPT it (don't copy): drums are **one-shots** — no pitch, no velocity layers, no release
  samples, no sustain; add **hi-hat choke** (42/44/46 mutual exclusive class).
- `firmware/mix-kit/src/TsfSink.h` — MidiSink shape.

## `DrumSampler.h` (new, gated `#if defined(TDSP_DRUM_SD)`)
- **8 voices** of `AudioPlaySdResmp` (int16 stereo: out 0=L, 1=R) at file scope.
- Graph → mix slot 2: per channel, 2× `AudioMixer4` (voices 0-3, 4-7) → 1× `AudioMixer4` (stage2)
  → `AudioConvert_I16toF32` → `outL/outR` slot 2, gain ~0.62 (mirror DrumTsf). FX-send branch in
  main.cpp's `TDSP_FX_SEND` block (mirror the `TDSP_DRUM_VOICE` line ~639).
- `DrumSamplerSink : tdsp::MidiSink`:
  - `onNoteOn(ch,note,vel)`: ch10 → trigger one-shot for `note`. Hi-hat choke: if note ∈ {42,44,46},
    stop any voice currently playing a note in {42,44,46} first. Pick voice: idle → oldest;
    retrigger of same note steals that voice. `player.playWav(path[note])`, `setPlaybackRate(1.0)`,
    `enableInterpolation(false)`. If `path[note]` empty, ignore.
  - Drop note-offs (one-shots ring out) — no envelope needed.
  - `onProgramChange(10, prog)` → `setKit(prog)`. `onAllNotesOff` → stop all voices.
- **Kits**: `scanKits()` lists subfolders of `/drums` → `_kitFolders[]` (cap ~64, name ≤24).
  `setKit(i)` → scan `/drums/<folder>` building `path[128]` (parse leading int of each `<n>-*.wav`).
  Expose `numKits()/kitName(i)/currentKit()` for the drum-kit table.
- `drumSamplerBegin()`: `scanKits()`, `setKit(0)`, mixer gains 1.0, slot-2 gain, set
  `g_engineHasDrums=true` if ≥1 kit. `pollVoices()`: free voices whose `isPlaying()==false`.

## main.cpp wiring (mirror `TDSP_DRUM_VOICE` at every site)
`#error` mutual-exclusion group (~598); include (~601); FX-send branch (~639); `kDrumEngineName
= "Sampler"` (~650); `kDrumKitSelectable = true` (kits ARE selectable — folder = kit) (~663);
`drumApplyKit` dispatch (~1981) → `g_drumSamplerSink.onProgramChange(10,prog)`; the **kit table**
(`numDrumKits/drumKitName/drumKitProg`, ~1826-1870) needs a `TDSP_DRUM_SD` branch returning the
scanned folder list; `drumSamplerBegin()` in setup (mirror `drumTsfBegin` call site); `pollVoices()`
in `loop()`; `@STATE` unchanged (drums.map etc reuse). Keep `drumEngineOk()/g_engineHasDrums`.

## `@REINDEX` staleness fix (fold in — user approved)
Bug: boot stale-check (`main.cpp` ~4270) compares only the **melodic** engine name
(`readStoredEngine`) + `kCatalogVersion`; swapping drum engines (Plaits↔OPLL↔Sampler) keeps the
same melodic engine → catalog not rebuilt → stale `drumEngine` label in the app. Fix: add
`readStoredDrumEngine(char*,size_t)` to `CatalogDb.h` (mirror `readStoredEngine`, key
`"drumEngine":"`), and in the boot block also compare it to `engineCaps().drumEngine`; force a
rebuild (`buildCatalog(..., forceAll=true)`) on mismatch.

## New env
`[env:teensy41_dexed_pool_nobt_drumsd]` — copy `teensy41_dexed_pool_nobt_drumvoice`, swap
`-D TDSP_DRUM_VOICE=1` → `-D TDSP_DRUM_SD=1`, keep `TDSP_METRONOME`. `teensy-variable-playback`
auto-found by LDF from `DrumSampler.h`'s `#include <TeensyVariablePlayback.h>`.

## Green-build (report each)
`teensy41_dexed_pool_nobt_drumsd` (new), `teensy41_opll` (baseline floor), and
`teensy41_dexed2_opll2_drums` (ensure the shared main.cpp edits didn't break the TSF drum build).
Do NOT flash. No git commit — leave edits in the working tree on branch `mars-drumfonts`.
