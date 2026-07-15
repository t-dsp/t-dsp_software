# Better drums for the TSF engine — merge a drum-only SoundFont

**Status:** tooling built + validated offline; **needs a real drum font + a working device** to
finish. This is the "do it when the board boots" runbook. Branch: `drum-sf2-merge`.

## Why this exists

The TSF engine (`lib/TDspTsf`) is the reliable, fast GM path — but its percussion is only as
good as the bank-128 kit inside whatever font it loads (TimGM6mb today, GeneralUser once there's
more PSRAM). When you're jamming a live melody over the looping ch10 grooves
(the "Drums" feature), the kit is the weak link. TSF loads **one** font, has no per-channel font
routing and no bank-merge (schellingb/TinySoundFont #79), so you can't just "add a drum font on
ch10." The fix is offline: graft a dedicated drum kit onto the base GM font and produce one
`.sf2` for the SD card. **No firmware change, no reflash** — the font path is unchanged; you're
swapping a file on the card.

## The tool: `merge_drum_sf2.py`

Rebuilds a font from `(base melodic presets) + (drum font's bank-128 kit)`, garbage-collecting
everything unreached — so the base's old GM drum samples are pruned and the result is often
*smaller* despite better drums. All preset/instrument/sample cross-references are re-indexed;
sample PCM is untouched. Validated two ways (see "Validation" below).

```
# inspect any font's presets + bank map:
python tools/sf2/merge_drum_sf2.py --list <font.sf2>

# graft the drum font's bank-128 kit onto the base, replacing base bank 128:
python tools/sf2/merge_drum_sf2.py <base_gm.sf2> <drum.sf2> <out.sf2>

# prove the splice logic with no external files:
python tools/sf2/merge_drum_sf2.py --selftest
```

Options: `--drum-bank N` (which bank in the drum font holds the kit; default 128),
`--dest-bank N` (bank to place it at in the output; default 128), `--keep-base-drums`
(keep the base's kits too, e.g. add the new kit at a different `--dest-bank`).

## Step 1 — get a drum-only SF2 (manual; sites block scripted download)

Browse the top-rated percussion fonts and pick one, then download by hand:
<https://www.musical-artifacts.com/artifacts?formats=sf2&order=top_rated&tags=percussion>

Vetted candidates (confirm the license on the page before shipping — they change):

| Font | Why | License | Where |
|---|---|---|---|
| **The Definitive Perfect Drums (Fixed Banks)** | Already mapped to **bank 128** → drops straight in; large, detailed kit | check page | musical-artifacts.com/artifacts/6554 |
| **909 Drum Soundfont** | Tiny, classic TR-909 electronic kit; great as an alt `--dest-bank` kit | **CC-BY 3.0** (product-safe w/ credit) | musical-artifacts (search "909") |
| **NS_Kit7Free** | Best free *acoustic* kit (rock/jazz), uncompressed | free, non-commercial-ish — **verify** | natural-studio / musical-artifacts |

> **Licensing matters — this is a shippable product.** Prefer CC0 / CC-BY. If a font is
> non-commercial or "don't redistribute," you can still *use* it to author your own patches, but
> don't ship the font on the card. Keep the source license file next to the merged font, same as
> `fetch_drums.py` does for the groove packs.

If the drum font's kit is **not** on bank 128, run `--list` on it to find the bank, then pass
`--drum-bank <that bank>`.

## Step 2 — pick the base font (depends on installed PSRAM)

The base is whatever GM font that PSRAM tier can hold (see `FONTS.md` for the full matrix):

| PSRAM | Base font | Base file |
|---|---|---|
| 8 MB (stock, today) | TimGM6mb | `tools/sf2/fonts/gm_tim.sf2` |
| 16 MB (2nd chip soldered) | GeneralUser gu12 (safe) / gu14 (stretch) | `gm_gu12.sf2` / `gm_gu14.sf2` |
| 32–64 MB (custom OPI core) | gu16–gu_full | `gm_gu16.sf2` … `gm_gu_full.sf2` |

**"Build what we can now, add RAM later":** run the merge against *each* base you might ship and
stage all the outputs. When you solder more PSRAM you just drop the bigger merged file on the card
and switch the build env — the merge tool doesn't change.

## Step 3 — merge, and (if needed) fit the PSRAM budget

Order of operations matters:

- **Merge first, then downsample** (recommended for size): graft full-rate drums onto the base,
  then run `build_gu_fonts.py` on the *result* to bring the whole thing under budget. The
  downsampler treats the grafted drum samples like any other.
  ```
  python tools/sf2/merge_drum_sf2.py gm_gu_full.sf2 perfect_drums.sf2 merged_full.sf2
  python tools/sf2/build_gu_fonts.py merged_full.sf2 tools/sf2/fonts 14000 12000
  #   -> tools/sf2/fonts/gm_gu14.sf2 / gm_gu12.sf2  (now with the better drums baked in)
  ```
- **Merge into an already-small base** (recommended for drum punch): keeps the drums at native
  rate while the melodic base stays downsampled — drums hit harder, at some size cost.
  ```
  python tools/sf2/merge_drum_sf2.py gm_gu12.sf2 perfect_drums.sf2 gm_gu12.sf2
  ```

Check the reported `smpl` size against the "Min PSRAM" column in `FONTS.md` (remember the ~2.5 MB
headroom for TSF tables + audio graph + ESP32 resampler + stack). If it's over, downsample a rate.

## Step 4 — put it on the card and test on device

No firmware change is required — overwrite the font file the current build env already loads
(`SynthBackendSF2Tsf.h` → `TSF_FONT_PATH`). For the default env that's `/sf2/gm_tsf.sf2`.

1. Copy the merged `.sf2` onto the SD at that path (card mounts as MTP "T-DSP Songs" on Windows;
   or copy on jay-mint). Keep the original as a backup first.
2. Boot; the TSF banner should print the font loaded (and `(N MB PSRAM installed)`).
   `load FAILED (… too big for PSRAM)` → the font is over budget, downsample a rate.
3. Send **`T`** over serial — it sweeps all 128 GM programs **+ drums** and prints `peak`/`SILENT`
   per preset. A clean sweep = the font loaded and every kit sounds.
4. A/B by ear: start a groove (Drums menu / `@DRUM`), jam a melody, compare against the backup
   font. Keep the winner.

## Validation done so far (offline, no hardware)

- `--selftest`: synthetic base (2 melodic + 1 weak drum preset) + synthetic drum font (good kit +
  junk melodic). Asserts the weak base kit and the junk melodic sample are GC-pruned, the good kit
  is grafted onto bank 128, melodic presets survive, PCM is byte-identical through the merge, and
  no dangling sample/instrument indices remain. **Passes.**
- Real end-to-end: `gm_tim.sf2` (base) + `gm_gu12.sf2` (drum source). Output = 141 presets
  (136 base − 8 base kits + 13 GeneralUser kits ✓), samples pruned 520+920 → 622, bank 0 keeps
  TimGM6mb names, bank 128 shows GeneralUser names, output re-parses clean. **Passes.**
- **Not yet validated:** that TSF *renders* a merged font on the actual Teensy. That's Step 4 —
  the reason this is staged on a branch to merge once the device is up.
