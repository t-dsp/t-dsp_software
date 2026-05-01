# 05 — Roadmap

Phased migration plan, per-panel component inventory, first spike,
risks, firmware dependencies.

## Sequencing principle

**Prove the navigation idea in vanilla code before touching the
framework.** Sel state, the persistent bottom strip, and the
five-workspace regroup are all additive changes that work in the
existing codebase. They are also the highest-value changes for
day-to-day use. If they alone close most of the navigation gap, the
framework migration becomes optional.

Only *after* navigation is proven do we migrate to Solid + Tailwind.
Within the framework migration, port one panel at a time, ship and use
each, and abandon if the ergonomics don't pay off.

## Phases

### Phase 0 — Sel + persistent bottom strip in vanilla (Spike 0)

**Goal:** prove that "Sel + always-visible mini-mixer" is the right
primitive. No framework, no rewrites.

**Tasks:**

1. Add `selectedChannel: Signal<number>` to `MixerState` in
   [state.ts](../../projects/t-dsp_tac5212_audio_shield_adaptor/tools/web_dev_surface/src/state.ts).
2. Add a Sel button to each channel strip in
   [channel-pair.ts](../../projects/t-dsp_tac5212_audio_shield_adaptor/tools/web_dev_surface/src/ui/channel-pair.ts);
   tap sets the signal.
3. Add a new `bottom-strip.ts` UI component:
   - 8 mini-faders + Sel + main meter+fader.
   - Subscribes to existing channel signals; writes via dispatcher.
   - Always docked at the bottom of `#app`.
4. Wire bottom strip into [main.ts](../../projects/t-dsp_tac5212_audio_shield_adaptor/tools/web_dev_surface/src/main.ts).
   It should be visible on all existing tabs.
5. Add a Mode toggle to the header (Engineer / Musician). For now,
   Musician mode swaps the bottom strip for the existing
   [keyboard.ts](../../projects/t-dsp_tac5212_audio_shield_adaptor/tools/web_dev_surface/src/ui/keyboard.ts).
6. Persist `mode` to localStorage.

**Done criteria:**
- [ ] Sel state set from any strip's Sel button updates a single global
      signal.
- [ ] Bottom strip is visible on every existing tab.
- [ ] Faders in the bottom strip drive and reflect the same signals as
      the main MIX faders (round-trip via dispatcher).
- [ ] Mode toggle swaps the bottom strip; preference persists across
      reloads.
- [ ] No regressions in meter rate, fader feel, OSC traffic.

**Touch:** no framework migration. New CSS classes added to
`style.css`. New strip widget hand-rolled like the existing ones.

### Phase 1 — Five-workspace regroup (Spike 1)

**Goal:** put existing panels under MIX/PLAY/TUNE/FX/SETUP without
rewriting them.

**Tasks:**

1. Replace the flat tab bar in `main.ts` with the five-workspace shell.
2. Add inner sub-tabs for PLAY (`Synths`/`Arp`/`Beats`/`Loop`) and FX
   (`Bus FX`/`Main Processing`/`Spectrum`) and SETUP (`Codec`/`Clock`/
   `System`/`Raw OSC`/`Serial`).
3. Move existing panel mounts under their new homes:
   - MIX: existing channel-pair / synth-bus / main-bus / host-strip
   - PLAY > Synths: existing synth sub-nav + per-engine panels
   - PLAY > Arp: existing arp-panel
   - PLAY > Beats: existing beats-panel
   - PLAY > Loop: existing looper-panel
   - TUNE: stub page (no firmware backing yet)
   - FX > Bus FX: existing fx-panel
   - FX > Main Processing: existing processing-panel
   - FX > Spectrum: existing spectrum.ts
   - SETUP > Codec: existing codec-panel (TAC5212 + ADC6140)
   - SETUP > Clock: existing clock-panel
   - SETUP > System: existing host-strip / input-host-strip / system bits
   - SETUP > Raw OSC: existing raw-osc.ts
   - SETUP > Serial: existing serial-console.ts
4. Add `activeWorkspace`, `activePlayTab`, etc., signals; wire tab
   buttons to them.
5. Mode toggle now also sets default landing workspace and tab order.

**Done criteria:**
- [ ] All five workspaces are reachable.
- [ ] Every existing panel renders in its new home and works as before.
- [ ] Tab state persists to localStorage so reload returns to the same
      workspace.
- [ ] Engineer mode lands on MIX; Musician mode lands on PLAY.
- [ ] Bottom strip from Phase 0 still works correctly across all
      workspaces.
- [ ] No new functionality, no rewrites — pure nav reorg.

### Phase 2 — Vite + Solid + Tailwind alongside vanilla (Spike 2)

