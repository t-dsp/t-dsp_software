# Synth / Voices 2 — split the Dexed pool for a live USB keyboard

## Goal

Split the 8-engine Dexed pool so a **USB-host-connected keyboard** gets its own
dedicated instrument, chosen in a new app section **"Synth / Voices 2"**, while the
existing rig (song player + arp + drums + DIN/app input) keeps running unchanged on
the other half of the pool.

- Pool is 8 engines today (`dxp0..dxp7`, `DexedPoolSink g_poolSink(g_pool, 8, 2)`).
- When Voices-2 is **enabled**: engines **0–3** → existing path (voice 1), engines
  **4–7** → the USB keyboard (voice 2). Keyboard is a **live instrument** — it does
  **not** go through the arp.
- When **disabled**: reverts to the current unified 8-engine / 16-voice pool, one voice.
- Dexed-pool builds only (single-engine backends have nothing to split — see §7).

## Current state (verified anchors)

- **Pool + audio graph:** `firmware/mix-kit/src/SynthBackendDexedPool.h`
  - `kPoolN=8`, `kPoolVpe=2` (`:29-30`); engines `dxp0..dxp7` (`:32-35`).
  - Mixer tree: engines 0–3 → `dxpMixA`, 4–7 → `dxpMixB`, `dxpMixA+dxpMixB → dxpSum`
    (`:46-51`) → `dxpTrim` (ReplayGain) → `dxpLimit` → `outL/outR` slot 3 (`:97-98`).
    **Note: the mixer tree already groups engines 0–3 and 4–7 — the split falls on a
    seam that already exists.**
  - `g_poolSink` + `g_synthSink = &g_poolSink` (`:227-229`).
  - `synthSetInstrument(i)` / `synthPickCartVoice()` load the *same* voice into **all 8**
    engines (`:273-324`).
- **Pool sink:** `firmware/mix-kit/src/DexedPoolSink.h`
  - ctor already takes an **arbitrary engine sub-array** `(engines**, n, voicesPerEngine)`
    (`:28`) — two independent sinks over disjoint halves is natural.
  - `allocNormal()` round-robin (`:212-218`), `allocMpe()` one-engine-per-note (`:219-224`),
    `setMpeMode()` (`:33`), `panic()` (used on every mode change).
- **Routing:** `firmware/mix-kit/src/main.cpp`
  - USB host + DIN share `midiNoteOn/Off/CC/...` (`:1421-1442`) → `g_router`.
  - `g_router → g_arpFilter → g_synthSink` (`:1893-1895`); song player → arp (`:1901`);
    drums ch10 → `g_synthSink` direct (`:1905-1906`).
  - USB host is pumped independently: `g_usbHost.Task(); while (g_usbMidi.read()){}` (`:2025-2026`);
    handlers registered at `:1877-1885`. **DIN and USB host can be given separate callbacks.**
  - Command dispatch: `handleControlLine()` `:1475`; mode switch `applyMidiMode()` `:1447-1456`;
    `@STATE` snapshot `:1754-1784`.
- **App:** `app/tdsp-control/App.tsx`
  - `synth` section object `:691` (breadcrumb nav + FlatList/ScrollView voice browser),
    `pickVoice()` `:415`, `stepVoice()` `:416-432`, sections array `:589`,
    `SECTION_ORDER` `:903`.
  - Transport interface `src/transport.ts:66-98`; impls `transport.web.ts` (Web Serial),
    `transport.native.ts` (BLE `RELAY_LINE`). Voice methods `dxVoice`/`dxPick`/`cartVoices`.

## Design

### A. Firmware — pool split

Keep **one** 8-engine `g_poolSink` for the main path and add a **second** 4-engine sink
for the keyboard, over the top half of the *same* engine array. Only one configuration is
"live" at a time, so shared engine ownership is safe as long as we panic on toggle.

1. **`DexedPoolSink`: settable active-engine window.**
   Add `void setActiveEngineCount(uint8_t n)` (default `= _n`). `allocNormal()`/`allocMpe()`
   clamp their engine iteration to `[0, _nActive)`; the setter calls `panic()`.
   Main sink runs `_nActive = 8` (off) or `4` (on, engines 0–3).

2. **Second sink for the keyboard half.**
   In `SynthBackendDexedPool.h`:
   ```cpp
   DexedPoolSink g_poolSinkB(&g_pool[4], 4, kPoolVpe);  // engines 4..7
   ```
   `g_poolSinkB` stays in **normal poly** by default (a keyboard, not an MPE controller);
   `setMpeMode` can be exposed later.

