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

## Status — 2026-07-12: milestone 1 VERIFIED ON DEVICE ✅
Built + flashed + confirmed making sound on the real board. `pio run -e teensy41_sf2` is
**green** (416 KB FLASH, ~460 KB RAM1; sample data lives in PSRAM, not in that RAM figure).
`teensy41_ymfm` / `teensy41_opl3` stay green. Font used: **GeneralUser GS v2.0.3**
(GeneralUser-GS.sf2, 32.3 MB) copied to `/sf2/gm.sf2` over USB-MTP — **NOT trimmed**, the
on-demand cache handles it.
- **Boot log:** `[sf2] ready: 8 MB PSRAM, 20 cache slots, 24 voices, drumInst=238` then
  `[synth] SF2 GM ready: 128 GM instruments + drums`. The preset resolver found the drum kit
  (SF2 instrument 238) and 128 melodic patches.
- **Playback test:** `W` played "William Tell Overture -> SF2 GM"; `outPeak` rose musically
  (0.005 → 0.13), melodic patches + drums both sounding, **~17% CPU** steady, memMax 27/80
  audio blocks. Stop → clean release, outPeak→0, back to 9% idle. No underruns/glitches.
- **One-time boot spike:** cpuMax briefly showed 230% while the first instruments loaded from
  SD at song start (blocking SD read, expected) — steady-state is ~17%.
- **Follow-up tuning (not blocking):** output level is a touch low (peaks ~0.13); raise
  `kSf2VoiceMix` (currently 0.4) and/or the F32 slot-3 gain. Level only, not correctness.

## Fix — 2026-07-12: proper per-note SF2 zone resolver (drums + split patches)
First device pass had **drums = noise**, **~half the melodic patches out of key or silent in
the GM sweep**. Root cause: the initial resolver took the *last instrument in a preset*. But
GeneralUser GS layers many instruments per preset — the drum kit maps ONE instrument per drum
(kick/snare/hat/tom/cymbal, split by key), and **65 of 128 melodic programs are key/velocity
splits** across 2-3 instruments. "Last wins" therefore played the wrong sample everywhere but
single-instrument patches.
- **New resolver** (`buildPresetMap`): for every `(program, note)` it matches the preset zone
  whose key range contains the note — explicit preset `keyRange` first, else the referenced
  instrument's own igen key coverage (exact per-note, NOT a min..max span, which was the trap:
  a span fills gaps so broad Toms/Accessory instruments stomped the specific hat/cymbal). Result
  validated offline against GM: kick=36, snare=38, hats=42/44/46, crash=49, ride=51, toms on
  41/43/45/47/48/50. Confirmed on device: `kick36->inst241` (Standard Kick 3).
- Precomputed into a **33 KB PSRAM note map** (`m_noteInst[129*128]`, row 128 = drums) at
  load; the six pdta sub-chunks are slurped into PSRAM scratch, resolved in RAM (no per-note SD
  seeks), scratch freed. `resolveInstrument(channel, note)` indexes it; drums skip pitch bend.
- Cache widened (24 slots @ 8 MB, 32 @ 16 MB) since a busy bar can touch ~13 drum instruments
  + melodic channels at once.
