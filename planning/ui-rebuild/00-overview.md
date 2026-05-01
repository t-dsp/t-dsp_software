# 00 — Overview

## What we are building

A **touch-first rebuild of the web dev surface** that replaces a flat
ten-tab navigation with a five-workspace hierarchy organized by signal flow,
adds a persistent fader/keyboard strip that is visible from every workspace,
and adopts a component framework (Solid.js) that preserves the existing
signal-based reactive model while making panel layout faster to iterate.

The current frontend at
`projects/t-dsp_tac5212_audio_shield_adaptor/tools/web_dev_surface/` calls
itself a "deliberately undercooked bench tool" with "no framework
dependencies" — but it is now ~9,500 LOC across 28 panels (mixer, codec,
arp, beats, neuro, supersaw, mpe, acid, dexed, looper, keyboard, fx,
processing, clock, codec, spectrum, raw OSC, serial console). It has
outgrown its README and is the actual day-to-day UI of a Teensy-based
synth/looper/mixer.

The touch problem is **structural**, not stylistic:

- `.cell { width: 26px }` — 26 px columns
- `.fader` ~24 px wide × 160 px tall (native `<input type="range">`)
- `.mute / .solo / .link-btn { height: 24px; font-size: 11px }`
- `.strip-name { font-size: 9px }` — 9 px labels
- `#app { max-width: 1200px }` — caps width even on bigger screens

Apple HIG, Microsoft, and Material Design all converge on **40-48 px
minimum tap targets**. The current UI is at 24 px with 9 px labels.
Re-skinning won't fix that. Tokenized control sizing, larger native or
custom controls, and a layout that adapts to screen size will.

## Why this matters

The web surface is the **primary control interface** for two distinct
users:

1. **Audio engineer** — mixing the band. Faders, EQ, dynamics, sends,
   per-channel processing. Same person the X32 and the planned Open Stage
   Control preset (M13) are aimed at.
2. **Musician** — playing the synths. Synth engine picker, on-screen
   keyboard, Arp, Beats, Looper. Doesn't typically touch per-channel
   processing.

These workflows live in different parts of the app, and the current flat
tab structure forces both personas to share the same cluttered top bar
with no semantic ordering. A tab strip with ten roughly-equally-prominent
buttons trains the user to remember positions, not affordances.

## What success looks like

**End of this epic, the following are true:**

1. Top-level navigation has **five workspaces** — MIX / PLAY / TUNE / FX /
   SETUP — each with a clear semantic role and inner sub-navigation where
   needed.
2. A **Mode toggle** in the header switches between Engineer-default and
   Musician-default behavior. Mode determines: default landing workspace,
   tab order, and the persistent bottom strip variant. No feature gating.
3. A **persistent bottom strip** is visible from every workspace. In
   Engineer mode it is an 8-fader mini-bank with Sel buttons and the main
   meter/fader; in Musician mode it is the on-screen keyboard with octave
   controls, the active synth's quick controls, and the main fader.
4. A **global `selectedChannel` signal** drives a Sel highlight in MIX,
   the Sel indicator in the bottom strip, and the channel detail rendered
   in TUNE.
5. **Touch tokens** are defined and enforced: 44 px minimum tap targets,
   56 px primary controls, 16 px minimum body text, fader thumbs ≥ 60 px.
6. **Sends-on-faders mode** exists as a UI affordance even before the
   firmware exposes aux buses — toggle in MIX flips the fader bank to
   show send levels to a chosen bus.
7. The frontend is built with **Vite + Solid.js + Tailwind v4** — Vite
   stays as the bundler (Electron-friendly), Solid replaces vanilla DOM
   building, Tailwind carries the design tokens. The existing wire layer
   (`transport.ts`, `osc.ts`, `dispatcher.ts`, `state.ts`,
   `codec-panel-config.ts`, `raf-batch.ts`) is **unchanged**.
8. The existing `Signal<T>` class is **bridged** to Solid signals via a
   small adapter so panels can be ported one at a time without rewriting
   `state.ts` or the dispatcher.
9. Performance budgets are met: meter updates at ≥ 30 Hz with no dropped
   frames, fader feel sub-16 ms (60 fps) on the Surface, no main-thread
   stalls during heavy OSC traffic (e.g. 4-voice MPE telemetry + 16-step
   beats grid + 30 Hz meters concurrently).

## Personas

### Engineer
- **Home workspace:** MIX
- **Most-used surfaces:** MIX (faders, sends-on-faders, Sel) → TUNE
  (per-channel processing) → FX (bus FX, processing, spectrum)
- **Rarely-used surfaces:** SETUP (codec, system, raw OSC), PLAY (only to
  mute a runaway synth)
- **Persistent bottom strip:** 8-fader mini-bank with Sel + main meter
- **Mental model:** X32-Mix / dLive Director / Mixing Station

### Musician
- **Home workspace:** PLAY
- **Most-used surfaces:** PLAY (synth picker, Arp, Beats, Loop) → MIX
  (level overview)
- **Rarely-used surfaces:** TUNE, FX, SETUP
- **Persistent bottom strip:** keyboard with octave control + active-synth
  quick controls + main fader
- **Mental model:** soft-synth host (Bitwig touch / Maschine / iPad
  GarageBand) crossed with a hardware groovebox

## Non-goals (explicit out-of-scope)

- **Firmware-side per-channel EQ / dynamics.** TUNE workspace will exist
  but will be partly empty until the firmware exposes those controls.
  Tracked as a risk in [05-roadmap.md](05-roadmap.md).
- **Aux-bus signal routing in firmware.** Sends-on-faders UI ships with
  whatever buses currently exist (main, synth-bus, FX return). Adding aux
  buses is a firmware epic.
- **Scene library content.** The header has a Scene picker, but it
  consumes whatever the firmware's `/-snap/save` and `/-snap/load` already
  expose. No curated scene library in this epic.
- **Open Stage Control preset.** That is M13 in the OSC mixer foundation
  epic. Disjoint surface, both ship.
- **Tauri replacement of Electron.** Out of scope. The Vite output stays
  Electron-hosted; switching the host is a future epic.
- **Mobile / phone breakpoints.** Target is the Surface (~1500-2000 px
  width touchscreens) and a possible future panel-mount screen. Phone
  layouts not addressed.
- **Accessibility audit.** Touch targets help by default, but full a11y
  (screen reader semantics, keyboard nav parity, contrast certification)
  is not in this epic's scope.
- **i18n.** English only.

## Why now

Three converging reasons:

1. **The UI has outgrown its framing.** What started as a bench tool is
   now ~9.5k LOC and the primary user surface. Refactoring the navigation
   while panel count is "only" 28 is cheaper than at 50.
2. **Sel and per-channel processing are blocked on hierarchy.** You can't
   add HPF/EQ/dyn widgets to a 26 px column. They need a destination
   (TUNE), and TUNE needs Sel state, and Sel state needs a hierarchy that
   gives it a home. Building TUNE without first regrouping is wasted work.
3. **The Surface is sitting on the desk.** Day-to-day use on a touchscreen
   is happening today, and it's bad. Every week of delay is a week of
   continued mouse-precision use of a touch device.

## Framing questions deferred to later docs

- What stays vs. what changes in the codebase → [01-current-state.md](01-current-state.md)
- Workspace contents and inner navigation → [02-hierarchy.md](02-hierarchy.md)
- Touch tokens, control specs, gestures → [03-design-system.md](03-design-system.md)
- Stack choice rationale and state bridge → [04-stack.md](04-stack.md)
- Phased migration sequence and component inventory → [05-roadmap.md](05-roadmap.md)
