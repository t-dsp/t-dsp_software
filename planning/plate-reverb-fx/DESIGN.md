# hexefx FX suite — design & plan

Add the **full hexefx `hexefx_audiolib_F32` effect suite** to the mix-kit as an optional,
build-flag-gated **FX pedalboard**: an ordered chain of runtime-bypassable effect slots that
any track can send to — or bypass straight to master — with per-slot params and a global
dry-blend. Two bus topologies (**master insert** and **per-track aux send**) behind one `@FX`
protocol and one "FX" section in the app.

Standard flow: prove it in a `/projects` spike first (starting with the plate reverb and a
**hard CPU/RAM measurement**), then integrate into `firmware/mix-kit` behind `TDSP_FX*`, then
add the app UI. Effects come online one slot at a time.

---

## 1. The library — full effect inventory (confirmed)

- **Source:** `hexefx_audiolib_F32` — https://github.com/hexeguitar/hexefx_audiolib_F32
- **License: MIT** — vendor the whole `src/` tree into `lib/TDspHexeFx/`.
- **Format: OpenAudio F32**, depends on `OpenAudio_ArduinoLibrary` — **already linked** by the
  mix-kit (`platformio.ini:46`). The whole mix bus is already F32 → **no int16 bridging.**

| Slot key | Class | F32 I/O | Kind | Key methods | Cost / gotcha |
|---|---|---|---|---|---|
| `PLATE`  | `AudioEffectPlateReverb_F32`     | 2→2 | reverb | size/time, hidamp/lodamp, lowpass/hipass, diffusion, freeze, mix/wet/dry, shimmer, pitch, bypass | RAM-moderate; the first slot we build |
| `SPRING` | `AudioEffectSpringReverb_F32`    | 2→2 | reverb | time, treble_cut/bass_cut, mix/wet/dry, bypass | RAM-moderate |
| `SC`     | `AudioEffectReverbSc_F32`        | 2→2 | reverb | feedback, lowpass, mix/wet/dry, freeze, bypass | **~396 KB buffer → PSRAM only** (`use_psram` ctor) |
| `DELAY`  | `AudioEffectDelayStereo_F32`     | 2→2 | delay  | time/delay, feedback, inertia, treble/bass(+cut), mix, mod_rate/depth, freeze, tap_tempo, bypass | PSRAM option for long delays |
| `PHASER` | `AudioEffectPhaserStereo_F32`    | 3→2 | mod    | lfo_rate, depth(top/btm), feedback, stages(2–12), stereo(phase), mix, bypass | 3rd input = mod CV, leave unconnected |
| `IPHASER`| `AudioEffectInfinitePhaser_F32`  | 1→1 | mod    | lfo_rate(±), depth, feedback, stages, mix, bypass | **mono** → dual instance for stereo |
| `WAH`    | `AudioEffectWahMono_F32`         | 3→1 | filter | setModel(G1..BASS/VOCAL), setFreq, setRange(heel/toe), setMix, bypass | **mono**, guitar-oriented |
| `DRIVE`  | `AudioEffectGuitarBooster_F32`   | 2→2 | drive  | drive(+range/norm), bottom, tone, bias, mix, volume, octave, bypass | mono processing |
| `EQ`     | `AudioFilterEqualizer_HX_F32`    | 1→1 | EQ     | equalizerNew(nBands, feq, adb, nFIR, cf, kdb), getResponse, bypass | **mono**, FIR = CPU-heavy; dual instance |
| `WIDE`   | `AudioEffectMonoToStereo_F32`    | 1→2 | util   | setSpread, setPan, bypass | mono→stereo helper |

**Two structural facts that drive everything below:**
1. **Stereo vs mono.** `PLATE/SPRING/SC/DELAY/PHASER/DRIVE` sit inline on the L/R bus.
   `IPHASER/WAH/EQ` are **mono (1→1)** → to keep the bus stereo they need a **dual L/R
   instance** (two objects sharing param setters) or live as a **single-track insert**. The
   mono guitar effects (`WAH`, `DRIVE`, `EQ`) are lower-priority for a stereo *master* bus —
   they shine as per-track inserts.
