# SF2 General-MIDI sample engine — build plan (fresh-agent handoff)

**Goal:** full 128-instrument General MIDI with **real sampled instruments** ("quality
keyboard," not FM/retro), as a new synth backend for the mix-kit. This replaces the
*character* of Dexed/OPL3/OPM (all synthesis) with actual recordings of instruments —
the only way to sound like a real piano/strings/etc.

It slots behind the SAME seam as the existing engines: a `tdsp::MidiSink` +
`SynthBackend*` in `projects/spike_esp32_bt_spdif_mix_kit_f32`, so the MIDI player,
the SD song catalog, the phone app, BT, and S/PDIF are all UNCHANGED — only the
engine is new. Build flag `-D TDSP_SYNTH_SF2` → env `teensy41_sf2`.

This is the **biggest** engine of the set (real sample playback with per-voice
pitch/loop/envelope/filter + SF2 parsing + PSRAM/SD sample management). Budget for it.

---

## Hardware / budget (measured on this board)
- Teensy 4.1, **8 MB PSRAM** (`external_psram_size == 8`; the mix-kit prints it at boot
  and via the `M` serial command). Sample data lives in PSRAM via the `EXTMEM` keyword.
- TAC5212 DAC through an **F32 / 48 kHz TDM** mix bus (OpenAudio). The synth feeds
  **mix slot 3** as int16 → `AudioConvert_I16toF32` → `outL/outR` (see SynthBackendOpl3.h).
- RAM1 (DTCM/ITCM) is already tight on this firmware — keep new DTCM use small; put big
  buffers in `EXTMEM` (PSRAM) or `DMAMEM` (OCRAM).

## Two-stage plan (do Stage 1 first)
- **Stage 1 — PSRAM-resident small GM SF2.** A compact full-GM SoundFont (~4–7 MB) whose
  entire 16-bit sample pool loads into `EXTMEM` at boot from the SD card. No streaming.
  Proves the SF2 parser + the sampled-voice engine. Fits 8 MB with headroom.
- **Stage 2 — SD-streaming (later).** Same voice engine, but sample data streams from the
  SD on demand (for a bigger/better SF2 like GeneralUser GS ~30 MB, which does NOT fit in
  8 MB PSRAM), using PSRAM as a sample cache. Only do this after Stage 1 sounds good.

## The engine — `lib/TDspSF2`
### 1. SF2 parser (`SF2File.{h,cpp}`) — pure, no audio
SF2 is a RIFF file ("sfbk") with LIST chunks:
- **sdta/smpl** — the raw 16-bit PCM sample pool (mono samples concatenated).
- **pdta** — the "hydra": `phdr` (presets) → `pbag`/`pgen`/`pmod` (preset zones) → `inst`
  (instruments) → `ibag`/`igen`/`imod` (instrument zones) → `shdr` (sample headers:
  start/end/loopstart/loopend, sampleRate, originalPitch, pitchCorrection).
Parse into: **preset(bank, program) → zones → instrument → zones → {sample header +
generator set}**. GM melodic = **bank 0, programs 0–127**; GM drums = **bank 128**.
Generators to honor (minimum): `sampleID`, `keyRange`, `velRange`, `overridingRootKey`,
`coarseTune`/`fineTune`, `sampleModes` (loop on/off), `startloopAddrsOffset` +variants,
volume ADSR (`attackVolEnv`/`decay`/`sustain`/`release`, timecents), `initialAttenuation`,
`pan`, `initialFilterFc`/`Q` (optional at first). Store a flat lookup: for a given
(program, key, velocity) → the matching zone's sample + params.

### 2. Sampled-voice engine (`AudioSynthSF2.{h,cpp}`)
A Teensy `AudioStream`, **stereo int16** (out 0=L, 1=R), summing N voices at
`AUDIO_SAMPLE_RATE`. Unlike the ymfm engines there is **no chip resampler** — each VOICE
resamples its own sample:
- **Pitch:** `step = (sample.rate / AUDIO_SAMPLE_RATE_EXACT) * 2^((note - rootKey + tune + bend)/12)`.
  Walk a fractional read pointer through the PSRAM sample with linear interpolation.
- **Loop:** if `sampleModes` has loop, wrap the pointer between loopStart/loopEnd; else
  play once to `end` then release.
- **Volume envelope:** SF2 ADSR (convert timecents → per-sample rates). Apply attenuation
  (`initialAttenuation` + velocity→attenuation + envelope).
- **Pan:** SF2 `pan` → L/R gains.
- **(Optional, later) low-pass filter** from `initialFilterFc`/`Q`.
- **Polyphony:** ~24–32 voices, oldest/quietest-note stealing. Per MIDI channel: current
  program (`programChange`), pitch bend. **Channel 10 = drums** → SF2 bank 128, the drum
  note selects the sample.
- Rapid retrigger is a non-issue here (a new note just resets the voice's read pointer +
  envelope) — unlike the OPL3 KON-edge bug (see [[project_opl3_dmxopl]]).

### 3. Public API — mirror `SynthBackendOpl3.h`/`Opl3Sink.h` EXACTLY
`AudioSynthSF2` must expose what the mix-kit backend calls:
```
begin();                                   // parse SF2 + load samples into PSRAM
noteOn(uint8_t ch1_16, note, vel); noteOff(ch, note); programChange(ch, prog);
pitchBend(ch, float semitones); controlChange(ch, cc, val); allNotesOff();
setGain(float); int activeVoices() const;
int numMelodic() const;                    // 128
const char* melodicName(int) const;        // GM name or SF2 preset name
```
Stereo `AudioStream(0,nullptr)` with 2 outputs. Load the SoundFont in `begin()`.

## Mix-kit side (write — mirrors the OPL3 backend)
- `src/SF2Sink.h` — channel-addressed `tdsp::MidiSink` → `g_sf2` methods (copy `Opl3Sink.h`).
- `src/SynthBackendSF2.h` — `AudioSynthSF2 g_sf2;` wired **stereo int16 → F32 → mix slot 3**;
  the `synth*` interface (name `"SF2 GM"`, description, catalog from `numMelodic`/
  `melodicName`, `synthSetInstrument` = audition a program on all channels); `setGain`;
  drums on: `g_player.setChannelMask(tdsp::MidiFilePlayer::kMaskAll)`. In `synthBegin`,
  parse the SF2 from `/sf2/gm.sf2` on the SD and load its samples into PSRAM (show progress;
  it takes a few seconds).
- `src/main.cpp` — add `#elif defined(TDSP_SYNTH_SF2) #include "SynthBackendSF2.h"` to the
  backend `#if` block (currently OPL3/YMFM/Dexed).
- `platformio.ini` — add `[env:teensy41_sf2]` (extends common) with
  `-D TDSP_SYNTH_SF2=1`, `lib_deps =` (empty — stay GPL-free), and
  `build_src_filter = +<*> -<DexedVoiceBank.cpp>`. No special PSRAM flag — `EXTMEM` works
  on teensy41.

## The SoundFont itself
- **Stage 1:** obtain a COMPACT full-GM `.sf2` (~4–7 MB, permissive license) and put it on
  the SD as `/sf2/gm.sf2`. Do NOT bake multi-MB into flash (8 MB flash, impractical) —
  parse from SD at boot and load the sample pool into `EXTMEM`. Good small options exist
  (various "GM" / "GMGSx" banks); verify licensing. If none small enough sounds good,
  down-sample/trim a bigger one offline.
- **Stage 2:** GeneralUser GS (~30 MB, very permissive) on SD, streaming.

## Build & test
- `pio run -e teensy41_sf2` → green; keep `teensy41`, `teensy41_ymfm`, `teensy41_opl3` green.
- Report FLASH + **PSRAM** usage (how much of 8 MB the sample pool takes).
- On device, play the diagnostic MIDIs already on the SD `/songs` (generated in
  `C:\tmp\opl3_tests`, copied to the card): `02_chromatic` (tuning), `03_velocity`,
  `04_gm_sweep` (now each GM program is a REAL instrument — the payoff test), `05_polyphony`,
  `01_drums`, and Daft Punk. Judge by ear with the user.
- **Do NOT flash the shared board without checking with the user first.**

## Reference files (read these)
- Backend to mirror: `projects/spike_esp32_bt_spdif_mix_kit_f32/src/SynthBackendOpl3.h`,
  `Opl3Sink.h`, the `main.cpp` backend `#if` branch, and the `[env:teensy41_opl3]`/`_ymfm`
  envs in `platformio.ini`.
- Engine style (AudioStream + voice allocator + stereo int16): `lib/TDspYmfm/src/
  AudioSynthYmfmOPL3.{h,cpp}` and `AudioSynthYmfmOPM.cpp`.
- Player + interface: `lib/TDspMidiPlayer` (`MidiFilePlayer`, `MidiFileEvent`) and
  `lib/TDspMidi/src/MidiSink.h`.
- **Existing sampler code to reuse if present:** the multisample sampler slot — check
  `projects/t-dsp_f32_audio_shield/src/synth/` and the `/samples/<bank>/<note>.wav` layout;
  there may be a sample-playback voice you can adapt instead of writing from scratch.
- Teensy PSRAM: `EXTMEM` keyword allocates in the 8 MB PSRAM; `extern "C" uint8_t
  external_psram_size;` = MB.

## Gotchas
- Every SF2 sample has its OWN sample-rate + root key + loop points — a note plays a sample
  pitch-shifted from its root key; you must resample per voice.
- GM drums = SF2 **bank 128**; melodic = bank 0. Channel 10 is drums.
- 8 MB PSRAM: GeneralUser GS (30 MB) does NOT fit — Stage 1 needs a small SF2, Stage 2
  streams.
- Loading MB of samples from SD → PSRAM at boot takes seconds; print progress.
- Watch RAM1 — it's already tight on this firmware; keep voice state small, samples in PSRAM.

Related memory: [[project_opl3_dmxopl]], [[project_midi_player_synth_agnostic]].