## Fix — 2026-07-12: stuck drums (loops) + out-of-key pitch (coarseTune/chCorrection)
Second device pass: **drums got "stuck on"** (rang for tens of seconds) and **some melodic
instruments still not in key**. Two independent causes, both fixed:
- **Stuck drums = looping drum samples + long release.** GeneralUser open hi-hat and the toms
  (which inherit `loop=1` from the instrument's global zone) loop, with 15-48 s release envelopes,
  so a hit rings forever. Fix in `Sf2GmEngine::ensureInstrument`: for any instrument in the drum
  set (`isDrumInstrument`, = any value in note-map row 128), force every loaded sample non-looping
  (`const_cast` LOOP=false). Drums then play their body once and stop. Harmless for the few
  percussion instruments also used melodically (agogo/woodblock are one-shots anyway).
- **Out of key = sf22aswt only folded `fineTune` into pitch.** Measured in this font: `coarseTune`
  != 0 on 209 zones (up to +-60 semitones!), and the sample header's `chCorrection` != 0 on 646 of
  921 samples (up to +-64 cents). T-DSP patch in `sf22aswt_reader_lazy.cpp` (Load_instrument_data):
  `CENTS_OFFSET = fineTune + chCorrection + coarseTune*100`. (`scaleTuning`, 64 occurrences, still
  ignored — needs per-note handling in AudioSynthWavetable; rare, deferred.)
## Fix — 2026-07-12: scaleTuning (pitched percussion / SFX out of key)
Third pass: Taiko (116), Melodic Tom (117), Synth Drum (118), Applause (126) still not in key.
Cause: all use **`scaleTuning = 50`** (pitch moves 1/2 semitone per key), which neither this
engine nor AudioSynthWavetable honored (both assume 100) -> intervals doubled. 64 zones use it.
- Added `AudioSynthWavetable::playNoteFreq(note, freq, amp)` (lib/Audio/synth_wavetable.h) — a
  one-line inline exposing `setState`, so a caller can select the sample by `note` but pitch it
  at an explicit freq. No change to existing behavior.
- Engine now reads each instrument's scaleTuning + root at load (`m_instScale`/`m_instRoot`,
  PSRAM, from a slurped `shdr` + igen scan). noteOn computes `effNote = root + (note-root)*
  scale/100` and calls `playNoteFreq(note, noteToFreq(effNote))` when scale != 100 (else plain
  playNote). Pitch bend uses the same per-voice scale/root. Muted Guitar (28) was separately a
  pure `chCorrection` case, already handled by the coarseTune/chCorrection loader patch.
## Fix — 2026-07-12: aux-layer resolution (first-wins) + unsorted-sample sort
Fourth pass: Clavinet plays the release thunk, Marimba only a sine, "Rock Organ isn't a rock
organ" (percussion click). Cause: GeneralUser presets list the MAIN instrument FIRST then LAYER
auxiliary ones (Clavinet_rel, Marimba_sine, Organ Percussion); AudioSynthWavetable can't sum
layers, and the resolver's "last wins" picked the tail. Fix: **first-covering-wins** in resolve()
(verified it changes only 1 drum note, SideStick rim-shot variant — harmless).
Also: **AudioSynthWavetable's sample scan requires note_ranges sorted ascending**, but SF2 zones
aren't (Melodic Tom lists 70-127 first -> every note grabbed sample 0). 13/128 programs affected.
Fix: insertion-sort samples by key-range in the converter (`to_AudioSynthWavetable_instrument_data`).
- **Reality check:** GeneralUser GS uses velocity layers, layered aux instruments, scaleTuning,
  and unsorted zones — all of which stress AudioSynthWavetable's one-sample-per-key model. We've
  now covered per-note resolution, drum one-shots, coarse/fine/chCorrection, scaleTuning, aux-layer
  first-wins, and range sorting. Residual limits: no velocity layering (picks one layer), no
  simultaneous aux layers. If instruments still misbehave, a simpler sampler-oriented GM font
  (one sample/key, no vel layers) is the lower-effort path than emulating full SF2 in this engine.
- **Catalog Q (app list):** firmware sends all 128 via `@INSTR` (one long UART line); a short list
  in the app is a BLE/UART truncation on the ESP32/app side, not the engine. "No drums" is by
  design — drums are MIDI ch10, not a selectable program (same as Dexed/OPL3). A selectable
  "Drum Kit" entry would be a small feature add.
- **Still needs the user's ear**: re-test Clavinet/Marimba/Rock Organ/Melodic Tom + the earlier batch.

## Fixes — 2026-07-12 (evening session, TSF engine)
- **Voice lockup ("frozen" board):** stock TSF drops new notes when its 32-voice pool is full of
  held/SUSTAIN voices -> hung notes accumulate -> song plays SILENT at ~44% CPU. Patched tsf.h
  (tsf_note_on) to STEAL the oldest voice (playIndex) when none is in release. See [[project_sf2_tsf_engine]].
- **Pitch bend too shallow (Staying Alive strings):** the song player (lib/TDspMidiPlayer/
  MidiFilePlayer.h) hardcoded a ±2-semitone bend range and ignored the file's RPN (CC 101=0,100=0,
  6=N). Staying Alive sets RPN range = 12 on the bend channels, so bends rendered at 2/12 = 1/6th
  depth (±0.34 instead of ±2.06 semis) -> nearly inaudible. Fixed: player now parses RPN per channel
  (pbRange_[16]) and resets on each play(). Affects ALL backends, not just TSF. Verified with a
  purpose-built /songs/bendtest.mid (RPN range 12, sweep +12/-12): device bend log reached 12.00 semis.