2. **PSRAM.** `SC` (396 KB) and long `DELAY` want PSRAM, which stock mix-kit builds don't
   have (see [[project_core_opi_psram]] — that's future). So `SC` is **PSRAM-gated**; on
   no-PSRAM builds the reverb options are `PLATE`/`SPRING`.

- Vendor these headers + their shared deps (`basic_delay.h`, `basic_lfo.h`,
  `filter_biquadStereo_F32.*`, `basic_DSPutils.*`, `wavetables.c`) — the whole small `src/`.
  LDF pulls them from the `#include` (same as every `lib/`). **Skip** `control_SGTL5000_F32`
  / `control_AK4452_F32` (those are for other codecs; we drive the TAC5212 ourselves).

---

## 2. Audio-graph design — the FX pedalboard

### 2.1 Constraints (real graph, `firmware/mix-kit/src/main.cpp`)

- **Master = `AudioMixer4_F32 outL/outR`** (`main.cpp:293`), a hard **4-slot ceiling**, already
  `0=BT,1=tone,2=S/PDIF,3=synth`; multi-voice builds fold everything into slot 3.
- **A `finalL/finalR` second stage already exists** (`main.cpp:325`, today `TDSP_AUDIOLOOP`-gated):
  `outL/outR → finalL/finalR[0] → tdmOut`, loop returns in `final[1..3]`. **This is the model**
  for the FX insert/return.
- F32, 48 kHz, 128-sample blocks throughout. One stereo effect object handles both channels.

### 2.2 The chain

Enabled slots are wired in a **fixed pedalboard order**, each **bypassed by default** and
toggled/parametrized at runtime (`bypass_set()`; hexefx bypass modes keep the tail/"trails").
Because the Teensy audio graph is static, "which effects exist" is a **compile-time** fact
(the `TDSP_FX_*` flags); "which are active + their settings" is **runtime**.

Canonical order (skip any slot not compiled in):

```
 chainIn ─► EQ ─► DRIVE ─► WAH ─► PHASER/IPHASER ─► DELAY ─► SPRING/SC/PLATE ─► chainOut
            (tone) (gain)  (filt)   (modulation)     (time)      (space)
```

Reverb last, drive early — standard signal flow. Adjacent slots connect L→L / R→R; a
compiled-out slot is simply not in the connection list (the neighbours link directly).

### 2.3 Two bus topologies (behind `@FX.ROUTE`)

**INSERT — "master → FX" (v1, decided default).** The chain is a master insert between the
master sum and the DAC; global dry/wet at the chain's tail reverb `mix()` (or a dedicated
`chainMix` node); `@FX.ON=0` bypasses the whole chain to clean.

```
 engines ─► outL/outR ─►[final]─► chainIn ─► …chain… ─► chainOut ─► tdmOut ─► DAC
```

**SEND — "tracks → FX" (v2, the matrix).** Each master-level source (track) gets a **send**
tap into a stereo FX-in mixer feeding `chainIn`; the chain's wet output returns to master via
a return trim; each track's **dry** path to master is untouched — that *is* "send to FX or go
direct, bypassing FX." Rows of the matrix = the things that feed the master (synth/pool,
drums, voice-2, BT-in), *not* every MPE voice → fan-in stays inside the 4-slot FX-in mixer.

```
 track ─┬─(dry)──────────────► outL/outR ─┐
        └─(send)─►fxInL/fxInR ─► chain ───┴─►[final]─► tdmOut
```

- **Tap points** (one send per master-level track): single-engine builds tap the engine's
  post-trim F32 out; Dexed pool taps `dxpLimit` (`DexedPoolSink.h:141`); hetero taps
  `g_hoOpllTrim` (`HeteroOpll.h:50`); drums tap the drum out; BT taps `btToF32L/R`. Each tap
  = an `AudioMixer4_F32` used as a runtime gain (the existing `g_hoOpllTrim` idiom).
- **Master send** (`@FX.MASTERSEND`) taps `outL/outR` (dry sum, *pre*-return) → whole mix into
  the chain with **no feedback loop** (return lands downstream in the final stage).

### 2.4 Dry-mix handling ("mix in the dry")

- **INSERT:** global dry/wet is one `chainMix` slider (the tail reverb's `mix()`, or a small
  dry/wet mixer around the chain). 0 = dry, 100 = fully processed.
- **SEND:** dry is preserved per-track at master; the FX **return level** is the global wet.
  Expose a global **dry-bleed** (tail reverb `dry_level()`) so the return carries some dry.

