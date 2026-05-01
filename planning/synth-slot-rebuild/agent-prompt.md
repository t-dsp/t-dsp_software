# Synth Slot Rebuild — Agent Prompt

Copy this prompt into a fresh chat. Replace `{ENGINE}`, `{SLOT}`, `{LIB}`, and `{OSC_ID}` with your assigned values from the **Assignments** table. One agent per engine. Agents work in parallel; the per-engine files don't conflict, and each agent's edits to shared files are at distinct marked sections.

---

## Assignmentsyou are 

| Agent | Engine | Slot index | Library                    | OSC namespace        |
|-------|--------|-----------:|----------------------------|----------------------|
| 1     | MPE      | 3 | `lib/TDspMPE/`        | `/synth/mpe/`        |
| 2     | Neuro    | 4 | `lib/TDspNeuro/`      | `/synth/neuro/`      |
| 3     | Acid     | 5 | `lib/TDspAcid/`       | `/synth/acid/`       |
| 4     | Supersaw | 6 | `lib/TDspSupersaw/`   | `/synth/supersaw/`   |
| 5     | Chip     | 7 | `lib/TDspChip/`       | `/synth/chip/`       |

**Already-occupied slots — don't touch:**
- **Slot 0** — Dexed FM
- **Slot 1** — Multisample sampler (Salamander piano)
- **Slot 2** — Plaits-inspired MPE macro oscillator

With 5 agents at slots 3–7, every slot is now assigned (slot 7 is no longer reserved).

---

## Mission

Rebuild the **{ENGINE}** synth as **slot {SLOT}** in the new T-DSP slot architecture. The engine library (`{LIB}`) already exists and was wired up in the legacy project at `projects/t-dsp_tac5212_audio_shield_adaptor/src/main.cpp`. Your job is to:

1. Wrap that engine in an `ISynthSlot` adapter so it plugs into the `SynthSwitcher`.
2. Wire its audio output into the synth sub-mixer at slot {SLOT}.
3. Provide OSC handlers under `{OSC_ID}` (volume, on/off, MIDI channel, anything else relevant).
4. Build a clean dev-surface configuration panel that appears when the user picks slot {SLOT} in the slot picker.
5. Don't break the existing Dexed (slot 0) or Sampler (slot 1) wiring.

You're working in parallel with 4 other agents on different slots. Stay in your lane.

---

## Architecture overview

The active firmware project is **`projects/t-dsp_f32_audio_shield/`**. Read these files before you start:

- [src/synth/SynthSlot.h](projects/t-dsp_f32_audio_shield/src/synth/SynthSlot.h) — the `ISynthSlot` interface you'll implement
- [src/synth/SynthSwitcher.h](projects/t-dsp_f32_audio_shield/src/synth/SynthSwitcher.h) — single-active-slot router; you register your slot with `g_synthSwitcher.setSlot({SLOT}, &yourSlot)`
- [src/synth/DexedSlot.h](projects/t-dsp_f32_audio_shield/src/synth/DexedSlot.h) and [DexedSlot.cpp](projects/t-dsp_f32_audio_shield/src/synth/DexedSlot.cpp) — your simplest reference, a slim adapter around a single engine
- [src/synth/MultisampleSlot.h](projects/t-dsp_f32_audio_shield/src/synth/MultisampleSlot.h) and [MultisampleSlot.cpp](projects/t-dsp_f32_audio_shield/src/synth/MultisampleSlot.cpp) — fuller reference with bank state, voice management, sustain pedal
- [src/main.cpp](projects/t-dsp_f32_audio_shield/src/main.cpp) — file-scope audio graph. Search for `// --- Dexed FM synth` and `// --- Sampler slot 1` to see how existing slots are wired

Audio routing summary:

```
your engine → [I16→F32 if needed] → slot gain stage → g_synthSumLA/B[input] →
g_synthSumLC → g_synthBusL → mixL[1] → main fader → DAC
```

Slots 0..3 feed `g_synthSumLA` / `g_synthSumRA` at inputs 0..3. Slots 4..7 feed `g_synthSumLB` / `g_synthSumRB` at inputs 0..3. **Your slot {SLOT} feeds:**

- If {SLOT} is 0..3: input `{SLOT}` of `g_synthSumLA` and `g_synthSumRA`.
- If {SLOT} is 4..7: input `{SLOT-4}` of `g_synthSumLB` and `g_synthSumRB`.

The dev surface lives at **`projects/t-dsp_web_dev/`**. Read:

- [src/ui/sampler-panel.ts](projects/t-dsp_web_dev/src/ui/sampler-panel.ts) — your panel template (clean, plain-DOM, follows the established layout)
- [src/ui/dexed-panel.ts](projects/t-dsp_web_dev/src/ui/dexed-panel.ts) — reference for a more complex panel with dropdowns
- [src/state.ts](projects/t-dsp_web_dev/src/state.ts) — search for `SamplerState` and `synthSlot` for the state-shape pattern
- [src/dispatcher.ts](projects/t-dsp_web_dev/src/dispatcher.ts) — search for `setSampler` and `/synth/sampler` for the OSC-binding pattern