- **Debug hooks (uncommitted, in the mix-kit):** serial 'T' = 128-instrument self-test, 'B' = audible
  pitch-bend sweep. `/songs/bendtest.mid` staged on the card for future bend testing.

## NEW ENGINE — 2026-07-12: TinySoundFont backend (full-fidelity SF2) ✅ on device
The whack-a-mole above is the AudioSynthWavetable ceiling (1 sample/key, no velocity layers, no
layering). Added a SECOND, full-fidelity SF2 backend using **TinySoundFont** (schellingb/tsf.h,
MIT) — a COMPLETE renderer (velocity layers, region layering, all generators, filters, envelopes,
GM drums), so none of the per-generator glue is needed. Lives in `lib/TDspTsf` (tsf.h + tsf_impl.cpp
+ AudioSynthTsf.h); mix-kit `src/SynthBackendSF2Tsf.h` + `src/TsfSink.h`; env `teensy41_sf2_tsf`
(`-D TDSP_SYNTH_SF2_TSF`). Coexists with the sf22aswt backend (`teensy41_sf2`); pick via env.
- **PSRAM allocators** (extmem_malloc/realloc/free) + **SD stream loader** (no stdio, no whole-file
  buffer). **T-DSP int16 patch** to tsf.h: store samples as `short` not `float` (struct field +
  load path + 3 render read sites scale by 1/32767) — halves resident memory so a ~6 MB font fits
  8 MB. Verified on device.
- Font: **TimGM6mb.sf2** (5.99 MB, 5.79 MB samples) at `/sf2/gm_tsf.sf2` (int16 -> 5.8 MB PSRAM).
  GeneralUser's 30 MB can't be resident (that's the sf22aswt backend's job). On the 16 MB upgrade,
  TSF could hold a ~14 MB font (higher quality) unchanged.