### 2.5 Output-stage slot budget (the one real conflict)

`finalL/finalR` has 4 slots; `TDSP_AUDIOLOOP` uses `[0]=bus, [1..3]=loops`. FX-return needs a
slot. Plan for a **cascade** when both features are on: `finalL/finalR → final2L/final2R[0]`,
`fxRet → final2[1]`, `final2 → tdmOut`. FX-only builds are trivial (`final[0]=bus,[1]=fxRet`).
Document the slot map in a header comment like the current `outL/outR` map.

### 2.6 Mono effects in a stereo chain

`IPHASER/WAH/EQ` are 1→1. Two options, per slot:
- **Dual instance** (default for chain use): two objects, L and R, param setters fan out to
  both. Doubles that slot's CPU/RAM — acceptable for `IPHASER`; watch `EQ` (FIR is heavy).
- **Single-track insert** (better fit for `WAH/DRIVE/EQ`): offer these as an insert on one
  track rather than the stereo master bus. Deferred to a later phase; note the seam now.

---

## 3. Build flags

Umbrella + per-slot, following the `-D TDSP_*` + `#if` pattern:

- **`TDSP_FX=1`** — umbrella gate: the FX bus scaffolding (chain plumbing, output-stage
  cascade, `@FX` dispatch, `@STATE."fx"` block, `caps.fx`, send-matrix taps).
- **`TDSP_FX_SEND=1`** — adds the per-track send taps (SEND mode). INSERT-only builds omit it
  to save the tap RAM.
- **Per-slot:** `TDSP_FX_PLATE`, `TDSP_FX_SPRING`, `TDSP_FX_REVERBSC`, `TDSP_FX_DELAY`,
  `TDSP_FX_PHASER`, `TDSP_FX_IPHASER`, `TDSP_FX_WAH`, `TDSP_FX_DRIVE`, `TDSP_FX_EQ`,
  `TDSP_FX_WIDEN`. Each adds its object + chain wiring + param handlers. `TDSP_FX_REVERBSC`
  additionally requires a PSRAM board profile (`#error` guard otherwise).
- **Per-env subsets** (decided target envs `teensy41`, `teensy41_opll`, `teensy41_sf2_tsf`,
  `teensy41_dexed_pool`): the spike's CPU/RAM numbers decide how many slots each env can
  afford. Expected reality: light envs (`opll`) can host a **fuller chain**
  (`EQ+DELAY+PHASER+PLATE`); heavy envs (`sf2_tsf`, `dexed_pool`) get a **minimal chain**
  (e.g. `PLATE` only, INSERT-only) or FX off. Green-build the [[feedback_test_with_opll]]
  canary first. Non-FX builds stay byte-identical.

Reserve a `[fx_full]` and `[fx_lite]` `platformio.ini` fragment (`extends`) so envs opt into a
slot set with one line instead of repeating flags.

---

## 4. Control protocol — `@FX.<SLOT>.<PARAM>`

### 4.1 Global (in `handleControlLine`, `main.cpp:~2469`, before the final `else`)

| Command | Effect |
|---|---|
| `@FX.ON=0\|1` | enable/bypass the whole chain |
| `@FX.ROUTE=insert\|send` | topology |
| `@FX.MIX=<0..100>` | global dry/wet (insert) |
| `@FX.WET=<0..100>` / `@FX.DRY=<0..100>` | return level / dry-bleed (send) |
| `@FX.MASTERSEND=<0..100>` | master→chain send (send mode) |
| `@FX` (bare) | echo full `fx` state |

### 4.2 Per-slot (dispatched by slot key)

