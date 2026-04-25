# Epic: UI Rebuild — Touch-First Web Surface

**Status:** Planning complete → first spike pending
**Owner:** Jay
**Last updated:** 2026-04-25

## What this epic delivers

A rebuilt frontend for `projects/t-dsp_tac5212_audio_shield_adaptor/tools/web_dev_surface/`
that:

- Is **touch-first**, designed for the Microsoft Surface (and future hardware
  panels) — minimum 44 px tap targets, large faders, gesture-driven.
- Has a **two-persona hierarchy** — Audio engineer (mixing) and Musician
  (synth performance) — with a Mode toggle that re-orders nav and swaps the
  persistent bottom strip.
- Has a **proper signal-flow hierarchy** of five workspaces — MIX / PLAY /
  TUNE / FX / SETUP — instead of the current flat ten-tab grab-bag.
- Has a **selected-channel "fat strip"** (TUNE workspace) so per-channel HPF /
  EQ / dynamics / sends finally have a place to live without 26 px columns.
- Has a **persistent mix overview** that stays visible from any workspace, so
  you can ride levels while editing a sound or tweaking FX (X32-Mix idiom).
- Adopts **Solid.js + Tailwind v4 on Vite** — preserving the existing
  fine-grained reactive `Signal<T>` model and Vite/Electron toolchain. No
  Next.js, no React VDOM diff at 30 Hz meter rate.
- Anticipates **sends-on-faders** even before aux buses exist on the firmware
  side, so the affordance is in place when buses come online.

This epic does **not** include: firmware-side per-channel EQ/dynamics
(those are a separate dependency tracked here as a risk), aux-bus signal
routing in the firmware, the Open Stage Control preset (M13), or the
downstream Tauri host-side GUI.

## How to read this epic

Documents are numbered in suggested reading order. Each is self-contained.

| # | Document | What it covers |
|---|---|---|
| 00 | [Overview](00-overview.md) | Vision, personas, success criteria, what's in / out of scope |
| 01 | [Current state](01-current-state.md) | Frontend audit, touch deficiencies, what code stays vs. is replaced |
| 02 | [Hierarchy](02-hierarchy.md) | Workspaces, mode toggle, Sel state, sends-on-faders, persistent bottom strip |
| 03 | [Design system](03-design-system.md) | Touch tokens, control specs, gesture vocabulary, Tailwind theme |
| 04 | [Stack](04-stack.md) | Vite + Solid.js + Tailwind decision; rejected alternatives; Signal-to-Solid bridge |
| 05 | [Roadmap](05-roadmap.md) | Phased migration, per-panel inventory, first spike, risks, firmware dependencies |

## The load-bearing principles

If you only remember four things from this epic:

1. **The wire protocol is sacred — keep `transport.ts`, `osc.ts`,
   `dispatcher.ts`, `state.ts`, `codec-panel-config.ts`,
   `adc6140-panel-config.ts`, `raf-batch.ts`.** The rebuild is a UI-layer
   rebuild, not a protocol rebuild. Every byte going to or from the firmware
   must come out the same as it does today. See [01-current-state.md](01-current-state.md).

2. **Two personas, one app, one Mode toggle.** Engineer and Musician share
   every workspace and every signal — the difference is the default landing
   tab and the persistent bottom strip. No feature gating, no separate apps.
   See [02-hierarchy.md](02-hierarchy.md).

3. **Sel is global state.** A single `selectedChannel` signal drives the
   highlighted strip in MIX, the bottom-strip indicator, and the channel
   shown in TUNE. Every workspace reads from the same Sel. See
   [02-hierarchy.md](02-hierarchy.md).

4. **Solid.js is chosen because the existing `Signal<T>` class IS Solid's
   `createSignal`.** Same get/set/subscribe shape, same fine-grained
   reactivity, no VDOM diff overhead at 30 Hz meter rate. The state.ts model
   migrates with a 5-line adapter, not a rewrite. React would force
   `useSyncExternalStore` and memo gymnastics on hundreds of signals. See
   [04-stack.md](04-stack.md).

## Execution status

- [x] Persona definitions agreed (Engineer + Musician)
- [x] Workspace hierarchy agreed (MIX / PLAY / TUNE / FX / SETUP)
- [x] PLAY inner-tab order agreed (Synths / Arp / Beats / Loop)
- [x] Mode toggle behavior specified
- [x] Sends-on-faders affordance scoped
- [x] Persistent bottom strip variants specified
- [x] Stack decision recorded (Vite + Solid + Tailwind v4)
- [x] State bridge approach designed (Signal<T> → Solid signal adapter)
- [x] Migration phases sequenced
- [x] Component inventory drafted
- [x] Spike 0 — Sel state + persistent bottom strip in current vanilla code (commit fdbe00e)
- [ ] **Spike 1 — workspace regroup (pure nav reorg, no rewrites)** ← next
- [ ] Spike 2 — Solid + Tailwind alongside vanilla, port one panel
- [ ] Spike 3 — TUNE workspace
- [ ] Spike 4 — sends-on-faders mode
- [ ] Spike 5 — gesture vocabulary
- [ ] Phase-by-phase panel migration
- [ ] Cutover: delete legacy panels

## Relationship to other epics

- **[osc-mixer-foundation](../osc-mixer-foundation/README.md)** — owns the
  protocol and firmware-side dispatcher. This epic consumes it. The
  Engineer-tier features (per-channel EQ/dyn, scenes, aux buses) are
  firmware features that this UI exposes, not invents.
- **[shared-clock](../shared-clock/README.md)** — Clock workspace is a
  consumer; the existing `clock-panel.ts` ports across with minimal changes.
- **[tac5212-hw-dsp](../tac5212-hw-dsp/PLAN.md)** — codec DSP panel ports
  into the SETUP workspace mostly unchanged; the descriptor-driven
  `codec-panel.ts` renderer is preserved.
- **Open Stage Control preset (M13, future)** — the engineer-facing OSC
  surface. Disjoint from this surface; both ship.
- **Future Tauri GUI (out of epic scope)** — could replace this Electron
  surface eventually, but not on this epic's timeline.

## Pickup-from-cold-start

If you (a future agent) are starting work on this epic with no conversation
context:

1. Read [00-overview.md](00-overview.md) and [01-current-state.md](01-current-state.md) first.
2. Read [02-hierarchy.md](02-hierarchy.md) to understand the workspace structure.
3. Read [05-roadmap.md](05-roadmap.md) and find the first uncompleted spike.
4. Honor the load-bearing principles above, especially #1 (wire protocol).
5. Before writing any new framework code, finish Spike 0 and Spike 1 — these
   are pure additive changes in the existing vanilla codebase that prove the
   navigation idea before we commit to Solid.