**Goal:** add the new toolchain in the same bundle, port one panel as
proof.

**Tasks:**

1. Add dependencies: `solid-js`, `vite-plugin-solid`, `@tailwindcss/vite`.
2. Update `vite.config.ts` to include both plugins.
3. Add `tailwind.css` with the design-token theme block from
   [03-design-system.md](03-design-system.md).
4. Write `signal-bridge.ts` (the `asSolid` / `asSolidRW` adapters).
5. Pick one panel to port. **Recommendation: the codec panel**, because
   it is descriptor-driven (the data shape is already declared in
   `codec-panel-config.ts` and `adc6140-panel-config.ts`), so the port
   is mostly a renderer rewrite without state-model changes.
6. Solid version: `ui/codec-panel.tsx` consumes the same descriptor and
   renders with Tailwind utilities + design tokens. Mounts into the
   SETUP > Codec tab.
7. Delete the old `ui/codec-panel.ts` only after the new one is in use
   for at least a few sessions.

**Done criteria:**
- [ ] Build completes; bundle size delta acceptable (target: < 300 KB
      added gzipped).
- [ ] Codec panel renders with new touch-first sizing — every control
      ≥ 44 px tap target.
- [ ] All TAC5212 and ADC6140 controls work: enums, toggles, actions.
- [ ] No regressions on other panels.
- [ ] Hot reload works on the new TSX file.

### Phase 3 — Build the touch-first control library

**Goal:** Solid components for the audio-specific controls used in
many places.

**Components to build:**

| Component | LOC est. | Used in |
|---|---|---|
| `Fader` (vertical) | ~150 | MIX strips, TUNE strip, bottom-strip |
| `Knob` (rotary) | ~120 | All synth panels, FX, processing |
| `StepPad` (toggle pad) | ~60 | Beats grid, arp step mask |
| `MeterBar` (vertical) | ~80 | All strips |
| `BusButton` | ~40 | Bus picker in MIX |
| `Tabs` (workspace + inner) | ~80 | Shell |
| `Drawer` (pull-down/up) | ~100 | Settings drawer, peek-mixer |
| `Pill` (selectable chip) | ~30 | Synth engine picker, preset cards |
| `ToggleSwitch` | ~40 | midiAuto, on/off, etc. |
| `Select` (custom dropdown) | ~80 | Bus picker, engine picker |

Each is implemented as a Solid component reading/writing via
`signal-bridge.ts`. All use the design tokens from
[03-design-system.md](03-design-system.md).

**Done criteria:**
- [ ] Each component has at least one consumer in a new TSX panel.
- [ ] Visual consistency: all controls use tokens, not magic numbers.
- [ ] Touch tested on the Surface — no precision issues on any control.

### Phase 4 — Per-panel migration

Port the remaining panels one at a time, each as its own commit. Order
by frequency-of-use (port the most-used first):

1. Channel strip + main bus + synth bus + host strip → unified `Strip`
   component (MIX workspace)
2. Bottom strip mini-bank → real Solid component (replaces the Phase 0
   vanilla version)
3. Synth panels (Dexed, MPE, Neuro, Acid, Supersaw, Chip) — six
   parallel ports; share `Knob` / `Pill` / `ToggleSwitch`
4. Arp panel
5. Beats panel
6. Looper panel
7. Clock panel
8. FX + Processing panels
9. Spectrum (mostly a re-mount; the canvas drawing stays)
10. Keyboard
11. Raw OSC + Serial console (low priority — these are dev tools)

After each port, **delete the corresponding `.ts` file** so legacy
code doesn't drift.

### Phase 5 — TUNE workspace

**Blocked** on firmware exposing per-channel HPF / EQ / dynamics. When
unblocked:

1. Add `channels[i].hpf`, `channels[i].eq.bands[]`, `channels[i].dyn`
   to `state.ts` with appropriate signal types.
2. Extend `dispatcher.ts` to translate `/ch/NN/eq/B/...` and similar.
3. Build TUNE workspace Solid components: parametric EQ canvas with
   draggable band points, dynamics graph, HPF section, pan, sends
   matrix.

Until then, TUNE shows pan + sends only (those are addressable today).

### Phase 6 — Sends-on-faders

If buses other than Main/Synth/FX exist by this point, wire the full
mode. Otherwise, ship with current buses and the affordance in place.

1. Add `sofMode: Signal<boolean>` and `sofBus: Signal<string>` to state.
2. MIX workspace gets a Sends-on-faders toggle and a Bus picker.
3. The unified Strip component reads from `sofMode`/`sofBus` and
   renders either the channel's own fader or its send-to-bus fader.

### Phase 7 — Gesture polish

