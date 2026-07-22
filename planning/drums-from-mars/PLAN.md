# Drums From Mars → mix-kit drum kits

Goal: use the local **"…From Mars"** one-shot packs (`C:\ALL THE DRUMS FROM MARS`) as
selectable drum kits in **mix-kit**, working on as many of the three target boards as
possible (16 MB PSRAM, 8 MB PSRAM, no-PSRAM Teensy 4.1).

## Source folder reality
- The usable content is the **complete `*.zip` packs**. The many `*.crdownload` files are
  stalled/incomplete Chrome downloads (several 0 bytes) — ignored by the tool.
- Each pack is a full producer bundle (Ableton/Kontakt/EXS presets + thousands of
  velocity-layered WAVs + loops). We cherry-pick ONE representative one-shot per GM
  percussion note — a kit is ~1 MB, not gigabytes.
- Folder layouts are inconsistent across packs, so mapping is by **filename keyword**
  (BD/Kick, SD/Snare, CH/OH, Clap, Rim, Tom Lo/Mid/Hi, Conga, Cowbell, Clave, Crash,
  Ride…). Synth packs (SH-101, DX, acid, CR-78) classify to 0 pieces and self-filter.

## How drums play (why the SF2 path)
mix-kit renders ch10 grooves through a **bank-128 SF2 kit** loaded by TSF
(`firmware/mix-kit/src/DrumTsf.h`). `drumTsfBegin()` prefers `/sf2/drumkits.sf2` if present
and the Drums menu switches to that kit list; each kit is a bank-128 **preset**, selected by
a plain ch10 program change. `tools/fetch_drumkits.py` already builds that font from acoustic
SFZ kits — we reuse its SF2 writer/validator/pusher.

## Tiered architecture (works on "as many as it can")
| Board | Drums | Path |
|---|---|---|
| 16 MB PSRAM | ✅ many kits (≤ ~14 MB font) | **Phase 1** SF2 |
| 8 MB PSRAM | ✅ curated kits (≤ ~6 MB font), melodic engine must be *synthesis* (Plaits/Dexed/OPLL), not the 8 MB TSF GM font | **Phase 1** SF2 |
| No PSRAM | ❌ TSF can't hold a resident font in 512 KB RAM | **Phase 2** SD sampler (new engine) |

TSF loads the ENTIRE font into PSRAM, so the SF2 path is inherently PSRAM-only. One font
sized ≤ ~6 MB runs on BOTH PSRAM boards — that's the recommended curated build.

## Phase 1 — DONE: `tools/build_mars_kits.py`
Reads WAVs straight out of the zips (no 36 GB extraction), classifies by filename, picks one
representative hit per GM note, normalizes to 48k/16-bit mono, bakes each pack (optionally each
tone variant) as a bank-128 preset into `/sf2/drumkits.sf2`, validates by compiling
`tools/sf2/tsf_drum_probe.cpp` against the shipped `tsf.h` and rendering every note, and can
push over USB `@WB`. Also emits `<out>/drums/<kit>/<gmnote>-<piece>.wav` (stereo) for Phase 2.

```
python tools/build_mars_kits.py --list                      # inventory + piece/variant counts
python tools/build_mars_kits.py --packs 808,909             # build (validated) to c:/tmp/t-dsp-drumkits
python tools/build_mars_kits.py --packs 909 --split-variants
python tools/build_mars_kits.py --packs 808,909,606,707,linn,dmx --push --port COM4
```
Verified: 808/909 build → 14/14 & 11/11 pieces audible through tsf.h; `--split-variants`
splits 808 into Clean/Color kits (Tube skipped <4 pieces); fit report warns per board.

Sizing: ~0.6–0.9 MB/kit (cymbal/open-hat tails dominate). 8 MB board ≈ 6–8 kits;
16 MB board ≈ 15–18 kits. `--all --split-variants` (100+ kits) will NOT fit — curate.

## Phase 2 — TODO: no-PSRAM SD-streaming drum sampler
New `ITrackEngine`-style drum engine that streams `/drums/<kit>/<note>.wav` off SD on ch10
triggers (Teensy `AudioPlaySdWav`/`AudioPlayMemory` style, ~tens of KB RAM, no resident font).
The Phase-1 tool already lays down the per-note WACs under `<out>/drums/<kit>/`. This is the
only way the no-PSRAM board plays real samples. Design pending.

## Phase 3 — TODO: loops
`808_loops`/`909_loops` (1–2 GB) are pre-rendered AUDIO loops, not one-shots. mix-kit has no
audio-loop *file* player (only `lib/TDspAudioLoop`, which records live device audio). Needs a
clock-synced WAV-loop player, or slice loops into one-shots. Deferred.