`@FX.<SLOT>.<PARAM>=<v>`, one small handler per compiled-in slot (`#if TDSP_FX_<SLOT>`), values
percent-in / float-internally. Examples:
- `@FX.PLATE.ON=1`, `@FX.PLATE.SIZE=60`, `@FX.PLATE.DAMP=40`, `@FX.PLATE.DIFF=70`, `@FX.PLATE.FREEZE=1`, `@FX.PLATE.SHIMMER=20`, `@FX.PLATE.PITCH=7`
- `@FX.SPRING.ON=1`, `@FX.SPRING.TIME=50`, `@FX.SPRING.TREB=..`, `@FX.SPRING.BASS=..`
- `@FX.SC.ON=1`, `@FX.SC.FB=70`, `@FX.SC.LPF=..`, `@FX.SC.FREEZE=1`  *(PSRAM builds)*
- `@FX.DELAY.ON=1`, `@FX.DELAY.TIME=300`, `@FX.DELAY.FB=45`, `@FX.DELAY.MIX=..`, `@FX.DELAY.MODRATE=..`, `@FX.DELAY.MODDEPTH=..`, `@FX.DELAY.FREEZE=1`, `@FX.DELAY.TAP`
- `@FX.PHASER.ON=1`, `@FX.PHASER.RATE=..`, `@FX.PHASER.DEPTH=..`, `@FX.PHASER.FB=..`, `@FX.PHASER.STAGES=6`, `@FX.PHASER.SPREAD=..`
- `@FX.IPHASER.ON=1`, `@FX.IPHASER.RATE=..`, `@FX.IPHASER.DEPTH=..`, `@FX.IPHASER.STAGES=..`
- `@FX.WAH.ON=1`, `@FX.WAH.MODEL=vocal`, `@FX.WAH.FREQ=..`, `@FX.WAH.MIX=..`
- `@FX.DRIVE.ON=1`, `@FX.DRIVE.DRIVE=..`, `@FX.DRIVE.TONE=..`, `@FX.DRIVE.LEVEL=..`, `@FX.DRIVE.OCT=1`
- `@FX.EQ.ON=1`, `@FX.EQ.LO=..`, `@FX.EQ.MID=..`, `@FX.EQ.HI=..`  *(map a small fixed band set onto `equalizerNew`)*
- `@FX.WIDEN.ON=1`, `@FX.WIDEN.SPREAD=..`, `@FX.WIDEN.PAN=..`

### 4.3 Per-track send (in `handleTrkCmd`, `main.cpp:2381`, beside `VOL=`)

- **`@TRK<i>.FXSEND=<0..100>`** → track `i`'s send-tap gain. Works for any track for free.

### 4.4 `@STATE` (`main.cpp:2935`)

- **caps:** `,"fx":%d` (`TDSP_FX?1:0`) drives the FX section; the app also reads the chain to
  know which per-slot cards to show.
- **`"fx"` object:** `{"on":..,"route":"insert","mix":..,"wet":..,"dry":..,"mastersend":..,
  "chain":[{"slot":"eq","on":..,"p":{...}},{"slot":"delay","on":..,"p":{...}},
  {"slot":"plate","on":..,"p":{...}}]}` — array = the compiled-in slots in chain order, so the
  app renders exactly the cards present. Emit each slot block under its `#if TDSP_FX_<SLOT>`.
- **per-track:** add `"fxsend":<0..100>` to each `tracks[]` entry.

All inside `#if TDSP_FX` → non-FX builds emit nothing extra.

---

## 5. App UI (`app/tdsp-control/App.tsx` + `src/transport*.ts`)

The app is a single `sections[]` array with `caps`-gated `show`; mirror the Metronome/TAC5212
cards (sliders + switch + pills) and the `midiInputBody` grid (`App.tsx:1445`) for the matrix.

1. **"FX" parent submenu** (id `fx`, `show: caps.fx`) mirroring the `settings`/`tempo` submenu
   pattern — groups the FX cards like the web bench's FX tab (which lives in the *separate*
   `t-dsp_web_dev` app, not here).
2. **FX Master card** (`parent:'fx'`): chain **Enable** switch, **Route** pills `Insert|Send`,
   global **Mix/Wet/Dry** sliders, **Master send** slider (send mode).
3. **Send matrix card** (`parent:'fx'`, `show: caps.fx && route==='send'`): rows = sendable
   tracks (`synthCount` + drums, labels from `trkNames`), each a **per-track send level
   slider** (decided) → `fxSend(i,v)` = `trk(i,'FXSEND='+v)`, hydrated from `tracks[i].fxsend`.
   Structurally the `midiInputBody` grid with a slider cell.
4. **One card per chain slot**, rendered **data-driven from `@STATE.fx.chain[]`** so the app
   shows exactly the compiled-in effects: e.g. Plate (size/damp/diff/freeze/shimmer),
   Delay (time/fb/mix/mod, tap-tempo button), Phaser (rate/depth/stages/spread), EQ (lo/mid/hi),
   Drive (drive/tone/level/octave), etc. Each card: per-slot **Bypass** switch + its sliders,
   sending `@FX.<SLOT>.<PARAM>` on `onSlidingComplete` (spare the link,
   [[project_serial_bridge_throttle]]).