1. Long-press on strip header → context menu (rename, copy, paste, reset).
2. Swipe on tab bar → next/prev workspace.
3. Two-finger drag → fine mode.
4. Pull-down from header → settings drawer.
5. Pull-up from bottom strip → peek-mixer overlay.

### Phase 8 — Cutover

1. Verify no `ui/*.ts` panels remain (all are `.tsx`).
2. Delete legacy `main.ts`; new `main.tsx` is the only entry.
3. Delete `style.css` modulo any remaining `@layer components` bits
   absorbed into `tailwind.css`.
4. Update [README.md](../../projects/t-dsp_tac5212_audio_shield_adaptor/tools/web_dev_surface/README.md)
   to reflect the new architecture (it currently claims "no framework
   dependencies").
5. Tag a release.

## Component inventory (per-panel migration table)

| Existing panel | LOC | Action | New home | Notes |
|---|---|---|---|---|
| `acid-panel.ts` | 273 | Port | PLAY > Synths > Acid | Knobs |
| `arp-panel.ts` | 608 | Port | PLAY > Arp | Largest panel; step grid + scale picker |
| `beats-panel.ts` | 374 | Port | PLAY > Beats | StepPad grid |
| `beats-presets.ts` | 180 | Keep | n/a | Pure data |
| `cells.ts` | 197 | Replace | n/a | Replaced by Fader / MeterBar |
| `channel-pair.ts` | 110 | Port | MIX | Use unified Strip |
| `chip-panel.ts` | 281 | Port | PLAY > Synths > Chip | |
| `clock-panel.ts` | 259 | Port | SETUP > Clock | |
| `codec-panel.ts` | 183 | Port (Spike 2) | SETUP > Codec | Descriptor-driven; port is straightforward |
| `connect.ts` | 22 | Port | Header | Tiny |
| `dexed-panel.ts` | 268 | Port | PLAY > Synths > Dexed | |
| `fx-panel.ts` | 138 | Port | FX > Bus FX | |
| `host-strip.ts` | 86 | Port | MIX (right dock) | Use unified Strip |
| `input-host-strip.ts` | 86 | Port | MIX (right dock) | Use unified Strip |
| `keyboard.ts` | 303 | Port | Bottom strip (Musician) + PLAY > Synths (Engineer) | Touch improvements: slide between keys, vel from Y |
| `looper-panel.ts` | 286 | Port | PLAY > Loop | Big transport buttons |
| `main-bus.ts` | 94 | Port | MIX (right dock) | Use unified Strip |
| `mpe-panel.ts` | 733 | Port | PLAY > Synths > MPE | Complex; voice telemetry orbs |
| `neuro-panel.ts` | 693 | Port | PLAY > Synths > Neuro | Complex; stink chain |
| `processing-panel.ts` | 172 | Port | FX > Main Processing | |
| `raw-osc.ts` | 153 | Port | SETUP > Raw OSC | Lower priority |
| `serial-console.ts` | 206 | Port | SETUP > Serial | Lower priority |
| `spectrum.ts` | 686 | Re-mount only | FX > Spectrum | Canvas; minimal port |
| `supersaw-panel.ts` | 255 | Port | PLAY > Synths > Supersaw | |
| `synth-bus.ts` | 92 | Port | MIX | Use unified Strip |
| `util.ts` | 13 | Keep / port | shared | Tiny |

## Risks & open questions

### R1 — Native addons in Electron + Solid
Electron + serialport native addon is already working. Adding Solid
should not change anything in the main process; this is renderer-only.
**Mitigation:** verify `pnpm app:dev` still launches after Phase 2.
**Likelihood:** very low.

### R2 — Touch + Electron behavior
Electron's Chromium is the same as Chrome, so Pointer Events and
touch behaviors should work identically. **Mitigation:** test on the
Surface during Phase 0 already, before any framework change.
**Likelihood:** low.

### R3 — Firmware doesn't expose per-channel EQ / dynamics
TUNE is partly stubbed. **Mitigation:** ship Phase 0-4 without TUNE;
re-engage when [osc-mixer-foundation](../osc-mixer-foundation/README.md)
delivers per-channel processing.
**Likelihood:** certain — flagged as an explicit dependency, not a
risk.

### R4 — Per-channel sends don't exist on firmware
Sends-on-faders affordance ships with whatever buses exist. Full
multi-bus support comes when the firmware adds aux buses.
**Mitigation:** UI accepts any number of buses from a config; default
is just Main/Synth/FX.
**Likelihood:** certain — explicit dependency.

### R5 — Performance regression at 30 Hz
Adding Solid's reactivity could in principle add overhead, though it
shouldn't given fine-grained reactivity. **Mitigation:** measure
before/after each phase; meter at the same rate; spectrum at the same
fps.
**Likelihood:** low.

### R6 — Bottom strip + workspace + Solid panel triple-subscribe
A signal subscribed by the bottom strip (mini-fader), the main MIX
strip, and a TSignal native subscription could fire 3× per change.
This is fine — `Object.is` short-circuits in `Signal.set`, so each
listener fires once per actual change. Same fan-out the existing
code has.
**Likelihood:** non-issue; flagging for future debugging only.

### R7 — Solid `Show` / `For` mounting overhead vs. legacy unconditional render
Solid's `<For>` mounts/unmounts on list changes. The current code
unconditionally renders all panels and toggles `display: none`. The
migration could mistakenly use `<Show>` for tab switching, causing
remounts on every tab switch. **Mitigation:** use CSS `display: none`
for inactive workspaces in v1, exactly as the current code does.
Component identity is preserved across tab switches.
**Likelihood:** low if explicitly noted in code review.

### R8 — Bridge throttle violations from new components
Phase 0-7 components must continue to route fader-drag-derived sets
through `raf-batch.ts`. Solid's automatic effect tracking can hide
where a write happens. **Mitigation:** wrap the dispatcher's setter
methods to enforce ≤ 25 msg/sec per logical control.
**Likelihood:** medium; flagged for Phase 4 review.

## Firmware dependencies (explicit list)

These are firmware features that this rebuild expects to exist. Some
are present today; some are blocked.

| Feature | Status | Used by | If absent |
|---|---|---|---|
| `/snapshot` reply | Present | All workspaces (state hydration on connect) | App can still load with defaults |
| `/ch/NN/mix/fader`, `/ch/NN/mix/on` | Present | MIX | — |
| `/ch/NN/eq/B/...` (per-channel EQ) | **Absent** | TUNE | TUNE EQ stub only |
| `/ch/NN/dyn/...` (per-channel dynamics) | **Absent** | TUNE | TUNE Dyn stub only |
| `/ch/NN/hpf/...` (per-channel HPF) | **Absent** | TUNE | TUNE HPF stub only |
| `/ch/NN/sends/Bn/level` (per-channel sends) | **Partial** (Main only) | MIX sends-on-faders | Single-bus mode only |
| `/codec/tac5212/...` | Present | SETUP > Codec | — |
| `/codec/adc6140/...` | Present | SETUP > Codec | — |
| `/synth/{engine}/...` | Present | PLAY > Synths | — |
| `/arp/...` | Present | PLAY > Arp | — |
| `/beats/...` | Present | PLAY > Beats | — |
| `/loop/...` | Present | PLAY > Loop | — |
| `/clock/...` | Present | SETUP > Clock | — |
| `/processing/...`, `/fx/...` | Present | FX | — |
| `/sub` (meter subscribe) | Present (provisional format) | MIX, bottom strip, TUNE | — |
| `/-snap/save`, `/-snap/load` | Present | Header Scene picker | Scene picker disabled |

## Long-running pickup prompt

If running this epic via `/loop` or autonomous mode, here is the
prompt to fire:

> Pick up the UI rebuild epic from `planning/ui-rebuild/`. Read
> `README.md`, `00-overview.md`, `01-current-state.md`, and
> `02-hierarchy.md` first; they define what this is and why. Then read
> `05-roadmap.md` and find the lowest-numbered phase whose Done
> criteria are not met. Execute that phase's Tasks and verify against
> Done criteria.
>
> Honor these constraints (load-bearing):
> 1. Wire layer is sacred — `transport.ts`, `osc.ts`, `dispatcher.ts`,
>    `state.ts` (existing fields), `codec-panel-config.ts`,
>    `adc6140-panel-config.ts`, and `raf-batch.ts` are not modified
>    except to add explicitly-listed signals (`selectedChannel`, `mode`,
>    `sofMode`, `sofBus`, `activeWorkspace`, `activePlayTab`,
>    `activeFxTab`, `activeSetupTab`, `bottomStripExpanded`).
> 2. Never break existing OSC addresses or types.
> 3. No co-authored-by trailers on commits (per repo memory).
> 4. After Phase 1, all new components are Solid + Tailwind. Vanilla
>    code is only edited to the extent needed to coexist during
>    migration.
> 5. Verify each phase's Done criteria before claiming completion.
> 6. If a phase is blocked on a firmware feature listed in the
>    Firmware Dependencies table, ship the partial UI and document the
>    block in the phase's commit message — do not invent stub
>    addresses.
>
> Commit per phase (or per logical sub-phase if a phase is large).
> Tests: there are no automated tests for the web surface. Verification
> is manual: `pnpm app:dev`, exercise the affected panels, confirm OSC
> traffic via the serial console pane.
