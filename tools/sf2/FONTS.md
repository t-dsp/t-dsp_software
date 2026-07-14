# TSF SoundFont test matrix

TSF (`lib/TDspTsf`, the full-fidelity GM engine — velocity layers, filters, region
layering, GM drums) holds the **entire font's sample data resident in PSRAM** (int16, the
T-DSP patch that halves it vs. stock float). There is no streaming. So the font's `smpl`
chunk + TSF's preset/region tables must fit the **installed PSRAM**.

- **Teensy 4.1 maxes at 16 MB** PSRAM (2× APS6404L-3SQR, 8 MB each).
- A **custom core module** with OPI PSRAM (FlexSPI2) can reach **32–64 MB** — that's the
  only way to hold the bigger variants (and native GeneralUser) resident.
- Rule of thumb: usable sample budget ≈ *(PSRAM MB − ~2.5 MB)* for TSF tables + audio graph
  + ESP32 resampler + stack. GeneralUser has 325 instruments / ~900 samples, so its tables
  cost more than TimGM6mb's — keep ~2.5 MB headroom.

Baseline for comparison: **sf22aswt** (`teensy41_sf2`) *streams* the full 30.6 MB GeneralUser
off SD with a ~1 MB PSRAM cache — lower fidelity (one sample/key, no velocity layers) but
any font size. TSF is the quality path; these variants make GeneralUser fit it.

## The fonts

All GeneralUser variants are anti-aliased downsamples of GeneralUser GS v2.0.3 (only samples
*above* the target rate are resampled; see `build_gu_fonts.py`). "Min PSRAM" includes the
~2.5 MB headroom.

| Build env | Flag | SD path | `smpl` | Min PSRAM | Fidelity |
|---|---|---|---|---|---|
| `teensy41_sf2_tsf` *(default)* | — | `/sf2/gm_tsf.sf2` | 5.5 MB | **8 MB** | TimGM6mb baseline |
| `teensy41_sf2_tsf_gu8`  | `TSF_FONT_GU8`  | `/sf2/gm_gu8.sf2`  | 8.0 MB  | 16 MB | GeneralUser, lo-fi (8 kHz) |
| `teensy41_sf2_tsf_gu12` | `TSF_FONT_GU12` | `/sf2/gm_gu12.sf2` | 11.7 MB | **16 MB safe** | GeneralUser, 12 kHz |
| `teensy41_sf2_tsf_gu14` | `TSF_FONT_GU14` | `/sf2/gm_gu14.sf2` | 13.5 MB | 16 MB (push) | GeneralUser, 14 kHz |
| `teensy41_sf2_tsf_gu16` | `TSF_FONT_GU16` | `/sf2/gm_gu16.sf2` | 15.4 MB | 32 MB+ | GeneralUser, 16 kHz |
| `teensy41_sf2_tsf_gu18` | `TSF_FONT_GU18` | `/sf2/gm_gu18.sf2` | 17.1 MB | 32 MB+ | GeneralUser, 18 kHz |
| `teensy41_sf2_tsf_gu22` | `TSF_FONT_GU22` | `/sf2/gm_gu22.sf2` | 20.8 MB | 32 MB+ | GeneralUser, 22 kHz |
| `teensy41_sf2_tsf_gu24` | `TSF_FONT_GU24` | `/sf2/gm_gu24.sf2` | 22.3 MB | 32 MB+ | GeneralUser, 24 kHz |
| `teensy41_sf2_tsf_gufull` | `TSF_FONT_GU_FULL` | `/sf2/gm_gu_full.sf2` | 30.6 MB | **64 MB** custom | GeneralUser, native 44 k |

### What fits what

- **8 MB (stock, current jay-mint):** TimGM6mb only. GeneralUser can't fit TSF at any usable
  rate — keep it on sf22aswt.
- **16 MB (after soldering the 2nd chip):** **`gu12` is the safe pick, `gu14` the stretch.**
  These are the real upgrade over TimGM6mb — full TSF fidelity with GeneralUser's instruments.
  `gu16`+ won't fit with headroom.
- **32 MB custom board:** `gu16`–`gu24` — near-CD-rate GeneralUser.
- **64 MB custom board:** `gm_gu_full` (native) resident, plus room for an even richer font.

## How to test (when you're back)

1. **Solder** the 2nd 8 MB PSRAM chip (both boards) → 16 MB. Confirm at boot: the TSF banner
   prints `(16 MB PSRAM installed)` and the selected font's description.
2. **Copy the font to SD.** Put the file from `tools/sf2/fonts/` onto the card at the path the
   flag names — e.g. for `gu12`, copy `gm_gu12.sf2` → `/sf2/gm_gu12.sf2`. (Card mounts as
   MTP "T-DSP Songs" on Windows — use Explorer / Shell.Application; on jay-mint copy to the
   mounted SD.) The default `/sf2/gm_tsf.sf2` must be TimGM6mb (`gm_tim.sf2`).
3. **Build + flash** the matching env, e.g.:
   `pio run -e teensy41_sf2_tsf_gu12 -t upload`
4. **Self-test:** send `T` over serial — it sweeps all 128 GM programs + drums and prints
   `peak`/`SILENT`. A clean sweep = the font loaded and every preset sounds. If the banner
   says `load FAILED (… too big for PSRAM)`, the font is over budget → step down a rate.
5. **A/B by ear:** play songs (`W`/`S`) and audition instruments (`V`) across `gu12` / `gu14`
   / TimGM6mb and pick the sweet spot for your PSRAM.

## Regenerating / adding rates

The `.sf2` binaries are **git-ignored** (too big); regenerate them anytime:

```
python tools/sf2/build_gu_fonts.py <GeneralUser.sf2> tools/sf2/fonts 22050 16000 14000 12000 8000
```

Add a rate → also add a `TSF_FONT_GUxx` branch in `SynthBackendSF2Tsf.h` and an env in
`platformio.ini`. The builder resamples per-sample with anti-aliasing, rewrites shdr
loop/offsets + the fine sample-address generators, and keeps the ~2 coarse-offset samples at
native rate (see the script header for the correctness notes).

> Not a downsample lever, but the ceiling-raiser: **more PSRAM.** A 32–64 MB custom core
> module is the path to full-rate GeneralUser (or a bigger font like a trimmed FluidR3) in TSF.