- **Device result:** `[synth] SF2 GM (TSF) ready: 136 presets, 32 voices`. William Tell: outPeak
  0.08-0.22 (fuller than sf22aswt's ~0.13), **~45% CPU** steady (float, 32 voices, filters), memMax 5.
  One-time ~250% CPU blip while loading the 6 MB font from SD at boot.
- **A/B:** teensy41_sf2_tsf (TSF/TimGM6mb, full fidelity, 45% CPU) vs teensy41_sf2 (sf22aswt/full
  GeneralUser on-demand, 17% CPU). **User verdict: TSF "sounds fucking great" — TSF is the pick.**

## Verified — 2026-07-12: all 128 GM + drums load on TSF; catalog fix
- **Instrument self-test** ('T' serial cmd in main.cpp `runInstrumentSelfTest`): steps every GM
  program 0..127 (ch1) + drum notes 35..81 (ch10), plays test notes, logs the output peak; prints
  "prog N -> on" BEFORE rendering so a hang/fault names the culprit. Result on TSF: **0/128 melodic
  SILENT, 0/47 drum notes silent, ran to completion (no hang/crash)**. Quietest are legit (126
  Applause 0.09, 119 Reverse Cymbal 0.10). So "some instruments don't load/crash" is NOT happening.
- **"Moonlight / Staying Alive fail after a few measures"**: NOT reproducible. Both played fully
  (Staying Alive 45 s+ continuous, steady ~46% CPU, outPeak to 0.62, no stall). Audio code was
  unchanged since the report -> likely a transient (SD/USB contention or momentary spike). Watch for
  recurrence; if it returns, capture CPU at the failure instant.
- **Catalog "app shows ~30 of 128"**: root cause = ESP32 relay line buffer (600 B) + BLE
  characteristic cap (512 B) truncate the ~2 KB @INSTR list. FIX (no ESP32 change, no pagination):
  GM engines don't stream names — `synthIsGM()` (true for SF2/TSF/OPL3) makes sendCatalog emit a
  "\tGM" header flag and NO names; the app renders the standard 128 `GM_INSTRUMENTS` locally
  (app/tdsp-control/src/tdspBle.ts). Firmware flashed + app code done & typechecks; **app needs an
  EAS build to land on the phone** (eas build -p android --profile preview, jayshoes-team). Drums
  are intentionally NOT in the picker (MIDI ch10, not a program).
- **Vendored:** `lib/TDspSF2/src/sf22aswt/` (manicken/sf22aswt @ 4b8c5d7, MIT). Only patch to
  it: added `#include <cmath>` to `sf22aswt_structures.h` (it used `std::pow` w/o the include).
- **Wrote:** `lib/TDspSF2/src/Sf2GmEngine.{h,cpp}` — the GM layer (voice allocator, program/
  bank→instrument resolver reading phdr/pbag/pgen, PSRAM-adaptive cache of `ReaderLazy` slots,
  128 GM names, live pitch bend). Mix-kit side: `src/SF2Sink.h`, `src/SynthBackendSF2.h`
  (24 AudioSynthWavetable voices → AudioMixer4 tree → `AudioConvert_I16toF32` → mix slot 3),
  `main.cpp` backend branch, `[env:teensy41_sf2]`. `AudioMemory(80)` already covers the pool.
- **Remaining before it makes sound:** (1) put a **trimmed** `gm.sf2` at `/sf2/gm.sf2` on the
  card (full GeneralUser GS is ~30 MB; per-instrument load keeps only in-use patches resident,
  but the *whole file* must still fit reads); (2) flash + verify with the `/songs` diagnostics;
  (3) confirm 48 kHz pitch (AudioSynthWavetable keys off `AUDIO_SAMPLE_RATE_EXACT`, set to
  48000 here, so it should be correct — verify by ear); (4) tune `kSf2VoiceMix` / gain for level.
- **Known TODO in code:** evicting a cache slot leaks the small `instrument_data` arrays (not
  the PSRAM samples — those are freed); bounded by reload count, negligible for milestone.

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

## Adaptive PSRAM (8 MB now, 16 MB after the second-chip mod) — free with sf22aswt
Confirmed from the sf22aswt source: `sf22aswt` **already reads `external_psram_size` at
runtime**. `ReaderBase::ReadSampleDataFromFile` (sf22aswt_reader_base.cpp) sets
`samples_useExtMem = (external_psram_size != 0)`, spills every sample to `extmem_malloc`
(PSRAM) when present, and budget-checks each load against the **detected** size
(`external_psram_size * 1024 * 1024 - samples_usedRam`), returning `EXTRAM_SIZE_INSUFF` on
overflow. So 8-vs-16 MB is handled by the library — **do NOT hardcode 8**. Firmware-side
adaptation is only: (1) size the instrument **cache-slot count** off `external_psram_size`
at `begin()` (more PSRAM → more resident GM patches → fewer mid-song reloads); (2) handle a
failed load gracefully (keep the voice on its previous/ default patch) so the engine degrades
instead of crashing when a font is too big for the installed RAM.

## How sf22aswt actually loads (confirmed from source — drives the GM-layer design)
- **Indexed by SF2 *instrument*, not GM preset.** `ReaderLazy::Load_instrument(instIdx, ...)`
  returns a heap `AudioSynthWavetable::instrument_data*` (a multisample: `sample_note_ranges[]`
  + `samples[]`). GM program/bank → instrument must be resolved by us from the preset chunks.
- **Preset→instrument resolver (we write it).** `ReaderLazy::sfbk` is **public** and holds the
  lazy file positions/counts for `phdr`, `pbag`, `pgen`. Walk phdr (`wBank`,`wPreset`,
  `wPresetBagNdx`) → pbag zones → pgen, and read the `SFGenerator::instrument (=41)` generator's
  `UAmount` = the instrument index. Build `melodicInst[128]` (bank 0) + `drumInst` (bank 128,
  prefer preset 0). No lib fork needed — read the file with the public sfbk offsets.
- **One reader owns ONE instrument's samples at a time** — `ReadSampleDataFromFile` calls
  `FreePrevSampleData()` on entry, freeing the reader's previous samples. So a multi-instrument
  resident **cache = a pool of worker `ReaderLazy` slots** (clone the master via `CloneInto`),
  one instrument per slot. A GM moment needs ≤17 distinct patches (16 melodic channels + drums),
  so ~20 slots almost never evicts; evict LRU among slots not bound to a channel's live program.
- **Voices are silent until `amplitude()` is called** (initial `tone_amp == 0`). The pool must
  set each voice's amplitude at begin; `playNote(note, vel)` takes velocity 0..127 for the env.
- Global `SF22ASWT::Samples_Max_Internal_RAM_Cap` (default 400000) only matters when there is
  **no** PSRAM; with PSRAM detected it's bypassed. `SF22ASWT::samples_usedRam` tracks live bytes.

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