5. **Transport methods** (`transport.ts` + `.web/.native/.wifi`): `fxEnable`, `fxRoute`,
   `fxMix/Wet/Dry`, `fxMasterSend`, `fxSend(i,v)`, and a generic per-slot
   `fxSlot(slot, param, v)` → builds `@FX.<slot>.<param>=<v>` so new slots need no new method.
6. Reference for parameter ranges/labels: `projects/t-dsp_web_dev/src/ui/fx-panel.ts`.

---

## 6. Phasing / milestones

1. **Spike** — `projects/spike_fx_plate_reverb/`. Vendor `lib/TDspHexeFx`; wire
   testTone/BT → **PLATE** → TDM out; serial knobs. Prove audio on COM4, capture with
   `@CAP`/`tools/capture_analyze.py` ([[reference_output_capture]]), and **record RAM +
   `AudioProcessorUsageMax` per effect** — build the same spike with `DELAY`, `PHASER`, `EQ`,
   `SPRING` swapped in to get a **per-effect CPU/RAM cost table**. Ship `tools/fx_cost.py`
   (build-matrix → `COST.md`) and the build-time budget check (§6.5) in this phase — the cost
   table decides the per-env slot subsets. Watch [[reference_itcm_boundary_cliff]]; `FLASHMEM`
   cold init; power-cycle the codec first ([[project_codec_power_cycle]]).
2. **FX framework + PLATE, INSERT** — `TDSP_FX` + `TDSP_FX_PLATE`; chain plumbing (single
   slot), output-stage cascade, `@FX.*` + `@FX.PLATE.*`, `@STATE.fx`, `caps.fx`. Delivers
   master→FX + dry blend. Green-build `teensy41_opll`+`teensy41`, flash-verify.
3. **App: FX section + Plate card** — usable end-to-end over USB/BLE/Wi-Fi.
4. **More slots** — add `DELAY`, `SPRING`, `PHASER`, `EQ`, `WIDEN` as chain slots (each its
   flag, handler, card). App auto-shows them from `@STATE.fx.chain[]`. Per-env subsets set by
   the Phase-1 cost table.
5. **SEND matrix** — per-track taps + `fxInL/fxInR` + return; `@TRK<i>.FXSEND`;
   `tracks[].fxsend`; app matrix card. Delivers tracks→FX.
6. **PSRAM + guitar/mono slots** — `REVERBSC`, long `DELAY` on a PSRAM core
   ([[project_core_opi_psram]]); `WAH`/`DRIVE`/`EQ`/`IPHASER` as per-track inserts.

---

## 6.5 Resource-budget guardrail — "whatever compiles is tested, so we don't max out"

The core value the user wants: FX slots and synth-engine count draw from **the same finite
budget** (FLASH, RAM1/DTCM, RAM2/OCRAM/DMAMEM, and CPU %). The user should be free to trade
"more engines / fewer FX" ↔ "fewer engines / more FX", and **any combination that compiles is
proven to fit** before it's flashed. Two halves:

### A. Static budget (FLASH + RAM) — checked at build time, hard fail

- **Cost table** (`tools/fx_cost.py` + `projects/spike_fx_plate_reverb/`, **built — Phase 1 done**):
  builds a matrix of one-effect-at-a-time spikes (`fx_none`, `fx_plate`, `fx_spring`, `fx_delay`,
  `fx_phaser`, `fx_reverbsc`, …), runs `teensy_size` on each ELF, and emits
  `planning/plate-reverb-fx/COST.md` — **ΔFLASH / ΔRAM1 per effect** vs the `fx_none` baseline.
