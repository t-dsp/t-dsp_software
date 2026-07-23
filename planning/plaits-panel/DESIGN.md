# Plaits voice panel — DESIGN

A device-specific **voice/timbre editor** for a Plaits synth track in the `tdsp-control` app.
When a track's `@STATE` engine tag is `"plaits"`, its synth detail page renders a Plaits control
surface — the model LED matrix + the HARMONICS / TIMBRE / MORPH macros + the LPG (decay/colour) —
mirroring the Mutable Instruments Plaits faceplate ergonomics.

Status: **Phase 1 + 2 implemented & verified** on branch `worktree-plaits-panel`.
Phase 3 (24-model port) is specced here, not built.

---

## Framing: it edits the VOICE, not the notes

The panel is a **timbre editor**. MIDI (keyboard / player / arp) only *plays* the voice; pitch comes
from the played note. So the hardware controls that only make sense with CV or a hardware pitch knob
are **deliberately omitted**:

- **FREQUENCY dial** — pitch is the MIDI note; a base-pitch knob is redundant.
- **Range switch** (Eurorack/free/LFO) — same reason.
- **CV attenuverters** (the two small knobs under TIMBRE/MORPH) — they scale external CV that doesn't
  exist in a MIDI-driven software voice.

What remains is exactly the set that shapes the engine's tone: **model + Harmonics + Timbre + Morph +
LPG Decay + LPG Colour.** Every control maps to a real firmware hook.

---

## Model / bank reality

`lib/TDspPlaits2` is the **original 16-engine Plaits** (`AudioSynthPlaits::kNumEngines == 16`):

- **Bank A (green), models 0–7** — pitched synthesis: virtual analog, waveshaping, 2-op FM, granular
  formant, harmonic, wavetable, chords, speech.
- **Bank B (red), models 8–15** — noise / percussion / speech: granular cloud, filtered noise,
  particle noise, inharmonic string, modal, analog bass drum / snare / hi-hat.

The panel's matrix colours by index and supports a **third amber bank (16–23)** that lights up
automatically once `ninstr` reports 24 — see Phase 3.

---

## Phase 1 — Firmware (`firmware/mix-kit/`, all `#if TDSP_HETERO_PLAITS`)

The Plaits track already existed (`HeteroPlaits.h`, `PlaitsVoiceEngineAdapter`, `engTag() == "plaits"`),
and **model selection already rode the generic per-track path** (`@TRK<i>.INSTR=` / `.INSTRS` /
`@STATE tracks[].{eng,ninstr,name}`). Only the continuous macros were missing.

### Protocol additions
| Command | Arg | Effect |
| --- | --- | --- |
| `@TRK<i>.HARM=`     | 0–1000 | `Plaits2Sink::setHarmonics(v/1000)` |
| `@TRK<i>.TIMBRE=`   | 0–1000 | `setTimbre` |
| `@TRK<i>.MORPH=`    | 0–1000 | `setMorph` |
| `@TRK<i>.LPGDECAY=` | 0–1000 | `setDecay` |
| `@TRK<i>.LPGCOLOR=` | 0–1000 | `setLpgColour` |

- Values are held as **int permille** (`g_hpHarm` … `g_hpColor`) so the wire + `@STATE` stay integer,
  matching the app's slider/knob convention; the sink takes 0..1 floats.
- Each command is **`voiceIsPlaits(i)`-gated** in `handleTrkCmd` (consistent with how OPLL is
  special-cased) and **echoes the clamped value** for confirm/rehydrate.
- `@STATE tracks[]` emits `harm/timbre/morph/lpgdecay/lpgcolor` for the Plaits track so the panel's
  knobs hydrate on connect/reconnect (no `@APP` needed — the firmware owns the values, like `INSTR`).
- `heteroPlaitsBegin()` seeds through the new setters so cache == sink at boot.

### Verified
`teensy41_opll_plaits1`, `teensy41_opll` (Plaits off → guard check), `teensy41_opl3_plaits1`,
`teensy41_dexed2_opll1_plaits1` — all build SUCCESS.

---

## Phase 2 — App (`app/tdsp-control/`)

- **`src/ui/PlaitsPanel.tsx`** — a fully controlled component:
  - **`Knob`**: a **dependency-free LED-collar rotary** (no `react-native-svg`). A ring of tick marks
    lit to the value + a rotating pointer cap; vertical `PanResponder` drag (160 px = full sweep).
    Chosen over adding a native SVG dep, which would force a dev-client rebuild.
  - **Model matrix**: bank-coloured LED grid, prev/next + tap-to-select → `@TRK<i>.INSTR=`, with a
    live model name + per-model HARM/TIMBRE/MORPH hint readout.
  - **Advanced drawer**: LPG Decay + LPG Colour knobs.
  - **Macro sends** are throttled (leading + trailing, ~60 ms) to stay inside the serial/BLE budget.
- **`App.tsx`**: `plaitsMacros` state hydrated from `@STATE tracks[]` + reconciled by `@TRK` echoes;
  `PlaitsPanel` renders in `voiceBrowserBody()` when `trkEng[oi] === 'plaits'` (replacing the flat
  model list). Works for any Plaits track index (bespoke Synth A/B card or a generated extra-voice card).
- Verified: `tsc --noEmit` clean.

### Ghost ring (honest scope)
The translucent inner collar on Timbre/Morph is a **static LPG preview** (`value + decay*0.3`), not a
live note-reactive pulse. True per-note LED pulsing / envelope animation would need the firmware to emit
a cheap per-track note-activity feed, which it doesn't today — deferred.

---

## Phase 3 — 24-model / amber DX7 bank (specced, NOT built)

Reaching the Plaits **1.2** (2022 firmware) model set means porting the upstream Mutable engines into
`lib/TDspPlaits2` and bumping `kNumEngines` 16 → 24. The new amber bank (16–23):

1. Classic waveshapes + resonant VCF
2. Phase distortion / modulation
3–5. Six-operator FM (three 32-preset DX7 banks)
6. Wave terrain synthesis
7. String machine
8. Chiptune (square voices, chords/arp)

### Risks to scope before starting
- **Flash size**: the 6-op FM banks ship large DX7 patch/resource tables. The engine is already
  flash-relocated (`plaits_flashmem.ld`); the new tables must land in FLASHMEM too.
- **RAM budget**: a 3rd melodic engine already busted DTCM on a dense build (see
  `project_multi_engine_tracks` / the ITCM/DTCM boundary cliff). Adding 8 engines' state needs a RAM
  audit; likely DMAMEM placement + per-build gating.
- **Block size**: upstream engines must honour the `kMaxBlockSize = 24` constraint the port already uses.

### App side
Already forward-compatible: the matrix colours by index and the amber bank lights up the moment
`ninstr` reports 24. Only the per-model HARM/TIMBRE/MORPH **hint strings** (in `PlaitsPanel.tsx`) need
the 16–23 rows filled in.

---

## Notes for whoever picks this up
- Built in an **isolated git worktree** (`.claude/worktrees/plaits-panel`, branch
  `worktree-plaits-panel`) to stay clear of another agent active on the main tree.
- The worktree has no `node_modules` (gitignored). To typecheck: junction it to the main tree's
  `app/tdsp-control/node_modules`, then `node node_modules/typescript/lib/tsc.js --noEmit`
  (TS 6.x — there is no `.bin/tsc`).
