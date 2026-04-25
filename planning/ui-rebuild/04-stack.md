# 04 — Stack

The technology choices and the rationale, including rejected options.

## Decision

```
Build tool:         Vite (kept)
Component layer:    Solid.js (added)
Styling:            Tailwind v4 (added)
State primitive:    existing Signal<T> + Solid signal adapter (kept + bridged)
Wire layer:         transport.ts, osc.ts, dispatcher.ts, raf-batch.ts (kept)
Host:               Electron (kept)
Bridge:             serial-bridge.mjs (kept)
```

No Next.js. No React. No CSS-in-JS. No component libraries (MUI, Mantine,
Radix, etc.). No state management library beyond what Signal already
provides.

## Why not Next.js

Next is a **server-rendered framework** — file-system routing, RSC,
streaming, edge functions. None of those are useful when shipping in
Electron talking to a local WebSocket bridge.

| Next.js feature | Useful here? |
|---|---|
| App Router (file-system routing) | No — single-window app, no URL persistence beyond the active workspace (and that's localStorage, not URL) |
| Server Components | No — there is no server. The "server" is the bridge process inside Electron's main process. |
| SSR / SSG | No — the page is loaded once, never refreshed |
| Streaming | No — it's a SPA |
| Image optimization | No — no images of substance |
| Edge runtime | No |

You'd be paying ~80 KB of framework + an entirely-irrelevant routing
mental model for *zero* benefit. Vite is the right base for an Electron
SPA, and Vite is already there. **Keep Vite.**

## Why not React

React is a fine framework but is the **worst fit** for this app's
performance profile.

The current code has hundreds of `Signal<T>` instances and the UI
subscribes to most of them. Examples:
- 10 channels × 8 signals = 80 channel signals
- MPE has 4 voices × 6 signals telemetry at 30 Hz
- Beats has 4 tracks × 16 steps × 2 signals (on/vel) = 128 step signals
- Meters fire at 30-50 Hz

In React, a "signal" usually maps to either:
- `useState` + a custom subscription hook + `useSyncExternalStore`, or
- a third-party signals library (Preact Signals, Zustand, Jotai, Valtio)
  with React adapters.

Either way, React **does VDOM diffing on render**. To avoid wasted
re-renders at 30 Hz × hundreds of components, every component needs
manual `memo`, `useCallback`, and `useMemo`. The current vanilla
direct-DOM-mutation pattern is much faster and the engineering cost
of preserving that performance in React is high.

React has plenty of UI advantages (ecosystem, hiring, docs) but those
don't matter for a one-developer project with a custom audio control
surface.

## Why Solid.js

Three reasons, in order of importance:

### 1. Solid's `createSignal` IS the existing `Signal<T>`

Compare:

```ts
// existing state.ts
class Signal<T> {
  get(): T;
  set(v: T): void;
  subscribe(l: Listener<T>): () => void;
}

// Solid.js
const [get, set] = createSignal<T>(initial);
// effects subscribe automatically when they read get()
```

Same conceptual model. The Solid version's "subscribe" is implicit —
any `createEffect` that reads the signal subscribes automatically — but
the underlying graph (signals → effects) is exactly the model the app
already uses.

A 5-line adapter wraps an existing `Signal<T>` as a Solid getter:

```ts
// signal-bridge.ts
import { createSignal, type Accessor } from 'solid-js';

export function asSolid<T>(s: Signal<T>): Accessor<T> {
  const [get, set] = createSignal<T>(s.get());
  s.subscribe(v => set(() => v));   // setter wrapper avoids function-type ambiguity
  return get;
}
```

This means **panels can be ported one at a time** without rewriting
`state.ts`. Each Solid component receives an existing `Signal<T>` from
state, calls `asSolid(signal)` once at the top, and uses the resulting
accessor in JSX.

### 2. Fine-grained reactivity = no VDOM overhead

Solid compiles JSX to direct DOM updates. There is no VDOM diff. When
a signal changes, only the DOM nodes that depend on it re-execute.
This is exactly what the current vanilla code does manually, but
declarative.

For 30 Hz meter updates: Solid will only run the effect that updates
the meter's `transform: scaleY()` property. No component-level
re-render, no diff, no reconciliation.

### 3. Vite-native, TSX-native

`vite-plugin-solid` is first-party. JSX transforms to direct DOM
operations at build time. TypeScript support is excellent. No
configuration drama.

## Why not Svelte

Svelte 5 with runes is also an excellent fit and was a serious
runner-up. Reasons Solid wins:

- **Solid stays in `.tsx`**; Svelte uses `.svelte` files with custom
  syntax. Porting the existing TS-heavy panels (especially the
  descriptor-driven codec panel and the dispatcher integration) is
  cheaper in TSX than in `.svelte`.
- **Solid's signal model is the Signal model already in `state.ts`**.
  Svelte's `$state` is similar but expressed differently.
- **Type inference** is slightly more transparent in Solid's TSX than
  in Svelte's compiled `.svelte` files.

If at any point Solid hits a wall (it shouldn't), Svelte 5 is the
fallback — the architectural plan in this epic doesn't depend on Solid
specifically; it depends on **fine-grained reactivity** as the
component-layer model.

## Why Tailwind v4

- **`@theme` block** lets the design tokens from
  [03-design-system.md](03-design-system.md) live in CSS, not in a JS
  config file. One place to look.
- **Utility classes** make panel layout markup readable and
  consistently sized. No more `class="row-fader"` magic strings.
- **Tree-shaking** strips unused utilities at build time.
- **No `tailwind.config.js`** in v4 — lighter config surface than v3.

Tailwind is **not** a component library. It does not provide buttons,
faders, or panels. It provides a way to express design tokens and
common layout patterns succinctly. We will still hand-build every
audio-specific control (faders, knobs, step pads).

## Why no component library (MUI, Mantine, Radix, shadcn)

- All are designed for **forms and dashboards**, not audio control
  surfaces.
- None ship faders, rotary knobs, step grids, or meters.
- Even for "non-audio" controls (buttons, selects, tabs), the design
  tokens we want are stricter than the libraries' defaults
  (touch-first, dark, monospace dB readouts).
- Adding a 200 KB component library to gain a `<Button>` and `<Tabs>`
  primitive is poor cost/benefit.

The components we'll need:
- `Button`, `IconButton` — trivial in Solid + Tailwind, < 30 LOC each.
- `Tabs` — same.
- `Select` — the existing `<select>` styled with Tailwind is fine for
  most cases; for the bus picker, a custom dropdown is worth the LOC.
- `Drawer` — a custom Solid component, < 100 LOC.
- `Modal` — same.

If a Headless library (Radix, Headless UI) had a Solid port that was
mature, we'd consider it for the accessibility primitives. The Solid
port of Headless (Kobalte) is reasonable but its API surface is
broader than we need. Skip for v1.

## State bridge — the critical piece

`signal-bridge.ts` is the load-bearing adapter:

```ts
import { Signal as TSignal } from './state';
import { createSignal as createSolidSignal,
         createMemo,
         type Accessor } from 'solid-js';

/**
 * Wrap an existing TSignal<T> as a Solid Accessor<T>.
 * Any Solid component or effect that calls the returned getter
 * subscribes to the underlying TSignal automatically.
 */
export function asSolid<T>(s: TSignal<T>): Accessor<T> {
  const [get, set] = createSolidSignal<T>(s.get());
  s.subscribe(v => {
    // Solid's set wraps a function in a value, so wrap the value
    // in a function to avoid that ambiguity for callable T.
    set(() => v);
  });
  return get;
}

/**
 * Bidirectional helper for editable signals: returns a [get, set]
 * pair where set() writes to the underlying TSignal (which also
 * propagates back to all of its subscribers, including this Solid
 * accessor — but TSignal.set() is idempotent on Object.is, so no
 * loop occurs).
 */
export function asSolidRW<T>(s: TSignal<T>): [Accessor<T>, (v: T) => void] {
  return [asSolid(s), (v: T) => s.set(v)];
}
```

Cleanup is **not** needed for the lifetime-of-the-app signals (most of
state.ts), since panels mount once and live until the app closes.
For panels that mount/unmount (sends-on-faders mode swap, MPE voice
slot fluctuation), wrap the subscribe call in `onCleanup` so the
TSignal listener Set doesn't leak.

## Build / dev workflow

### `pnpm app:dev`
Unchanged. Vite dev server + Electron + bridge. Hot reload on save.

### `pnpm build`
Unchanged. Vite production build into `dist/`.

### `pnpm app:build`
Unchanged. `electron-builder` bundles `dist/` + bridge into
`release/win-unpacked/`.

### Migration overlap

During migration, the new Solid + Tailwind code lives in **the same
`src/`** as the existing vanilla code. No `src-new/` or parallel tree.
Reasons:
- Vite handles `.tsx` and `.ts` in the same bundle natively.
- Each panel is independent; no cross-imports between old and new.
- `state.ts`, `dispatcher.ts`, etc. are imported by both old and new
  panels; duplicating them would split state.

The new shell (`main.tsx`) replaces `main.ts` once it can render at
least one workspace. Until that point, `main.ts` wires both the new
shell *as a placeholder workspace* and the existing flat tabs, with a
URL flag (`?new=1`) to toggle.

Once the new shell renders all five workspaces, `main.ts` is deleted.

## Dependencies to add

```jsonc
{
  "devDependencies": {
    "vite-plugin-solid": "^2.x",
    "@tailwindcss/vite": "^4.x"
  },
  "dependencies": {
    "solid-js": "^1.8.x"
  }
}
```

That's it. No router (the app is single-window with workspace state in
a Signal), no UI library, no animation library (CSS transitions
suffice), no state library.