- **Heap caveat (measured, important):** the hexefx effects `malloc()` their delay/allpass
  buffers from the **RAM2 heap** at construction, so `teensy_size`'s *static* numbers show
  ~0 extra RAM for them (plate's static ΔRAM1 is only ~1.9 KB — the buffers are elsewhere). The
  real RAM cost is therefore a **runtime** number: the spike reports `heapRAM2` (`mallinfo().uordblks`)
  in its `[FXCOST]` line, and `fx_cost.py --port` folds it into the table. **Conclusion: the
  build-time check catches FLASH + static RAM; the runtime `heapRAM2`/CPU pass catches the
  dynamic cost. Both are required — a static-only guardrail would miss the biggest RAM users.**
- **Per-board budget table** in the repo (`include/tdsp_fx_budget.h` or a small TSV): the
  usable FLASH/RAM1/RAM2 ceiling per board profile (leave headroom for stack + audio blocks +
  the ITCM 32 KB-block rounding, [[reference_itcm_boundary_cliff]]).
- **Build-time check** — a PlatformIO `extra_scripts` post-`buildprog` hook (sibling of the
  existing `tools/cores_overlay.py`) that reads the linker size output and **errors the build**
  if RAM1/RAM2/FLASH exceeds the target's budget. This makes "it compiled" ⇒ "it fits RAM" a
  guarantee, not a hope. (Linking already fails on hard RAM1 overflow; this catches the softer
  "fits but no stack headroom" cases the linker allows.)

### B. Dynamic budget (CPU %) — measured, self-reported, can't be a pure build check

CPU load isn't known at link time, so the guardrail is measure-then-assert:

- The spike (and later the mix-kit) prints **`AudioProcessorUsageMax()`** (and the F32 memory
  high-water) over serial, per effect and for the full enabled chain, via a `@FXMEAS` command.
  `tools/fx_cost.py` captures these into `COST.md` alongside the static numbers → the CPU
  column of the cost table.
- **A documented CPU budget per env** (e.g. "≤ 70 % peak with the synth engine active"). Each
  candidate env's chain is validated against it from the cost table before the env is added.
- **Firmware self-guard at runtime:** on boot the mix-kit logs `AudioProcessorUsageMax` after a
  warm-up and raises a `@STATE."fx".over` flag (app shows a warning) if peak crosses a
  threshold — so even a hand-tweaked build that slips past review surfaces the overload instead
  of glitching silently.

### C. The tradeoff surface

Because engine count is already flag-driven (`TDSP_SYNTH_VOICES`, `TDSP_DEXED_VOICES`,
`TDSP_OPLL_ENGINES`, pool size) and FX are now flag-driven too, an env is just a **line-item
budget**: `Σ(engine costs) + Σ(FX slot costs) ≤ board budget`. `tools/fx_cost.py` gives every
line item; the build-time check enforces the sum. Ship a few **named preset envs** that
pre-balance the tradeoff, e.g.:
- `teensy41_opll_fxfull` — 1 light engine + `EQ+DELAY+PHASER+PLATE`
- `teensy41_dexed_pool_fxlite` — 8-engine pool + `PLATE` INSERT-only
- `teensy41_sf2_tsf_fxmin` — TSF + `PLATE` INSERT-only (or FX off if it doesn't fit)

Each preset's numbers come straight from the cost table, so its label is a *measured* promise.

---

## 7. Risks / open questions

- **CPU budget is now the headline risk.** A full chain (EQ+drive+phaser+delay+reverb) on
  Teensy 4.1 @ 48k/128 could run 40–60 %+ CPU, on top of the synth engine. The Phase-1 cost
  table is the gate; expect light envs to host more slots than heavy ones.
- **RAM fit** — `SC` is PSRAM-only; heavy engine envs (`sf2_tsf`, `dexed_pool`) may afford
  only `PLATE`, INSERT-only.
- **Output-stage slot contention** with audioloop → cascade (§2.5).
- **Mono effects in a stereo bus** → dual-instance cost or defer to per-track inserts (§2.6).
- **Codec quirks** — power-cycle-first before diagnosing "no FX" ([[project_codec_power_cycle]]).
- **SEND tap points** lean on the tracks refactor; INSERT is independent and ships first.

## Decisions (locked)
1. **Scope:** the **whole hexefx suite** as build-selectable chain slots — not reverb only.
2. **Routing:** INSERT ships first (v1), SEND matrix next.
3. **Matrix cell:** per-track **send level slider**, not on/off grid.
4. **First effect:** PLATE reverb is the first slot proven + integrated; the framework
   generalizes to the rest.
5. **Envs:** `teensy41` + `teensy41_opll` + `teensy41_sf2_tsf` + `teensy41_dexed_pool`; slot
   subsets per env decided by the Phase-1 CPU/RAM cost table (heavy envs likely INSERT-only /
   minimal chain).