3. **Range-scoped voice loading.** Refactor the load helpers to take an engine range:
   `loadVoiceRange(voice, start, count)` / `pickCartVoiceRange(...)`. Then:
   - `synthSetInstrument(i)` / `synthPickCartVoice()` load `[0, mainCount)` where
     `mainCount = split ? 4 : 8`.
   - **new** `synthSetInstrument2(i)` / `synthPickCartVoice2()` load `[4, 4)` (pool B).
   - Store voice-2 selection separately (`g_synthInstrument2`, `g_curCart2/…`), mirroring
     the existing `g_synthInstrument` / `g_curCartRel/Voice/Name` fields.

4. **Independent Voices-2 volume (cheap — the seam already exists).**
   Today `dxpMixA` (engines 0–3) and `dxpMixB` (engines 4–7) both feed `dxpSum → dxpTrim`.
   Re-tap so each half has its own ReplayGain/volume trim before recombining:
   `dxpMixA → dxpTrimA`, `dxpMixB → dxpTrimB`, both → final sum → `dxpLimit`.
   - Off: `trimA`/`trimB` carry voice-1 ReplayGain (identical), behaves as today.
   - On: `trimA` = voice-1 RG × main vol, `trimB` = voice-2 RG × Voices-2 vol.

### B. Firmware — routing the keyboard off to pool B

1. **Second router** `MidiRouter g_kbdRouter;` with a single sink `g_poolSinkB`
   (gives the keyboard the same pitch-bend-range / CC74 normalization the main router does).
2. **Separate the USB-host callbacks from DIN.**
   - DIN keeps `MIDI.setHandle*` → existing functions → `g_router` (unchanged).
   - USB host gets its own `usbNoteOn/Off/CC/Pitch/Pressure` that branch on a global flag:
     `g_split2 ? g_kbdRouter.handle*(…) : g_router.handle*(…)`.
   - Preserve the "first live note arms/starts the groove" behavior (`main.cpp:1425-1435`)
     by hoisting it into a shared helper both DIN and USB note-on call — a keyboard press
     should still SYNCHRO-start drums.
3. Keyboard path is **arp-free by design** (`g_kbdRouter → g_poolSinkB`, no `g_arpFilter`).

### C. Firmware — enable/disable command

`@SPLIT2=<0|1>` in `handleControlLine()`:

- **Enable:** panic all engines → `g_poolSink.setActiveEngineCount(4)` → route USB host to
  `g_kbdRouter` → load voice 2 into engines 4–7 → set `trimB` for voice 2.
- **Disable:** panic → USB host back to `g_router` → `g_poolSink.setActiveEngineCount(8)` →
  reload voice 1 across all 8 → `trimB` back to voice-1 RG.

New voice-2 commands (mirror the existing ones, targeting pool B):
- `@DXVOICE2=<i>` → `synthSetInstrument2(i)`
- `@DXPICK2=<relCart>\t<voice>` → `synthPickCartVoice2()`, reply `@DXPICKED2=…`
- `@VOL2=<f>` → Voices-2 volume (`dxpTrimB` make-up)
- Browse commands `@DXLS`/`@DXVL` are read-only SD reads → **reused as-is** (only the pick
  target differs).

Extend `@STATE` (`main.cpp:1754-1784`) to report `split2` (bool), the voice-2 selection
(`{cart,cv,name}` or `{i,name}`), and `vol2`, so the app rehydrates on reconnect.

### D. App — "Synth / Voices 2" section

1. **New section** `{ id:'synth2', title:'Synth / Voices 2', … }` pushed into `sections[]`
   near `App.tsx:689`; insert `'synth2'` into `SECTION_ORDER` (`:903`) right after `'synth'`.
2. Section body:
   - **Enable toggle** — "Split the USB keyboard onto its own instrument." Calls
     `tp.setSplit2(bool)`. When off, the rest of the section is disabled/greyed.
   - **Voice browser** — same breadcrumb + FlatList/cart browser as `synth`, but its
     pick handlers call `dxVoice2`/`dxPick2`. **Recommended:** extract the existing voice
     browser (state around `App.tsx:220-432` + body `:718-737`) into a reusable
     `<VoiceBrowser target={1|2}/>` so both sections share it; fallback is duplicated
     `selVoice2/…` state.
   - **Voices-2 volume** slider → `tp.setVoices2Vol(v)` (`@VOL2`).
3. **Transport additions** (`transport.ts` + both impls): `setSplit2(b)`, `dxVoice2(i)`,
   `dxPick2(rel,i)`, `setVoices2Vol(v)` — each an `@`-line, same pattern as `dxVoice`/`dxPick`.