The legacy implementation of {ENGINE} (the one you're rebuilding) lives at:

- Firmware: `projects/t-dsp_tac5212_audio_shield_adaptor/src/main.cpp` — search for `g_{OSC_ID lowercased}` or `tdsp::{ENGINE}` to find its setup, audio nodes, and OSC handlers
- Library: `lib/TDsp{ENGINE}/` — the engine itself (don't modify; just use)
- Dev surface panel: `projects/t-dsp_web_dev/src/ui/{engine lowercased}-panel.ts` — the legacy panel. **Examine it first, then choose:**
  - If it's already speaking the slot-architecture OSC paths (`/synth/{engine}/...`) and rendering useful engine-specific controls, **just wire it in** — rename it to `{engine}-slot-panel.ts` and export `{engine}SlotPanel` so main.ts's import lines up with the convention. Keeping a rich, working panel beats rewriting it.
  - If it's stale (legacy OSC paths, dead controls, broken under the new state shape), **rewrite from scratch following the sampler-panel.ts pattern.**
  - When in doubt, ask the user before discarding rich UI like voice orbs, preset grids, or modulation matrices — those are usually the defining UX of an engine.

---

## Files you create

1. **`projects/t-dsp_f32_audio_shield/src/synth/{Engine}Slot.h`** — class declaration of `tdsp_synth::{Engine}Slot : public ISynthSlot`. Mirrors `DexedSlot.h` shape: pointer-based, doesn't own the audio nodes, just provides the slot interface.

2. **`projects/t-dsp_f32_audio_shield/src/synth/{Engine}Slot.cpp`** — class implementation. Implements `setActive()` (panic + zero gain on inactive, restore on active), `panic()`, `applyGain()`, and the `MidiSink` shim for note/CC routing.

3. **`projects/t-dsp_web_dev/src/ui/{engine-lowercase}-panel.ts`** — config panel UI. Layout follows [sampler-panel.ts](projects/t-dsp_web_dev/src/ui/sampler-panel.ts):
   - ON/OFF toggle row at top (`synth-on-row` class, reuse existing CSS)
   - Volume slider with dB readout
   - MIDI channel selector (0=omni, 1..16)
   - Engine-specific controls (whatever the legacy panel had — port them)

## Files you edit (small, surgical)

These are shared with other agents. Add your block at the end / in clearly-marked sections to minimize merge conflicts.

### `projects/t-dsp_f32_audio_shield/src/main.cpp`

Add **5 small blocks** at distinct locations:

1. **Include** at the top:
   ```cpp
   #include "synth/{Engine}Slot.h"
   ```

2. **Audio nodes** in the section commented `// --- Sampler slot 1` (after that block, before `// --- Synth sub-mixer chain (8 slots)`):
   ```cpp
   // --- Slot {SLOT} ({Engine}) ---
   // Audio nodes for the {ENGINE} engine. Output feeds g_synthSum{X}A/B[{INPUT}].
   tdsp::{Engine} g_{lowercase};
   AudioConvert_I16toF32   g_{lowercase}ToF32;     // if engine is int16; skip if F32 native
   AudioEffectGain_F32     g_{lowercase}Gain;
   tdsp_synth::{Engine}Slot g_{lowercase}Slot(...);
   ```

3. **AudioConnections** in the section commented `// --- Synth sub-mixer chain (8 slots) ---`:
   ```cpp
   // Slot {SLOT} ({Engine}) -> sub-mixer
   AudioConnection      c_{lowercase}_to_conv (g_{lowercase}, 0, g_{lowercase}ToF32, 0);
   AudioConnection_F32  c_{lowercase}_to_gain (g_{lowercase}ToF32, 0, g_{lowercase}Gain, 0);
   AudioConnection_F32  c_{lowercase}_to_sumL (g_{lowercase}Gain, 0, g_synthSum{LA_or_LB}, {INPUT});
   AudioConnection_F32  c_{lowercase}_to_sumR (g_{lowercase}Gain, 0, g_synthSum{RA_or_RB}, {INPUT});
   ```
   where `{LA_or_LB}` is `LA` for slots 0..3 and `LB` for slots 4..7, and `{INPUT}` is your slot index modulo 4.

4. **Switcher registration** in `setup()` — find the block that starts with `g_synthSwitcher.setSlot(0, &g_dexedSlot);` and *replace your slot's existing `&g_silentSlotN`* with `&g_{lowercase}Slot`:
   ```cpp
   g_synthSwitcher.setSlot({SLOT}, &g_{lowercase}Slot);  // was: &g_silentSlot{SLOT}
   ```
   Then call `g_{lowercase}Slot.begin()` (or equivalent init) right after.

5. **OSC handlers** under `onOscMessage()` — add a block of `if (strcmp(address, "{OSC_ID}/...") == 0) { ... }` handlers. Look at the existing `/synth/sampler/...` handlers for the exact pattern. Add at the end of the existing `/synth/...` handler chain, before the catch-all dispatcher fallback.

6. **Snapshot entries** in `broadcastSnapshot()` — add a block alongside the existing `// Slot 1 — multisample sampler.` block. Format:
   ```cpp
   // Slot {SLOT} — {Engine}.
   { OSCMessage m("{OSC_ID}/mix/fader"); m.add(g_{lowercase}Slot.volume()); reply.add(m); }
   { OSCMessage m("{OSC_ID}/mix/on");    m.add((int)(g_{lowercase}Slot.on() ? 1 : 0)); reply.add(m); }
   { OSCMessage m("{OSC_ID}/midi/ch");   m.add((int)g_{lowercase}Slot.listenChannel()); reply.add(m); }
   ```

### `projects/t-dsp_web_dev/src/state.ts`

Add a `{Engine}State` interface near the existing `SamplerState`. Add a `{lowercase}` field to `MixerState`. Add the default in `createMixerState`. Mirror the Sampler pattern exactly.

### `projects/t-dsp_web_dev/src/dispatcher.ts`

Add `set{Engine}On`, `set{Engine}Volume`, `set{Engine}MidiChannel` methods (and any engine-specific setters). Add inbound handlers for the OSC echoes. Mirror the existing Sampler block.

### `projects/t-dsp_web_dev/src/main.ts`

Three additions:

1. Import: `import { {engine}Panel } from './ui/{engine}-panel';`
2. Instantiate near the existing `samplerPanelEl`: `const {engine}PanelEl = {engine}Panel(state, dispatcher);`
3. Append to `synthContent` and add visibility toggle to the `state.synthSlot.active.subscribe` block.

---

## Conventions

- **Filenames** — `{Engine}Slot.h/cpp` for firmware (PascalCase), `{engine}-panel.ts` for dev surface (kebab-case).
- **C++ namespace** — slot classes go under `tdsp_synth::`. Engine library namespace stays as it is in `lib/`.
- **OSC paths** — always lowercase, dash-free: `/synth/mpe/`, `/synth/supersaw/` (not `/synth/Supersaw/`).
- **Variable prefixes** in main.cpp: `g_{lowercase}` for global instances (e.g., `g_mpe`, `g_mpeGain`, `g_mpeSlot`).
- **CSS classes** — reuse `.synth-on-row`, `.synth-on-btn`, `.synth-on-label` from the existing CSS for the ON/OFF row. Engine-specific styles go under `.{engine}-panel` selector.
- **Fader law** — use `tdsp::x32::quantizeFader` and `tdsp::x32::faderToLinear` for volume conversions, same as DexedSlot/MultisampleSlot.
- **MIDI channel default** — 0 (omni) unless your engine has a specific reason (MPE master channel = 1, member channels start at 2).

---

## Verification

Before declaring done, run from the repo root:

```bash
# Firmware compile
/c/Users/jaysh/AppData/Roaming/Python/Python313/Scripts/platformio.exe run \
    --project-dir projects/t-dsp_f32_audio_shield

# Dev surface type-check
cd projects/t-dsp_web_dev && pnpm exec tsc --noEmit
```

Both must pass without errors. Don't flash hardware — the user will flash once all 5 engines are merged.

---

## What NOT to do

- **Don't refactor existing code.** Don't touch DexedSlot, MultisampleSlot, SynthSwitcher, the audio graph for slots 0/1, or any unrelated panel.
- **Don't change kMaxSlots, kMaxSamples, kReleaseGain*, kVoices**, or other firmware tuning constants.
- **Don't add OSC paths outside `{OSC_ID}/`.** If you need a global setting, ask the user.
- **Don't import other engines' panels** in your panel file.
- **Don't add audio nodes outside your slot's wiring block.** No global FX, no shared state.
- **Don't try to be clever about merge ordering.** Your edits to shared files (main.cpp, state.ts, dispatcher.ts, main.ts) should be small and obviously local; the user manually resolves any conflicts.

---

## Done when

1. Firmware builds clean.
2. Dev surface type-checks clean.
3. Your panel renders in the slot picker when slot {SLOT} is selected.
4. The legacy panel for {ENGINE} (`projects/t-dsp_web_dev/src/ui/{engine-lowercase}-panel.ts`) remains untouched (it'll be deleted once you confirm your replacement works).
5. No existing slot is broken — slot 0 (Dexed) and slot 1 (Sampler/Piano) still play.

When done, commit your work as a single commit on a branch named `slot-rebuild/{engine-lowercase}`. Title: `firmware+ui: rebuild {Engine} as slot {SLOT}`.
