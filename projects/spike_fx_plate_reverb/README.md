# spike_fx_plate_reverb — hexefx F32 FX cost/space bench

Proves the hexefx `AudioEffectPlateReverb_F32` (and siblings) on the real T-DSP hardware
(Teensy 4.1 + TAC5212, SAI1 TDM), and measures each effect's **FLASH / RAM / CPU** cost so we
know what fits before touching the mix-kit. Phase 1 of the FX plan — see
`planning/plate-reverb-fx/DESIGN.md` (§6.5 for the budget guardrail).

## What it is

`src/main.cpp` builds a minimal graph: **F32 test tone → one hexefx effect → TAC5212 (TDM)**.
The effect is chosen at build time by the env's `SPIKE_FX_*` flag; exactly one is
instantiated so `--gc-sections` strips the rest and the size report is that effect's real cost.
Once per second it prints a machine-parseable line:

```
[FXCOST] effect=PLATE cpu=12.3 memI16=3 memF32=21 heapRAM2=45120
```

`heapRAM2` (bytes malloc'd from the RAM2 heap) matters most: the effects allocate their delay
buffers on the heap, which `teensy_size`'s **static** report can't see — so the true RAM cost
is a runtime number.

## Run it

```sh
# one effect on the bench board:
pio run -e fx_plate -t upload      # then open the serial monitor to hear + see [FXCOST]

# the whole cost table (build-only, no board needed for FLASH/static RAM):
python ../../tools/fx_cost.py

# add CPU% + heapRAM2 (needs the board on COM4):
python ../../tools/fx_cost.py --port COM4
```

Output: `planning/plate-reverb-fx/COST.md` (+ `cost.json`). That table decides how many FX
slots each mix-kit env can afford — the engines⇄FX tradeoff.

## Envs

`fx_none` (baseline), `fx_plate`, `fx_spring`, `fx_reverbsc` (PSRAM-only), `fx_delay`,
`fx_phaser`. Add more by dropping one `#elif` block into `main.cpp` (headers are in
`lib/TDspHexeFx/src`) and one `[env:fx_*]` into `platformio.ini` — see the TODO list at the
bottom of `platformio.ini`.

## Notes

- Reuses the mix-kit's `include/tdsp_hw_config.h` (mux helper) via `-I`; no board files copied.
- `AudioOutputTDM_F32` is declared **first** (owns SAI1 `update_responsibility` — see
  `project_f32_update_order`). Don't reorder.
- If the DAC is silent after flashing, **power-cycle the board first** (`project_codec_power_cycle`).