4. `@STATE` parse (`App.tsx:254-265`) extended to restore split2 + voice-2 + vol2.
5. Expo is pinned — check `app/tdsp-control/AGENTS.md` / Expo v57 docs before app edits.

## Consequences / tradeoffs

- **Polyphony:** with the split on, the main rig drops from 16 → 8 voices (4 engines × 2),
  and the keyboard gets 8. Inherent to a 4/4 split; acceptable per the request.
- **MPE:** in MPE mode the main pool is limited to 4 simultaneous MPE notes while split.
  Keyboard pool B defaults to normal poly.
- **Split ratio** is a constant (4/4) but the window API (`setActiveEngineCount`) makes other
  ratios a one-line change if wanted later.
- **Non-pool synths** (single-engine SF2/TSF/OPLL/… backends) have no pool to split — the
  `synth2` section should be hidden/disabled unless `@STATE` reports a Dexed-pool build
  (add a `poolSplittable` flag to `@STATE`).

## Build/verify

- Green-build with `teensy41_opll` first (fits everywhere, per repo convention) to catch
  shared-code breakage, then the Dexed-pool env (`teensy41_dexed_pool`).
- HW check on COM4: plug a USB keyboard, enable Voices 2, confirm keyboard plays voice 2
  live while a song+arp+drums run on voice 1; toggle off and confirm 8-voice unified pool
  and keyboard rejoins the main path.
- Close the serial monitor before uploading.

## Implementation status (built on branch `synth-voices-2`)

Done and green-building (`teensy41_opll` + the new `teensy41_dexed_pool_nobt_voice2`;
app `tsc --noEmit` clean):

- **Gated behind two build flags**, per the ask: `TDSP_VOICE2` (the split) and `TDSP_ARP2`
  (a second arp on the keyboard path, implies voice 2). Both default to `0` and are only
  set in the new env. `@STATE` reports `"caps":{"voice2":..,"arp2":..}` so the **app shows
  the cards only when they're compiled in**.
- **Pool split**: `DexedPoolSink::setEngineCount()` + a second sink `g_poolSinkB` over
  `&g_pool[4]`; range-scoped voice loading; `synthSetInstrument2/synthPickCartVoice2/
  synthSetVoice2Enabled`. The main sink shrinks to 4 engines only while the split is on.
- **Routing**: `g_kbdRouter` + split USB-host callbacks (`usb*`), arp-2 wired
  `g_kbdRouter → g_arpFilter2 → g_synthSinkB` (or straight to the sink without `TDSP_ARP2`).
  The `@ARP...`/`@ARP2...` commands now share one parser (`handleArpLine`).
- **App**: `synth2` card gated on `caps.voice2`; the voice picker reuses the Synth browser
  in a `voiceTarget===2` mode (full bundled + `/dexed` parity, minimal duplication); Voices-2
  volume; arp-2 controls gated on `caps.arp2`. New transport methods on both platforms.

Deviation from the plan worth noting:

- **Voices-2 volume** is done by scaling the `dxpMixB` (engines 4..7) mixer gains rather
  than a separate ReplayGain trim node, so voice 2 currently shares voice 1's `dxpTrim`
  ReplayGain. Fine for phase 1 (SD carts ship at unity); a dedicated `dxpTrimB` re-tap is a
  phase-2 refinement.

### Phase 2 (deliberately deferred, not written off)

- **Voice 2 on the OTHER engine** (e.g. the Dexed-pool + OPLL dual-synth build): the routing
  seam is already generic — `g_synthSinkB` is a `tdsp::MidiSink*`, so a future OPLL-backed
  melodic sink can be assigned to it without touching `main.cpp`'s routing. Today the OPLL in
  that env is a channel-10 *drum* voice (`DrumVoice.h`), not a melodic engine, so making it a
  playable voice 2 is its own task.
- Fully independent voice-2 ReplayGain (`dxpTrimB`), and an optional MPE keyboard on voice 2.

## Task checklist

1. `DexedPoolSink::setActiveEngineCount()` + clamp alloc loops.
2. `SynthBackendDexedPool.h`: `g_poolSinkB`, range-scoped load helpers, voice-2 state,
   `dxpTrimA/dxpTrimB` re-tap, `synthSetInstrument2()`/`synthPickCartVoice2()`, `@VOL2`.
3. `main.cpp`: `g_kbdRouter`, split USB-host callbacks + shared first-note helper,
   `@SPLIT2`/`@DXVOICE2`/`@DXPICK2`/`@VOL2`, `@STATE` fields, `g_split2` flag.
4. App: `synth2` section, `VoiceBrowser` extraction (or duplicated state), transport methods,
   `@STATE` restore, non-pool hide.
5. Build (opll → dexed_pool) + HW verify on COM4.
