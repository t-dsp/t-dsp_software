# TDspHexeFx — vendored hexefx_audiolib_F32 (effects subset)

Stereo F32 audio effects for the OpenAudio_ArduinoLibrary, by Piotr Zapart (hexefx.com).
Used by the T-DSP FX pedalboard — see `planning/plate-reverb-fx/DESIGN.md`.

- **Upstream:** https://github.com/hexeguitar/hexefx_audiolib_F32
- **Vendored at commit:** `ef8b85d07513300e0f70213737fe47a111788e16` (v1.3.0)
- **License:** MIT (see `LICENSE`).

## Why vendored (not `lib_deps`)

This repo vendors its audio libs under `lib/` (OpenAudio, TAC5212) so builds are offline and
reproducible; only GPL `synth_dexed` is fetched. hexefx is MIT, so it lives here too. LDF finds
it via `lib_extra_dirs = ../../lib`; it compiles against the vendored OpenAudio headers.

## What was pruned from upstream `src/`

Removed because they target other hardware and would drag unneeded deps / fail to compile:
- `control_*.{h,cpp}` — 6 other-vendor codec drivers (WM8731, SGTL5000, ES8388, AK4452/4558/5552).
  We drive the TAC5212 ourselves (`lib/TAC5212`).
- `input_i2s2_F32.*`, `input_i2s_ext_F32.*`, `output_i2s2_F32.*`, `output_i2s_ext_F32.*` — their
  board's I2S I/O. We use OpenAudio's `AudioInputTDM_F32` / `AudioOutputTDM_F32`.
- `hexefx_audiolib_F32.h` — the umbrella header (it `#include`s all of the above). **Include the
  specific effect header you need instead**, e.g. `#include "effect_platereverb_F32.h"`.

Everything else (effects, filters, `basic_*`, `wavetables.c`) is upstream-verbatim. To update:
re-clone upstream, re-apply the prune list above, and bump the commit SHA here.

## Effects kept (all stereo F32 unless noted)

reverb: `effect_platereverb_F32` · `effect_springreverb_F32` · `effect_reverbsc_F32` (needs PSRAM) ·
delay: `effect_delaystereo_F32` · modulation: `effect_phaserStereo_F32` · `effect_infphaser_F32` (mono) ·
drive/guitar: `effect_guitarBooster_F32` · `effect_wahMono_F32` (mono) · `filter_tonestackStereo_F32` ·
`filter_ir_cabsim_F32` (+ SD variant) · dynamics: `effect_compressorStereo_F32` ·
`effect_noiseGateStereo_F32` · `effect_gainStereo_F32` · EQ/filter: `filter_3bandeq` ·
`filter_equalizer_F32` (mono FIR) · `filter_biquadStereo_F32` · `filter_DCblockerStereo_F32` ·
util: `effect_monoToStereo_F32` · `effect_xfaderStereo_F32` · `switch_selectorStereo_F32`.
