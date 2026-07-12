# SF2 General-MIDI sample engine — build plan (fresh-agent handoff)

**Goal:** full 128-instrument General MIDI with **real sampled instruments** ("quality
keyboard," not FM), as a new synth backend for the mix-kit. It slots behind the SAME
`tdsp::MidiSink` + `SynthBackend*` seam as Dexed/OPL3/OPM, so the MIDI player, SD song
catalog, phone app, BT, and S/PDIF are UNCHANGED — only the engine is new. Build flag
`-D TDSP_SYNTH_SF2` → env `teensy41_sf2`.

> **This plan is grounded in the Teensy state-of-the-art (PJRC forum, Teensy Audio
> Library, and community GitHub repos), not first principles. The headline finding:
> DO NOT write your own SF2 parser or wavetable voice engine — proven, MIT-licensed,
> PSRAM-aware Teensy code already exists. Reuse it.** (Research done 2026-07-12; sources
> at the bottom.)

---

## Hardware / budget (measured on this board)
- Teensy 4.1, **currently 8 MB PSRAM**, upgradeable to **16 MB** by soldering a second
  APMemory **APS6404L-3SQR** (8 MB / 64 Mbit QSPI PSRAM, 8-SOIC) into the second bottom-side
  pad. The core auto-detects total size — **the firmware must run unchanged on 8 MB OR 16 MB**
  (see §"Adaptive PSRAM" — read `external_psram_size` at runtime, don't hardcode). The
  mix-kit prints the size at boot and via the `M` serial command.
- Sample data must be **PSRAM/RAM-resident** — see §"Streaming".
- TAC5212 DAC through an **F32 / 48 kHz TDM** mix bus (OpenAudio). The synth feeds **mix
  slot 3** as int16 → `AudioConvert_I16toF32` → `outL/outR` (see `SynthBackendOpl3.h`).
- RAM1 (DTCM/ITCM) is already tight on this firmware — samples go in PSRAM (`extmem`), voice
  state small.

## THE core decision: reuse the proven stack, don't rebuild
The community has already solved runtime SF2 → polyphonic sample playback on Teensy 4.1 with
PSRAM. Use it. Recommended (Path A):

| Component | Repo / source | Role | License |
|---|---|---|---|
| **AudioSynthWavetable** | Teensy Audio Library (from https://github.com/TeensyAudio/Wavetable-Synthesis) | The **voice engine**: DDS wavetable playback + attack/loop/release envelope + linear interp, 16-bit. **One object = one voice** (no internal allocator). | PJRC/MIT |
| **manicken/sf22aswt** | https://github.com/manicken/sf22aswt | **Runtime SF2 loader** into AudioSynthWavetable. `Load_instrument_from_file` reads an SF2 off SD; **auto-spills sample data to `extmem`/PSRAM** (tunable `Samples_Max_Internal_RAM_Cap`). Multi-instrument + lazy-load. This is the "GitHub addition to the outdated library." | **MIT**, active (v0.1.5, 2024); self-described WIP |
| **manicken/SoundFontDecoder** | https://github.com/manicken/SoundFontDecoder | Fixed **offline** SF2→C++ decoder (the original TeensyAudio one has a documented zone/sample duplication → file-size-explosion bug). Use for any offline prep. | active |
| **manicken/sf22aswtTester** | https://github.com/manicken/sf22aswtTester | Serial-command control + SF2/instrument upload — a working integration to copy patterns from. | — |

Path A gives you: SF2 parsing, per-voice sample rendering, envelopes, interpolation, and
PSRAM residency **for free**. What YOU still write is the thin GM layer on top (below).

**Alternative (Path B, less proven):** port **TinySoundFont** (`schellingb/tsf.h`, single
header, MIT, overridable `fopen`/`malloc` → point at PSRAM). It's a COMPLETE GM renderer
(parse + allocate + render + drums internally) — thinner GM glue, cleaner SF2 semantics —
but there is **no existing Teensy port** (only an ESP8266Audio port), it loads the whole
font's samples into one PSRAM buffer (trimmed font only), renders its own voice pool
(bypasses the Teensy audio graph — feed its block into an F32 sink), and its CPU at high
polyphony on Teensy 4.1 is unbenchmarked. Consider only if Path A's per-instrument model
proves too limiting.

## What you actually build (the GM layer + integration)
Regardless of path, the mix-kit needs a backend matching the exact API the other backends
use (mirror `SynthBackendOpl3.h`/`Opl3Sink.h`):
```
begin();  noteOn(ch1_16,note,vel);  noteOff(ch,note);  programChange(ch,prog);
pitchBend(ch,semitones);  controlChange(ch,cc,val);  allNotesOff();
setGain(float);  int activeVoices();  int numMelodic();  const char* melodicName(int);
```
- **Voice allocation / polyphony (Path A):** AudioSynthWavetable has NO allocator — you
  instantiate a **pool of N voices** (target **24–32**; 48 is proven at 1–10% CPU on a 4.0)
  and write the allocator: note-on → pick free/oldest voice, set its instrument (the current
  program for that MIDI channel; drums = channel 10), trigger; note-off → release. You may
  reuse **`newdigate/teensy-polyphony`** (https://github.com/newdigate/teensy-polyphony, MIT,
  a generic voice-allocation layer over the Teensy Audio Library) instead of writing it.
- **GM routing:** per MIDI channel keep current program (`programChange`) + pitch bend;
  channel 10 = drums (SF2 bank 128; the drum note selects the sample). Load the needed SF2
  instruments via `sf22aswt` (per-instrument or a resident set).
- **F32 / 48 kHz bridge:** AudioSynthWavetable outputs **int16** at the Teensy audio-block
  model. In THIS firmware `AUDIO_SAMPLE_RATE_EXACT=48000`, so the wavetable engine runs at
  48 kHz too — feed the summed int16 voices → `AudioConvert_I16toF32` → mix slot 3, exactly
  like the OPL3/OPM backends. (Verify the wavetable/SF2 sample-rate handling honors 48 kHz;
  flag if it assumes 44.1.)

## The SoundFont
- **PSRAM-resident, always** (see below). Put the `.sf2` on the SD as `/sf2/gm.sf2`;
  `sf22aswt` loads it at runtime and spills samples to PSRAM.
- **GeneralUser GS** (S. Christian Collins) is the de-facto Teensy GM bank (used by PJRC's
  ISO-Drone build with manicken's decoder). Full ~30 MB doesn't fit even 16 MB PSRAM →
  **load per-instrument on demand** into the adaptive PSRAM cache (§"Adaptive PSRAM") so each
  patch keeps full fidelity but only in-use ones sit in PSRAM. Optionally keep a **trimmed**
  variant (mono/16-bit, fewer layers, via Awave/OpenMPT/Polyphone) so that at **8 MB** more
  of the bank can stay resident; at **16 MB** the on-demand cache holds more of the full-
  fidelity patches. The cache budget scales with `external_psram_size` automatically.
- Verify licensing of whatever bank ships (GeneralUser GS is very permissive).

## Adaptive PSRAM (8 MB OR 16 MB) — ONE firmware, sized at runtime
The board ships 8 MB now and may get a second APS6404L-3SQR later (→ 16 MB). The engine must
adapt at boot, not need a rebuild:
1. Read `extern "C" uint8_t external_psram_size;` (MB) in `begin()`. Set a resident-sample
   budget `= external_psram_size * 1024*1024 * ~0.8` (leave headroom for the voice pool,
   the audio graph, and malloc).
2. **Design so more PSRAM = better, automatically.** The robust way: the full SF2 lives on
   the SD; instruments load **on demand** into a PSRAM cache sized to the budget, with **LRU
   eviction** when the cache is full. Same code path both modes — at 8 MB it caches fewer
   instruments (occasional reload on program-change), at 16 MB it caches (potentially all)
   more, with fewer/no reloads. `sf22aswt`'s lazy per-instrument load + PSRAM spill is the
   substrate for this.
3. Optionally preload a "resident set" (the most common GM programs + the drum kit) up to the
   budget, then demand-load the rest. Scale the resident-set count by `external_psram_size`.
4. Report at boot: PSRAM size, budget, how many instruments are resident. If a requested
   instrument won't fit even after eviction (huge patch on 8 MB), fall back to a trimmed
   version or the nearest smaller GM program — never crash.

Net effect: solder the second chip and the SAME firmware just holds more of the bank
resident (fewer reloads, more headroom for a fuller/higher-fidelity font). No 8-vs-16 build
split.

## Streaming: NO. Samples MUST be resident.
PJRC forum consensus (thread 58480): *"mixing several instruments directly from SD isn't
feasible because you access data too randomly for synthesis."* SD streaming works for ~1–2
voices, not polyphonic GM. So: **PSRAM-resident samples**, loaded from SD at boot /
instrument-change (use SDIO 4-bit, ~4× faster than SPI, for the load). Do not design an
SD-per-voice streaming engine.

## Mix-kit side (write — mirrors the OPL3 backend)
- `src/SF2Sink.h` — channel-addressed `tdsp::MidiSink` → `g_sf2` methods (copy `Opl3Sink.h`).
- `src/SynthBackendSF2.h` — the voice pool + allocator wired **int16 → F32 → mix slot 3**;
  the `synth*` interface (name `"SF2 GM"`, catalog from `numMelodic`/`melodicName`); `setGain`;
  drums on (`g_player.setChannelMask(kMaskAll)`); load `/sf2/gm.sf2` in `synthBegin` (show
  load progress — MB into PSRAM takes seconds).
- `src/main.cpp` — add `#elif defined(TDSP_SYNTH_SF2) #include "SynthBackendSF2.h"` to the
  backend `#if` block.
- `platformio.ini` — `[env:teensy41_sf2]` (extends common), `-D TDSP_SYNTH_SF2=1`,
  `lib_deps =` (stay GPL-free; sf22aswt/Wavetable are MIT), `build_src_filter = +<*>
  -<DexedVoiceBank.cpp>`. Vendor `sf22aswt` + `AudioSynthWavetable` (if not already in the
  Audio lib) under `lib/` like `lib/TDspYmfm`. `extmem` needs no special flag on teensy41.

## Build & test
- `pio run -e teensy41_sf2` → green; keep `teensy41`/`_ymfm`/`_opl3` green.
- Report FLASH + **PSRAM** usage and the achieved **voice count vs CPU** on Teensy 4.1
  (the 48-voice/1–10% number is from a 4.0 — confirm on 4.1).
- On device, play the diagnostic MIDIs already on the SD `/songs` (in `C:\tmp\opl3_tests`,
  copied to the card): `04_gm_sweep` is the payoff (each GM program is now a REAL instrument),
  plus `02_chromatic`, `03_velocity`, `05_polyphony`, `01_drums`, and Daft Punk.
- **Do NOT flash the shared board without checking with the user first.**

## Reference files in-repo (read these)
- Backend to mirror: `projects/spike_esp32_bt_spdif_mix_kit_f32/src/SynthBackendOpl3.h`,
  `Opl3Sink.h`, the `main.cpp` backend `#if` branch, the `[env:teensy41_opl3]` env.
- Engine style (AudioStream + allocator + int16 stereo + F32 bridge): `lib/TDspYmfm/src/
  AudioSynthYmfmOPL3.{h,cpp}`.
- Player + interface: `lib/TDspMidiPlayer`, `lib/TDspMidi/src/MidiSink.h`.
- Existing sampler infra to possibly reuse: `projects/t-dsp_f32_audio_shield/src/synth/`
  (multisample slot) + the `/samples/<bank>/<note>.wav` layout.
- Teensy PSRAM: `extmem`/`EXTMEM` keyword → 8 MB PSRAM; `extern "C" uint8_t external_psram_size;`.

## Gotchas (from the research)
1. **Reuse, don't rebuild** — the official TeensyAudio SF2 decoder has a real
   zone/sample-indexing bug (file-size explosion); manicken's fixes it and `sf22aswt` does
   the runtime+PSRAM loading. Rebuilding an SF2 parser reproduces solved, subtle problems.
2. **No SD streaming for polyphony** — PSRAM-resident only.
3. **Trim the font** — full GeneralUser GS (30 MB) won't fit 8 MB; SF2→resident also expands
   the data. Trim offline or load per-instrument.
4. **F32/48 kHz bridge** — proven engines are int16 classic-block; bridge into the F32 bus
   (already the pattern here). Verify 48 kHz handling.
5. **`sf22aswt` is WIP** — validate stability under 128-instrument switching; have a fallback
   (per-instrument reload) if a full resident GM set is unstable.
6. Rapid retrigger is fine for sample voices (reset read pointer + envelope) — no OPL3
   KON-edge issue (see [[project_opl3_dmxopl]]).

## Sources (state-of-the-art, 2026-07-12)
- manicken/sf22aswt, manicken/SoundFontDecoder, manicken/sf22aswtTester, manicken/teensy4.0polysynth (github.com/manicken/*)
- TeensyAudio/Wavetable-Synthesis + soundFontDecoder guide (teensyaudio.github.io/Wavetable-Synthesis)
- newdigate/teensy-polyphony, newdigate/teensy-sample-flashloader; jerry20091103/Teensy_Grovebox; Soundpauli/NI404; wrightflyer/SF2_SoundFonts
- schellingb/TinySoundFont (tsf.h); earlephilhower/ESP8266Audio (TSF port precedent)
- PJRC forum: "Wavetable synthesis of large soundfonts" (thread 58480 — streaming infeasible, shrink offline); "SoundFont Decoder & File Size" (thread 70218 — decoder bug + PSRAM); PJRC blog ISO-Drone (2026/02 — GeneralUser GS + manicken decoder on 4.1)
- Circuit Cellar "Build a SoundFont MIDI Synthesizer" Part 2 (48 voices @ 1–10% CPU on Teensy 4.0)

Related memory: [[project_opl3_dmxopl]], [[project_midi_player_synth_agnostic]].
