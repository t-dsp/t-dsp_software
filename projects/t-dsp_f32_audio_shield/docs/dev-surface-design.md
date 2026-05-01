# Dev Surface — Architecture Redesign

> Author: design proposal, no code changes.
> Audience: firmware dev who built the v1 dev surface and the upcoming X32-style OSC tree.
> Scope: the host-side mixer UI that talks to the Teensy 4.1 firmware over SLIP-OSC on USB CDC.

The current dev surface (`projects/t-dsp_web_dev/`, was `projects/t-dsp_tac5212_audio_shield_adaptor/tools/web_dev_surface/` before being promoted to a top-level project) works but has known scaling problems:

- Hard-coded channel count, hard-coded panel set, hard-coded address strings everywhere in `dispatcher.ts`.
- Volume sliders feel sluggish — the per-address 33 ms throttle is right in concept, but the rendering path round-trips through the synchronous `Signal.subscribe` callback, which writes the DOM inline; on a 30 Hz meter blob fan-out this fights the main thread.
- `dispatcher.ts` is 1791 lines — every leaf appears at least twice (one outbound setter, one inbound `if`). Adding an XLR or a synth means edits in 4-6 files.

The new dev surface inherits the working bits (Electron shell, SLIP framing, Solid bridge, RAF batching, throttled bridge) and replaces the parts that don't scale (hand-rolled per-leaf wiring, hard-coded topology).

---

## 1. Goals + Non-Goals

### Goals (v1)

- **Topology-driven UI.** The firmware's `/snapshot` reply tells the surface which channels exist, which subtrees are bound, what kinds of controls each subtree exposes. The UI renders from that descriptor; nothing in the host code says "there are 10 channels" or "there's a Dexed panel."
- **Modular per-component.** A component is a function of `{ addressPrefix, leafDescriptors, role }`. A channel strip works the same way as a synth panel as a codec section as a master strip.
- **Real-time fader response.** Pointer drag updates the visual fader within the current animation frame. The OSC write is decoupled from the visual update; the firmware echo doesn't block local responsiveness.
- **Signal-driven reactivity.** Every leaf is a Signal. UI controls subscribe. Firmware echoes update Signals. UI re-renders only the changed control. Solid is the canonical reactive engine; the existing custom `Signal<T>` class continues to exist as a thin shim for ergonomics where Solid is awkward (panel descriptors, the dispatcher's incoming routing).
- **Stereo-link aware.** A pair shares a single fader visual when linked, with a touch-target affordance to break the link. Writes optimistically mirror locally; both echoes (the one we sent + the firmware's mirror) are absorbed idempotently.
- **X32-tool flavored.** Familiar layout: horizontal channel strips with name/color scribble, fader, mute, solo, optional Sel+Tap; master section docked right; tab/rack metaphor for FX, codec, synths.
- **Bridge stays decoupled.** `serial-bridge.mjs` keeps its current shape — agnostic to address tree, just shovels SLIP-framed bytes between WebSocket and COM port.

### Non-goals (v1)

- **Production mixing surface.** This is still a bench tool plus the rebuild. The Open Stage Control preset (M13) is the engineer-facing product; this complements it.
- **Mobile / tablet UX.** Touch-friendly is a goal of the parallel UI rebuild epic (see `planning/ui-rebuild/`); this design does not duplicate that effort. Where the two intersect, this doc references that plan rather than re-specifying it.
- **Scene management, undo/redo, snapshot diff.** v1 reads `/snapshot` on connect and tracks live state. Scenes can be triggered through the raw-OSC pane.
- **Authoring tool for new firmware addresses.** The firmware drives the topology; the host renders it.
- **Cross-platform serial port abstraction.** Windows-only via Electron, same as today. The Node-side `serialport` package handles macOS/Linux but the build hasn't been validated.
- **Standalone web build (no bridge).** WebSerial direct-from-browser was tried and abandoned (USB Audio buzz on composite device). The bridge stays.

---

## 2. Tech-Stack Recommendation

### TL;DR

| Layer | Keep | Replace | Reason |
|---|---|---|---|
| Shell | Electron 33 | — | Electron stays — the WebSocket-bridge story works. Windows packaging via `target: "dir"` is solved. |
| Bundler | Vite 5 + TypeScript 5 | — | Already tuned, dev-server hot-reload works. |
| Reactivity | `Signal<T>` (state.ts) | Promote Solid to primary; keep `Signal<T>` as compat shim | Solid signals are functionally a superset; Solid's automatic dependency tracking removes a class of subscribe-leak bugs. |
| Rendering | Hand-written DOM | Solid `JSX` (already the direction — `vite-plugin-solid` is in `package.json`, `tune-stub.tsx` is the first one) | DOM-by-hand is fine for one panel, breaks down at six panels with shared sub-components. Solid keeps the surgical-DOM-update perf. |
| Styling | Tailwind v4 + legacy `style.css` | Tailwind primary, `style.css` shrinks over time | Tailwind v4 is already in. Per-component variants via class composition match the topology-driven render model. |
| Transport | WebSocket → bridge → SerialPort | — | Settled and works; don't touch. |
| OSC encode/decode | hand-rolled (`osc.ts`, ~300 lines) | — | Tiny, correct, no deps. Adding `/node` reply parsing is a small extension. |

### Why Solid (not Preact, MobX, Redux, Lit)

- **No virtual DOM.** Solid compiles JSX to direct DOM updates. Fader drags become `style.height = '70%'` with no diff/reconcile cycle. This is the perf-critical property.
- **Fine-grained reactivity.** `<input value={fader()} />` only re-runs the inner expression on change. A meter blob update doesn't repaint the label or the mute button next to it.
- **Already in the codebase.** `solid-js@1.9.12`, `vite-plugin-solid` already installed; `tune-stub.tsx` is mounted; `signal-bridge.ts` already exists. Promoting Solid is a continuation, not a pivot.
- **Compatibility with `Signal<T>`.** `signal-bridge.ts`'s `asSolid()` and `asSolidRW()` already adapt the existing class. The dispatcher's `state.channels[idx].fader.set(v)` path keeps working unchanged; Solid components consume those signals as Solid Accessors.

### Why not the alternatives

- **Preact signals + Preact.** Same model as Solid but with VDOM diffing on top. The compiled output is fatter and the per-frame work is higher when many signals change at once (meter blobs).
- **Lit / lit-html.** Web Components are a fine packaging story but the rendering path is template-string + tagged literals, no fine-grained signal subscription. We'd have to layer reactivity ourselves and end up rebuilding what Solid already does.
- **MobX.** Excellent observable model but ships ~50 KB of runtime and pulls in proxy semantics that interact poorly with the binary OSC blob path (we don't want every `Uint8Array` mutation to fire mobx tracking).
- **Redux Toolkit.** Centralized store model — wrong shape. Our state is intrinsically distributed (per-leaf signals indexed by OSC address). RTK adds an action/reducer indirection that buys nothing for a surface that's already 1:1 with firmware addresses.
- **React 18 + React.useSyncExternalStore.** Could work; bigger runtime, slower fader response under heavy meter load, and we already paid the activation cost for Solid.

### Custom `Signal<T>` keeps a role

The existing `Signal<T>` class survives, demoted to:

- The **topology cache** — `Signal<TopologyTree>` for the descriptor returned by `/snapshot` / `/node`. Solid's reactive primitives don't work well outside a reactive root; the topology is consumed both inside JSX (via `asSolid`) and outside it (the dispatcher reading "what address belongs to this leaf?").
- The **dispatcher's incoming router** — `handleIncoming` writes to `Signal<T>`s indexed by address, since it runs outside any Solid root. The `signal-bridge` adapts those into Solid Accessors at component boundaries.

This keeps the network-layer code simple (no Solid imports in `dispatcher.ts`) and the UI layer expressive (Solid JSX everywhere).

---

## 3. Topology-Driven Rendering

The defining shift: the dev surface stops knowing about channels, panels, or tabs at compile time. It learns the mixer shape from a topology document the firmware emits.

### 3.1 The topology document

A new firmware subtree, `/-info/topology`, returns the packed tree on `/node` query. Schema (host-side TS view):

```ts
interface Topology {
  version:   number;            // bump on incompatible changes
  generated: number;            // unix-ms, for cache invalidation
  nodes:     TopologyNode[];    // flat list, parent links by index
}

type TopologyNode =
  | StripNode
  | PanelNode
  | LeafNode
  | GroupNode;

interface StripNode {
  kind:        'strip';
  prefix:      string;          // e.g. "/ch/01"
  role:        'channel' | 'aux' | 'fx_return' | 'main' | 'main_l' | 'main_r' | 'sub' | 'mc';
  display:     {
    nameAddr?:    string;       // /ch/01/config/name  (if name leaf exists)
    colorAddr?:   string;       // /ch/01/config/color
    iconAddr?:    string;       // /ch/01/config/icon
    sourceLabel?: string;       // human-readable source ("USB L", "XLR 1")
  };
  capabilities: StripCapability[]; // what's wired on this strip
  link?: {                      // present only on stereo-paired strips
    partnerPrefix: string;      // e.g. "/ch/02"
    linkAddr:      string;      // canonical link address ("/config/chlink/1-2")
    isOdd:         boolean;     // true for the L side of the pair
  };
}

type StripCapability =
  | { kind: 'fader';      addr: string; law: 'x32' | 'linear'; }
  | { kind: 'mute';       addr: string; idiom: 'x32_inverted' | 'mute'; }
  | { kind: 'solo';       addr: string; }
  | { kind: 'pan';        addr: string; }
  | { kind: 'meter';      kind2: 'peak_rms'; blobAddr: string; blobOffset: number; }
  | { kind: 'gain';       addr: string; min: number; max: number; step: number; unit: string; }
  | { kind: 'rec_send';   addr: string; }
  | { kind: 'subgroup';   subtreePrefix: string; }; // e.g. eq, dyn, sends

interface PanelNode {
  kind:    'panel';
  prefix:  string;              // e.g. "/codec/tac5212"
  role:    'codec' | 'synth' | 'fx' | 'looper' | 'beats' | 'arp' | 'clock' | 'processing';
  title:   string;
  // For codec/synth, panels have sub-sections defined declaratively:
  sections?: PanelSection[];
}

interface PanelSection {
  name: string;
  controls: ControlDescriptor[];
}

type ControlDescriptor =
  | { kind: 'enum';   addr: string; label: string; options: string[]; }
  | { kind: 'toggle'; addr: string; label: string; }
  | { kind: 'range';  addr: string; label: string; min: number; max: number; step: number; unit: string; }
  | { kind: 'biquad'; addrBase: string; label: string; }
  | { kind: 'action'; addr: string; label: string; arg?: string | number; };

interface LeafNode {
  kind:  'leaf';
  addr:  string;
  type:  'i' | 'f' | 's' | 'b';
  meta?: Record<string, string | number>; // free-form (rate hints, etc.)
}

interface GroupNode {
  kind:     'group';
  id:       'mute_group_1' | 'dca_1' | string;
  members:  string[];           // strip prefixes that belong
  nameAddr: string;
}
```

### 3.2 Why this shape

- **Strips and panels are first-class.** The host doesn't have to discover that `/ch/01/mix/fader` exists — the topology says "there's a strip at `/ch/01` with a fader capability at `/ch/01/mix/fader`."
- **Capabilities are flat lists.** A future strip with HPF + EQ + dynamics + sends just adds capability entries. The renderer iterates capabilities; no `if (channel.hasEq)` branches anywhere.
- **Stereo-link is structural.** The `link` field is on the strip itself. The renderer pairs strips at layout time without parsing the prefix string.
- **Codec/synth panels are still descriptor-driven** but the descriptor lives on the firmware side (echoed via `/node ,s "/codec/tac5212"`) instead of being a hand-edited TypeScript file like `codec-panel-config.ts`. v1 can ship with the existing TS descriptor as a fallback for codec panels — it's the channel side that benefits most from server-driven topology.

### 3.3 Data flow on connect

```
  ┌──────────┐    open WS     ┌──────────┐
  │  client  │───────────────▶│  bridge  │── COM ──▶ Teensy
  └──────────┘                └──────────┘
       │
       │  /xremote                                                 (subscribe to all change broadcasts)
       │  /-info/topology  via  /node ,s "/-info/topology"          (fetch topology)
       │◀─────────────────────────────────────────  bundle reply
       │
       │  buildComponentTree(topology)                              (host-side, no I/O)
       │
       │  /snapshot                                                 (fetch initial values)
       │◀─────────────────────────────────────────  bundle reply    (every leaf echoes once)
       │
       │  bind addresses: each leaf write → subscribe a Signal      (host-side)
       │  bind controls:  each control reads its leaf Signal        (host-side)
       │
       │  for each open tab/panel: /subscribe ,si <pattern> 80      (per-tab streams)
       │
       │  every ~7 s: /xremote                                      (renew TTL)
       │
       └──── steady state: echoes update signals; UI repaints diff
```

### 3.4 What's hard-coded vs. server-driven

| Thing | Source |
|---|---|
| Channel count | topology |
| Channel names + colors | topology + /snapshot |
| Stereo-link pairing | topology |
| Synth list (Dexed, MPE, Neuro, Acid, Supersaw, Chip) | topology |
| Synth panel internals | topology (long-term); local `synth-panel-config.ts` (v1 fallback) |
| Codec panel internals | local `codec-panel-config.ts` (v1; topology in v2) |
| Workspace tabs (MIX/PLAY/TUNE/FX/SETUP) | host (UI rebuild epic) |
| Tab→panel mapping | host (declarative — see §9) |
| Persona toggle | host |
| Fader law | host (X32 piecewise — see §5.4) |

The "v2" boxes are a forward path; v1 ships with the topology covering strips + capabilities + links + groups, and codec/synth panel descriptors staying local for one more iteration. That's the highest-value slice.

---

## 4. Component Contract

### 4.1 The Strip — generic, role-parameterized

```ts
// Module: components/Strip.tsx

import type { StripNode, StripCapability } from '../topology';

export interface StripProps {
  node:       StripNode;
  store:      AddressStore;     // see §5
  dispatcher: Dispatcher;       // outbound writes
}

export function Strip(props: StripProps) {
  const { node, store, dispatcher } = props;

  return (
    <div class={`strip strip-${node.role}`}>
      <ScribbleStrip
        nameAddr={node.display.nameAddr}
        colorAddr={node.display.colorAddr}
        sourceLabel={node.display.sourceLabel}
        store={store}
        dispatcher={dispatcher}
      />
      <For each={node.capabilities}>
        {(cap) => <Capability cap={cap} store={store} dispatcher={dispatcher} />}
      </For>
      {node.link && (
        <LinkButton
          linkAddr={node.link.linkAddr}
          isOdd={node.link.isOdd}
          store={store}
          dispatcher={dispatcher}
        />
      )}
    </div>
  );
}

function Capability(p: { cap: StripCapability; store: AddressStore; dispatcher: Dispatcher }) {
  switch (p.cap.kind) {
    case 'fader':    return <Fader   addr={p.cap.addr}     law={p.cap.law} store={p.store} dispatcher={p.dispatcher} />;
    case 'mute':     return <Mute    addr={p.cap.addr}     idiom={p.cap.idiom} store={p.store} dispatcher={p.dispatcher} />;
    case 'solo':     return <Solo    addr={p.cap.addr}     store={p.store} dispatcher={p.dispatcher} />;
    case 'pan':      return <Pan     addr={p.cap.addr}     store={p.store} dispatcher={p.dispatcher} />;
    case 'meter':    return <Meter   blobAddr={p.cap.blobAddr} blobOffset={p.cap.blobOffset} store={p.store} />;
    case 'gain':     return <Knob    addr={p.cap.addr}     min={p.cap.min} max={p.cap.max} step={p.cap.step} unit={p.cap.unit} store={p.store} dispatcher={p.dispatcher} />;
    case 'rec_send': return <RecSend addr={p.cap.addr}     store={p.store} dispatcher={p.dispatcher} />;
    case 'subgroup': return <Subgroup prefix={p.cap.subtreePrefix} store={p.store} dispatcher={p.dispatcher} />;
  }
}
```

### 4.2 Why this works

- **No address arithmetic in components.** A `<Fader>` doesn't know whether it's at `/ch/01/mix/fader` or `/main/st/mix/faderL`. It receives `addr` and uses it.
- **Capabilities are extensible.** Adding HPF: declare a new capability variant, render a new sub-component, the topology gets a new entry per strip that has it. No global edit.
- **Linking is local to the strip.** The Strip's `LinkButton` writes to the link address; the firmware mirrors the partner; both echoes update both signals; neighboring strips repaint independently.
- **Subgroups recurse.** Per-channel EQ/dynamics/sends become sub-components that take a sub-prefix. Same contract.

### 4.3 The Panel — for non-strip surfaces

Codec, synth, FX, looper, beats, etc. don't fit a strip mold. They share a sectioned-form pattern:

```ts
// Module: components/Panel.tsx

export interface PanelProps {
  node:       PanelNode;
  store:      AddressStore;
  dispatcher: Dispatcher;
}

export function Panel(props: PanelProps) {
  return (
    <section class={`panel panel-${props.node.role}`}>
      <h2>{props.node.title}</h2>
      <For each={props.node.sections ?? []}>
        {(section) => <PanelSection section={section} store={props.store} dispatcher={props.dispatcher} />}
      </For>
    </section>
  );
}

function PanelSection(p: { section: PanelSection; store: AddressStore; dispatcher: Dispatcher }) {
  return (
    <div class="panel-section">
      <h3>{p.section.name}</h3>
      <For each={p.section.controls}>
        {(ctl) => <Control ctl={ctl} store={p.store} dispatcher={p.dispatcher} />}
      </For>
    </div>
  );
}

function Control(p: { ctl: ControlDescriptor; store: AddressStore; dispatcher: Dispatcher }) {
  switch (p.ctl.kind) {
    case 'enum':   return <EnumSelect  ctl={p.ctl} store={p.store} dispatcher={p.dispatcher} />;
    case 'toggle': return <Toggle      ctl={p.ctl} store={p.store} dispatcher={p.dispatcher} />;
    case 'range':  return <RangeSlider ctl={p.ctl} store={p.store} dispatcher={p.dispatcher} />;
    case 'biquad': return <BiquadEdit  ctl={p.ctl} store={p.store} dispatcher={p.dispatcher} />;
    case 'action': return <ActionBtn   ctl={p.ctl} store={p.store} dispatcher={p.dispatcher} />;
  }
}
```

### 4.4 Atomic controls — the address binding pattern

Every control follows the same skeleton:

```ts
export function Fader(p: {
  addr:       string;
  law:        'x32' | 'linear';
  store:      AddressStore;
  dispatcher: Dispatcher;
}) {
  // 1. Read the current value (reactive).
  const value = p.store.getFloat(p.addr);

  // 2. Local optimistic state during a drag — diverges from
  //    `value()` between pointerdown and pointerup, then echoes
  //    reconcile.
  const [dragValue, setDragValue] = createSignal<number | null>(null);
  const displayed = () => dragValue() ?? value();

  // 3. Pointer handlers.
  const onMove = (frac: number) => {
    setDragValue(frac);
    p.dispatcher.writeThrottled(p.addr, 'f', frac);
  };
  const onRelease = (frac: number) => {
    setDragValue(null);                  // hand back to value()
    p.dispatcher.writeFlush(p.addr, 'f', frac); // ensure final
  };

  return <FaderCanvas value={displayed} law={p.law} onMove={onMove} onRelease={onRelease} />;
}
```

The contract: every control gets `(addr, store, dispatcher)`. `store` is reactive read; `dispatcher` is throttled+optimistic write. The control owns its render; the engine owns its data.

### 4.5 Capability matrix (v1)

| Capability  | Wire              | Display | Notes |
|---|---|---|---|
| `fader`     | `f` 0..1          | Vertical fader, dB readout | X32 piecewise dB displayed locally |
| `mute`      | `i` 0/1           | Square button             | `idiom: 'x32_inverted'` flips the bit on display only |
| `solo`      | `i` 0/1           | Yellow button              | |
| `pan`       | `f` 0..1          | Horizontal slider (0.5 center) | Stereo-linked = both center, dual mono = independent |
| `meter`     | blob              | LED ladder                 | Blob offset addresses peak/rms pair within shared blob |
| `gain`      | `f` (varies)      | Rotary knob                | `min/max/step/unit` from topology |
| `rec_send`  | `i` 0/1           | "REC" tab button           | Disabled when main loop on |
| `subgroup`  | (nested controls) | Section/popover            | Recursive renderer |

---

## 5. State Model

The state model is **one Signal per OSC address**, looked up by address string. This replaces the hand-typed `MixerState` interface in `state.ts` for everything topology-driven; the local UI-only signals (workspace tab, persona, selectedChannel) stay outside the address store.

### 5.1 The AddressStore

```ts
// Module: state/address-store.ts

import { Signal } from './signal';   // existing class

export type LeafType = 'i' | 'f' | 's' | 'b';

export class AddressStore {
  private leaves = new Map<string, Signal<unknown>>();
  private types  = new Map<string, LeafType>();

  /** Register a leaf with its declared type. Called when topology is built. */
  declare<T>(addr: string, type: LeafType, initial: T): Signal<T> {
    let sig = this.leaves.get(addr) as Signal<T> | undefined;
    if (!sig) {
      sig = new Signal<T>(initial);
      this.leaves.set(addr, sig as Signal<unknown>);
      this.types.set(addr, type);
    }
    return sig;
  }

  /** Type-narrowed reactive accessors. */
  getInt   (addr: string): Accessor<number>  { return asSolid(this.leafOrThrow<number> (addr)); }
  getFloat (addr: string): Accessor<number>  { return asSolid(this.leafOrThrow<number> (addr)); }
  getStr   (addr: string): Accessor<string>  { return asSolid(this.leafOrThrow<string> (addr)); }
  getBlob  (addr: string): Accessor<Uint8Array> { return asSolid(this.leafOrThrow<Uint8Array>(addr)); }

  /** Raw leaf access — used by dispatcher.handleIncoming() to .set() on echo. */
  leaf<T>(addr: string): Signal<T> | undefined {
    return this.leaves.get(addr) as Signal<T> | undefined;
  }

  /** Bulk-declare from a topology — every node's address gets a Signal. */
  ingestTopology(topology: Topology): void {
    for (const node of topology.nodes) {
      switch (node.kind) {
        case 'strip':
          if (node.display.nameAddr)  this.declare(node.display.nameAddr,  's', '');
          if (node.display.colorAddr) this.declare(node.display.colorAddr, 'i',  0);
          for (const cap of node.capabilities) declareCapability(this, cap);
          break;
        case 'panel':
          if (node.sections) for (const s of node.sections)
            for (const c of s.controls) declareControl(this, c);
          break;
        case 'leaf':
          this.declare(node.addr, node.type, defaultForType(node.type));
          break;
      }
    }
  }

  // (helpers omitted for brevity)
  private leafOrThrow<T>(addr: string): Signal<T> {
    const s = this.leaves.get(addr);
    if (!s) throw new Error(`AddressStore: unknown leaf ${addr}`);
    return s as Signal<T>;
  }
}
```

### 5.2 Read path

```
firmware echo  ─▶  decode OSC  ─▶  dispatcher.handleIncoming(msg)
                                       │
                                       ▼
                               store.leaf(msg.address)?.set(msg.args[0])
                                       │
                                       ▼ (Signal fires)
                               every Solid Accessor wrapper notifies
                                       │
                                       ▼
                               Solid effects re-run; DOM patches;
                               rafBatch coalesces same-element writes
```

### 5.3 Write path — optimistic + reconcile

```ts
// Module: net/dispatcher.ts (sketch)

export class Dispatcher {
  constructor(private store: AddressStore, private send: (b: Uint8Array) => void) {}

  /** Optimistic local write + immediate send. For toggles, enums, buttons. */
  writeNow(addr: string, type: LeafType, arg: OscArg): void {
    this.store.leaf(addr)?.set(arg);
    this.send(encodeMessage(addr, type, [arg]));
  }

  /** Optimistic local write + throttled send. For drags / continuous controls. */
  writeThrottled(addr: string, type: LeafType, arg: OscArg): void {
    this.store.leaf(addr)?.set(arg);                 // local UI updates immediately
    this.throttle(addr, type, arg);                  // network coalesces
  }

  /** Force-fire whatever's pending for this address. Called on pointerup. */
  writeFlush(addr: string, type: LeafType, arg: OscArg): void {
    this.store.leaf(addr)?.set(arg);
    this.flushThrottle(addr);                        // if pending, send now
    this.send(encodeMessage(addr, type, [arg]));
  }

  // ... throttle bookkeeping mirrors today's dispatcher.ts
}
```

The key change from today: the optimistic local `set()` is unconditional and synchronous. When the firmware echoes back the same value, `Signal.set()`'s `Object.is` short-circuit prevents a second notify (already true today). When the echo is a *different* value (firmware clamped, rejected, or mirrored to the link partner), the Signal updates and the UI re-renders to the firmware's truth.

### 5.4 Fader law — host-side

Per the design brief, faders are 0..1 on the wire and **always** displayed in dB locally. The X32 4-segment piecewise law:

```ts
// Module: util/x32-fader.ts

// Maps 0..1 fader position to dB. From X32 OSC reference.
//   0.00 .. 0.0625  →  -∞ .. -60 dB  (bottom segment, very steep)
//   0.0625 .. 0.25  →  -60  .. -30 dB
//   0.25   .. 0.50  →  -30  .. -10 dB
//   0.50   .. 1.00  →  -10  .. +10 dB
export function faderToDb(f: number): number {
  if (f <= 0)     return -Infinity;
  if (f < 0.0625) return -90 + (f / 0.0625) * 30;     // -90..-60
  if (f < 0.25)   return -60 + ((f - 0.0625) / 0.1875) * 30;
  if (f < 0.50)   return -30 + ((f - 0.25)   / 0.25)   * 20;
                  return -10 + ((f - 0.50)   / 0.50)   * 20;
}

export function dbToFader(db: number): number {
  if (db <= -90) return 0;
  if (db < -60)  return ((db + 90) / 30) * 0.0625;
  if (db < -30)  return 0.0625 + ((db + 60) / 30) * 0.1875;
  if (db < -10)  return 0.25   + ((db + 30) / 20) * 0.25;
                 return 0.50   + ((db + 10) / 20) * 0.50;
}
```

Wire reads/writes always use the 0..1 form; the `<Fader>` component runs `faderToDb(value())` for the readout. Replaces today's single-segment log approximation in `ui/util.ts`.

### 5.5 What's NOT in the AddressStore

UI-only state that never goes to firmware stays as plain Signals on a separate `UiState` object:

- `connected: Signal<boolean>` — connection status
- `selectedChannel: Signal<number>` — Sel highlight
- `mode: Signal<'engineer' | 'musician'>` — persona
- `activeWorkspace: Signal<...>`, `activePlayTab`, `activeFxTab`, `activeSetupTab` — tab routing
- `metersOn: Signal<boolean>` — local subscription toggle

These are imported directly by the relevant UI surface, not address-keyed. Persistence (localStorage) lives on these signals only — the address-store never persists.

---

## 6. Subscription / Push Management

### 6.1 What the firmware exposes (recap from brief)

| Mechanism | Purpose | Lifecycle |
|---|---|---|
| `/xremote` | Subscribe to all change broadcasts (faders, mutes, names, etc.) | TTL 10 s, renew every ~7 s |
| `/subscribe ,si <addr> [tf]` | Per-leaf streamed subscription with rate code | TTL bounded; renew on heartbeat |
| `/snapshot` | One-shot full state dump | One-off on connect |
| `/node ,s "<path>"` | Group read of a subtree | One-off on demand |

Rate codes: `0`=200 ms (5 Hz), `2`=100 ms (10 Hz), `40`=20 ms (50 Hz), `80`=33 ms (30 Hz approx — mapped from "3/10s" description).

### 6.2 Subscription manager API

```ts
// Module: net/subscriptions.ts

export class SubscriptionManager {
  private xremoteRenewTimer: number | null = null;
  private streams = new Map<string, StreamHandle>();  // addr -> handle

  constructor(private send: SendFn) {}

  /** Open the global change feed. Call on connect. */
  startXremote(): void {
    this.send(encodeMessage('/xremote', '', []));
    this.xremoteRenewTimer = window.setInterval(
      () => this.send(encodeMessage('/xremote', '', [])),
      7_000,
    );
  }

  stopXremote(): void {
    if (this.xremoteRenewTimer) clearInterval(this.xremoteRenewTimer);
    this.xremoteRenewTimer = null;
    this.send(encodeMessage('/xremote', 's', ['off']));
  }

  /** Open a per-address stream at the requested rate. Returns a handle
   *  that's idempotent on repeat acquireStream calls — refcount based. */
  acquireStream(addr: string, rate: SubRate): StreamHandle {
    let h = this.streams.get(addr);
    if (!h) {
      h = { addr, rate, refCount: 0, };
      this.streams.set(addr, h);
      this.send(encodeMessage('/subscribe', 'si', [addr, rateCode(rate)]));
    } else if (rate < h.rate) {
      // Faster rate requested — re-issue at the higher rate.
      h.rate = rate;
      this.send(encodeMessage('/subscribe', 'si', [addr, rateCode(rate)]));
    }
    h.refCount++;
    return h;
  }

  /** Reverse of acquireStream. Last release tears down. */
  releaseStream(h: StreamHandle): void {
    h.refCount--;
    if (h.refCount === 0) {
      this.streams.delete(h.addr);
      this.send(encodeMessage('/unsubscribe', 's', [h.addr]));
    }
  }

  /** Pause everything — called when the window loses focus or visibility. */
  pauseAll(): void { /* unsubscribe all, retain handles, restart on resumeAll */ }
  resumeAll(): void { /* re-issue subscribes for every retained handle */ }
}
```

### 6.3 Subscription policy

Tabs/panels acquire streams in `onMount` and release in `onCleanup`:

```ts
// Inside Spectrum panel
onMount(() => {
  const h = subs.acquireStream('/spectrum/main', 'rate_30hz');
  onCleanup(() => subs.releaseStream(h));
});
```

This replaces today's manual `dispatcher.subscribeSpectrum() / unsubscribeSpectrum()` pairs scattered through `applyVisibility()` in `main.ts`. Streams are reference-counted, so two open panels watching the same address only issue one subscribe.

### 6.4 Default rate map (host policy)

| Stream | Rate | Reason |
|---|---|---|
| `/meters/input`, `/meters/output`, `/meters/host`, `/meters/gr` | 30 Hz | Smooth meter motion |
| `/spectrum/main` | 30 Hz | FFT canvas refresh |
| `/synth/mpe/voices` | 30 Hz | Voice orb position |
| `/midi/events` | as-fast | Note events are sparse, push them all |
| `/loop/length`, `/clock/bpm` | 5 Hz | Just for live readout while a take records |
| Everything else (faders, mutes, names) | via `/xremote` | One subscription, all controls |

### 6.5 Pause on visibility

```ts
document.addEventListener('visibilitychange', () => {
  if (document.hidden) subs.pauseAll();
  else                 subs.resumeAll();
});
```

Pulled out into `subscriptions.ts` so the policy is testable without DOM.

---

## 7. Throttling / Coalescing

Two distinct concerns: outbound (UI gestures → firmware) and inbound (firmware echoes → UI repaint).

### 7.1 Outbound — fader gestures

Pointer events fire at the device's report rate (60 Hz on most touchpads, 120-1000 Hz on gaming mice). Sending every event would (a) saturate the bridge's 20 ms gap, (b) make the firmware's per-control queue tail-stale, and (c) waste CDC bandwidth. The current `sendThrottled()` is the right shape; the redesign keeps it.

```
pointermove  (rate: 60-1000 Hz)
   │
   ▼  setDragValue(v)             ◀── instant local UI update via Solid signal
   ▼  dispatcher.writeThrottled(addr, 'f', v)
                                       │
   ┌──────────── throttle, per-address ┘
   │
   ▼  if (now - lastSentAt >= 33ms)  send immediately, set lastSentAt=now
   │  else                           stash latest args, schedule trailing-edge fire
   │
   ▼  send(encodeMessage(addr, 'f', [v]))
   ▼  enqueueWrite(slipEncode(packet))   in serial-bridge — TX_GAP_MS=20
```

Two important properties:

- **Trailing-edge fire.** The last value the user dragged to is always the last value sent. Today's `sendThrottled` does this; preserve it. Without trailing-edge, releasing a fader between throttle windows leaves the firmware on the previous value.
- **Flush on release.** `pointerup` calls `writeFlush(addr, 'f', v)` which fires the trailing send synchronously and clears the pending timer. This avoids a 33 ms tail latency on every gesture end.

### 7.2 Why the v1 dev surface feels slow

Triaged from the codebase (without re-running it):

1. **Synchronous DOM writes in `Signal.subscribe` callbacks.** Every echo notifies every subscriber inline. A meter blob arriving at 30 Hz fires 20 signals × N subscribers each, all synchronous. Solid's compiled effects + the existing `rafBatch` already address this; the redesign makes them universal (today they're partial).
2. **Echo on every `writeThrottled` round-trip.** When the user is dragging, the firmware echoes back *every* throttled write. That echo lands in `handleIncoming`, triggers `Signal.set()`, fires subscribers — even though `Object.is` short-circuits same-value sets, the dragged-value path *isn't* same-value (it's the value we just sent, but the local Signal is already at it because of optimistic update). Object.is short-circuit applies, no notify, no DOM write. This part is fine.
3. **Fader binding direct to value, not to drag-state.** Today, the fader visual's `subscribe` reads `chL.fader` — which is updated optimistically on every pointermove AND by every echo. The visual repaint is in the optimistic-update path, which is fine; but the slow-feel report suggests the repaint isn't on RAF — it's a synchronous style write. Solid + RAF batching solves this.

The redesigned `<Fader>` component (§4.4) reads `displayed()` which is `dragValue() ?? value()`. During a drag, only the local optimistic signal drives the visual; echoes don't fight the drag. After release, `dragValue` is null, and the visual tracks the canonical signal.

### 7.3 Inbound — RAF batching

Today's `raf-batch.ts` keys by element. Keep the API; ensure every meter / spectrum / voice telemetry consumer uses it. The contract:

```ts
rafBatch(domElement, () => {
  // any DOM write — collapsed to one per frame per key
  domElement.style.height = `${peak * 100}%`;
});
```

In Solid, the equivalent is `createRenderEffect()` / `createMemo()` — Solid's runtime already collapses synchronous signal updates into a single render pass. We don't need to wrap every write in `rafBatch` if Solid is doing it. But for raw DOM canvas paints (meter LED ladder, spectrum), `rafBatch` stays.

### 7.4 Bridge-side throttle

The `serial-bridge.mjs` `TX_GAP_MS=20` (20 ms minimum gap) is correct and stays. The host-side per-address 33 ms throttle ensures we never feed the bridge faster than it can drain. A common misconception worth recording in code comments: 33 ms (host) + 20 ms (bridge) is not additive — the bridge gap only applies on consecutive-message bursts, not steady-state.

### 7.5 Echo storms

When `/xremote` is on and the firmware echoes mirror writes (e.g. linked stereo pair), the host receives both echoes for one user gesture. With Solid + Object.is short-circuit, this is a no-op for the moved side and a single notify for the partner. No special handling needed — assert this in tests.

---

## 8. Stereo-Link UX

The `/config/chlink/N-M` semantics: writes to either side mirror server-side; both addresses echo the same value. The host UI needs to:

1. **Detect link state.** From topology — every strip carries `link?.linkAddr` and `link?.partnerPrefix`. The store has a Signal at `linkAddr` updated by echoes.
2. **Render linked pairs as visually paired.** When linked: shared scribble strip color span across both columns, fader handles tracked in lockstep, muted L+R buttons merged into one wide button. When unlinked: independent.
3. **Optimistically mirror local writes.** Touching L's fader during a drag updates both L's and R's local Signals so the visual stays paired without waiting for the echo.

### 8.1 Visual states

```
unlinked:                              linked (X32 style):

┌──┬──┐                                ┌──┬──┐
│N1│N2│  separate names                │ Name 1+2 │  shared name (or "L/R")
├──┼──┤                                ├──────────┤
│  │  │                                │          │
│║ │║ │  separate fader rails          │   ║║║   │  one fader, two handles tracked
│  │  │                                │          │
├──┼──┤                                ├──────────┤
│M │M │  separate mutes                │   MUTE   │  shared mute
├──┼──┤                                ├──────────┤
│ link  │                              │  linked  │  link tab inverted
└──┴──┘                                └──────────┘
```

The current dev surface keeps L+R separate even when linked, with both knobs interactive — that's a **valid choice** (a sound engineer can briefly de-link with a touch, tweak, re-link). The redesign keeps this option behind a CSS variant toggle (`data-link-style="separate" | "merged"`).

Default: `merged` for stereo-mic / stereo-line strips (mic L+R, line L+R), `separate` for the master (L/R independent visual is iconic) and any pair where the user has set a non-zero pan offset (auto-fall-back: panned pairs imply intent to control L+R independently in volume).

### 8.2 Interaction

```ts
// Inside <Fader> for a linked-pair strip
const onMove = (frac: number) => {
  setDragValue(frac);
  dispatcher.writeThrottled(p.addr, 'f', frac);
  // Optimistic mirror to the partner. The firmware will echo the
  // mirrored value too, but we don't wait — partner repaints
  // synchronously alongside.
  if (linkInfo?.linked) {
    store.leaf<number>(linkInfo.partnerAddr)?.set(frac);
  }
};
```

The dispatcher does not send to `partnerAddr` — the firmware mirrors. Sending both would race the firmware's mirror logic and produce two `chlink` echoes per tick.

### 8.3 Breaking and re-establishing link

- **Break (unlink):** Click LINK button. Send `/config/chlink/1-2 ,i 0`. Firmware echoes 0; UI flips to separate state.
- **Re-link:** Click LINK button. Snap R fader visual to L's value optimistically (firmware does the same on its side). Send `/config/chlink/1-2 ,i 1`.

### 8.4 Edge cases

- **Unlinked but pair-rendered (XLR layout).** XLR 1+2 are layout-paired but firmware-unlinked by default. Topology `link.linkAddr` exists but the value is 0 and the user can't link them (they're independent mono mics on different ADC channels). The pair renderer treats `link.linkAddr` value 0 as "render separate" and presents the LINK button greyed if the firmware says the addresses aren't link-capable (a `meta: { link_capable: false }` flag in the topology).
- **Mid-drag link toggle.** If the user toggles link during a fader drag, the in-flight `dragValue` becomes the new linked-pair value. The drag handler's optimistic mirror logic checks `linkInfo.linked` on every event, so the next pointermove mirrors.

---

## 9. Layout

### 9.1 Workspace structure (already shipped in v1, preserved)

```
┌────────────────────────────────────────────────────────────────┐
│  T-DSP Dev Surface       [Meters: ON]  [Mode: Engineer] [Conn] │  ← header
├────────────────────────────────────────────────────────────────┤
│  MIX  PLAY  TUNE  FX  SETUP                                    │  ← workspace tabs
├────────────────────────────────────────────────────────────────┤
│                                                                │
│   ( active workspace renders here )                            │
│                                                                │
├────────────────────────────────────────────────────────────────┤
│  ( bottom dock — Engineer fader bank or Musician keyboard )    │
└────────────────────────────────────────────────────────────────┘
```

### 9.2 MIX workspace — channel strips, master section

```
┌─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┬───────────┐
│ Ch1 │ Ch2 │ Ch3 │ Ch4 │ Ch5 │ Ch6 │ XLR1│ XLR2│ XLR3│ XLR4│SYNTH│  MASTER + │
│ USB │ USB │ Line│ Line│ Mic │ Mic │     │     │     │     │ BUS │  HOST     │
│  L  │  R  │  L  │  R  │  L  │  R  │     │     │     │     │     │           │
│     │     │     │     │     │     │     │     │     │     │     │           │
│ ███ │ ███ │ ███ │ ███ │ ███ │ ███ │ ███ │ ███ │ ███ │ ███ │ ███ │ ████  ███ │  meters
│  ║  │  ║  │  ║  │  ║  │  ║  │  ║  │  ║  │  ║  │  ║  │  ║  │  ║  │  ║  │  ║ │  faders
│  ║  │  ║  │  ║  │  ║  │  ║  │  ║  │  ║  │  ║  │  ║  │  ║  │  ║  │  ║  │  ║ │
│ MUTE│ MUTE│ MUTE│ MUTE│ MUTE│ MUTE│ MUTE│ MUTE│ MUTE│ MUTE│ MUTE│ MUTE │MUTE│
│ SOLO│ SOLO│ SOLO│ SOLO│ SOLO│ SOLO│ SOLO│ SOLO│ SOLO│ SOLO│     │      │    │
│  ┊  │  LINK     │  LINK     │  LINK     │     │     │     │     │  LINK    │
│ REC │ REC │ REC │ REC │ REC │ REC │ REC │ REC │ REC │ REC │     │  LOOP    │
└─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┴───────────┘
```

The master + host strip pair is docked right via the existing `output-dock` flex pattern.

### 9.3 PLAY workspace — synth tabs + arp + beats + loop

```
┌────────────────────────────────────────────────────────────────────┐
│  ▸ Synths   Arp   Beats   Loop                                     │  ← inner subnav
├────────────────────────────────────────────────────────────────────┤
│                                                                    │
│  ▸ Dexed  MPE  Neuro  Acid  Supersaw  Chip                         │  ← synth subnav
│                                                                    │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │  ( active synth panel — controls per descriptor )            │  │
│  └──────────────────────────────────────────────────────────────┘  │
│                                                                    │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │  88-key keyboard (engineer mode) / hidden (musician — moves  │  │
│  │  to bottom dock)                                             │  │
│  └──────────────────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────────────┘
```

### 9.4 SETUP workspace — codec / clock / raw / serial

```
┌────────────────────────────────────────────────────────────────────┐
│  ▸ Codec   Clock   Raw OSC   Serial                                │
├────────────────────────────────────────────────────────────────────┤
│                                                                    │
│  ▸ TAC5212  TLV320ADC6140                          (codec subnav)  │
│                                                                    │
│  ┌──────────┐  ┌─────────────────────────────────────────────────┐ │
│  │  Tabs    │  │                                                 │ │
│  │  ─────   │  │   ( panel sections from descriptor )            │ │
│  │  Routing │  │                                                 │ │
│  │  ADC     │  │                                                 │ │
│  │  DAC     │  │                                                 │ │
│  │  EQ      │  │                                                 │ │
│  │  HPF     │  │                                                 │ │
│  │  ...     │  │                                                 │ │
│  └──────────┘  └─────────────────────────────────────────────────┘ │
└────────────────────────────────────────────────────────────────────┘
```

Each codec panel is a sectioned form rendered from the descriptor (today's `codec-panel-config.ts` shape, possibly server-driven later).

### 9.5 FX workspace — bus FX, main processing, spectrum

```
┌────────────────────────────────────────────────────────────────────┐
│  ▸ Bus FX   Main Processing   Spectrum                             │
├────────────────────────────────────────────────────────────────────┤
│                                                                    │
│   ( active sub-panel )                                             │
│                                                                    │
└────────────────────────────────────────────────────────────────────┘
```

### 9.6 Status bar (sketched, not yet present)

```
┌────────────────────────────────────────────────────────────────────┐
│ ● Connected  COM4  CPU: 32%  Blocks: 4/64  Frames: 32 ms  /xremote │  ← status bar
└────────────────────────────────────────────────────────────────────┘
```

Bottom-most strip on all workspaces. Subscribes to a small set of meta-leaves the firmware exposes (`/-info/cpu`, `/-info/blocks`, `/-info/audio_block_ms`).

### 9.7 Component nesting (canonical)

```
<App>
  <Header>
    <Title/>
    <ConnectButton/>
    <ModeToggle/>
    <MeterToggle/>
  </Header>

  <WorkspaceTabs/>

  <Workspace activeId={...}>
    <MixWorkspace>
      <MixerRow>
        <For each={topology.strips}>
          {(strip) => <Strip node={strip} store={store} dispatcher={dispatcher}/>}
        </For>
        <SynthBusStrip/>
        <OutputDock>
          <MainStrip role="main_l_r"/>
          <HostStrip/>
        </OutputDock>
      </MixerRow>
    </MixWorkspace>

    <PlayWorkspace>
      <PlaySubnav/>
      <Switch on={activePlayTab()}>
        <SynthsPanel/>
        <ArpPanel/>
        <BeatsPanel/>
        <LoopPanel/>
      </Switch>
      <KeyboardDock/>
    </PlayWorkspace>

    ...
  </Workspace>

  <BottomDock variant={mode()}/>
  <StatusBar/>
</App>
```

---

## 10. Migration Plan

### 10.1 Salvageable from today

Keep verbatim:

- `serial-bridge.mjs` — already agnostic to address tree, already throttled at 20 ms gap, already auto-reconnects. Zero changes.
- `electron/main.js`, `electron/launch.js` — Electron shell.
- `osc.ts` — OSC encoder/decoder. Add `/node` reply parsing as a small extension.
- `transport.ts` — SLIP encoder + StreamDemuxer.
- `state.ts` `Signal<T>` class — kept as the substrate.
- `signal-bridge.ts` — Solid adapter; central to the new design.
- `raf-batch.ts` — DOM-write coalescing.
- `tailwind.css` config — Tailwind v4 setup.
- `codec-panel-config.ts`, `adc6140-panel-config.ts` — usable as v1 panel descriptors until the firmware emits them via `/node`.
- `biquad-design.ts` — biquad math.

### 10.2 To replace

- `dispatcher.ts` (1791 lines) → `dispatcher.ts` + `address-store.ts` + `subscriptions.ts` (~600 lines combined). The address-keyed store eliminates the per-leaf `if/else` chains in `handleIncoming`.
- `state.ts` (788 lines) → trimmed to `Signal<T>`, `UiState`, and topology types (~150 lines). Mixer state lives in the address store.
- `main.ts` (~1000 lines) → `App.tsx` (~250 lines) + a topology-driven router. The 7-row grid layout, persona toggle, workspace+tab persistence stay.
- All `ui/*.ts` panels → `components/*.tsx` Solid components, parameterized by descriptor.

### 10.3 Phasing

**Phase 0 — preconditions (firmware)**

1. Firmware exposes `/-info/topology` with strips + capabilities + links + groups for the current mixer.
2. `/snapshot` enriched: emits topology before values.
3. `/xremote` and `/subscribe` confirmed wire formats.

Without these, the host can't be topology-driven for channels. v1 can ship with codec/synth panels still using local descriptors and *just* the strip side server-driven.

**Phase 1 — address store + dispatcher rewrite**

1. Add `address-store.ts`. Keep the existing `MixerState` scaffolding alive.
2. Add a feature flag `useAddressStore` defaulting to false.
3. Rewrite `dispatcher.handleIncoming` as a small `store.leaf(addr)?.set(arg)` plus a fallback to the existing per-address `if`s (for codec listeners).
4. Validate that flagged-on, the existing UI still works when the flag is enabled (signals still update via the legacy hand-written subscribers).

**Phase 2 — Solid Strip component**

1. Add `components/Strip.tsx` rendering from a strip descriptor.
2. Migrate the channel-pair render path to Solid + Strip when the flag is enabled. Keep the legacy `channel-pair.ts` available for one release behind the flag.
3. Validate that fader response is now real-time (drag a fader, watch the visual; should never lag the cursor).

**Phase 3 — topology fetch**

1. On connect, fetch `/-info/topology` via `/node`. On error or timeout, fall back to a hard-coded topology that mirrors today's 10-channel layout.
2. Build the strip descriptor list from topology and feed `<For>` over `<Strip>`.

**Phase 4 — Solid Panel migration**

1. Migrate codec panel renderer to Solid.
2. Migrate synth panels one at a time (Dexed first; it's the most-used).
3. Beats / Arp / Looper / Clock — these are larger custom UIs; migrate when the rest is stable.

**Phase 5 — kill the legacy path**

1. Delete the feature flag.
2. Delete `channel-pair.ts`, `main-bus.ts`, `host-strip.ts`, `input-host-strip.ts` (replaced by `<Strip role="...">`).
3. Trim `state.ts`.

Each phase ships independently. The flag-gated approach means the dev surface keeps working at every commit.

### 10.4 Tests to gate the migration

- **Topology round-trip.** Mock firmware emits a known topology; host renders the expected strip count and capability widgets.
- **Echo idempotency.** Sending a fader value, receiving the echo, must not fire a redundant DOM write (assert via a render-counting subscriber).
- **Stereo link mirror.** Touch L's fader; assert R's signal updates within the same task tick (no echo round-trip required for visual consistency).
- **Subscription refcount.** Open Spectrum tab twice (synthetic), close once; the firmware should receive one subscribe and zero unsubscribes.
- **Throttle trailing-edge.** Drive `writeThrottled` 100 times in 50 ms, assert exactly 2 sends (immediate + trailing) and the trailing is the last value.

---

## 11. Open Questions

These need a decision (most are pick-one) before Phase 1 starts.

### 11.1 Topology format — flat list vs nested tree

The schema in §3.1 uses a flat `nodes: TopologyNode[]` list with parent links by index. Alternative: nested tree (`children: TopologyNode[]`).

- Flat is easier to wire-encode (one OSC bundle per node, no recursion in the firmware emitter).
- Nested is easier for the host to consume (no rebuild step).

**Recommendation:** flat on the wire, host rebuilds nested on ingest. Pick-one decision.

### 11.2 Topology versioning

- Bump `version` on incompatible changes; host warns and falls back to legacy hard-coded topology if mismatch?
- Or: host accepts any version, just renders what it can?

**Recommendation:** soft compatibility — log a warning, render what we recognize, ignore unknown capability kinds. The dev surface is dev-mode software; better to surface partial UI than refuse to connect.

### 11.3 Codec panel — server-driven or local?

The TAC5212 panel descriptor is 343 lines of TS today. The firmware does not currently emit it. Options:

- **A. Keep local for v1, server-driven for v2.** Saves firmware effort now; host-side edits when registers change.
- **B. Move to server-side immediately.** Firmware emits the descriptor; host renders. Adds firmware complexity (PROGMEM tables for option strings).
- **C. Keep local forever.** Codec panels are bench tools, not user-facing; server-driven adds no value.

**Recommendation:** A. The firmware milestone budget is better spent on the strip-side topology.

### 11.4 Master strip representation

The master is currently rendered as L+R separate signals (`/main/st/mix/faderL`, `/main/st/mix/faderR`) with explicit `link` and `loopEnable`. Topology options:

- **One strip with role `'main'` and capability list including `link`.** Matches X32. Renderer handles the L/R split internally.
- **Two strips with role `'main_l'` and `'main_r'` plus a separate group node.** Mirrors today's state shape.

**Recommendation:** one strip, role `'main'`, internal capability for L/R. Matches X32 mental model and reduces topology size.

### 11.5 Mute groups, DCAs

The brief calls out "mute groups, DCA assigns" as part of the X32-flavor goal. Firmware doesn't have these yet. Topology already accommodates `GroupNode` for both. Ship the renderer with empty groups in v1; firmware adds `/dca/N/mix/fader`, `/mute_group/N` later.

**Open:** does the firmware roadmap include these? If not, drop them from v1 design and add in v2 when firmware lands them.

### 11.6 Persistent state — what's saved?

Today: `t-dsp.ui.mode`, `t-dsp.ui.workspace`, `t-dsp.ui.playTab`, `t-dsp.ui.fxTab`, `t-dsp.ui.setupTab`. Topology should not be cached across sessions (might be stale; cheap to re-fetch).

**Open:** should we save the last-connected COM port / WS URL? Today the bridge auto-detects. If a user ever runs two Teensy boards, this matters; otherwise no.

### 11.7 Status bar metering

`/-info/cpu`, `/-info/blocks`, `/-info/audio_block_ms` — does the firmware expose these? If not, drop the status bar from v1 and add it when the firmware-side counters land.

**Open:** confirm with firmware roadmap.

### 11.8 TUNE workspace per-channel surface

Today's TUNE is a stub. The plan: per-channel HPF/EQ/dynamics/sends matrix when the firmware exposes per-channel processing. The Strip's `subgroup` capability already accommodates this — TUNE is just a different render of the same descriptor (full panel vs strip-strip).

**Open:** firmware roadmap for per-channel processing; until that lands, TUNE stays a stub.

### 11.9 Solid migration scope creep

If we go full Solid, we should also reconsider:

- **Move from custom `Signal<T>` to native Solid signals everywhere?** Possible, but would require rewriting `dispatcher.handleIncoming`'s direct `set()` calls (Solid setters need to be invoked inside reactive scope). The bridge approach (custom Signal + `asSolid`) sidesteps this; recommend keeping it.
- **Solid Router?** No — the workspace+tab pattern is too custom for Router; keep the existing display-toggle approach behind a `<Switch>` + `<Match>`.
- **Solid Store (createStore)?** No — the address-keyed map of `Signal<T>` outperforms a single nested store for our access pattern.

**Recommendation:** Solid for components, custom Signal for dispatcher. Don't blanket-rewrite.

### 11.10 Test infrastructure

The current dev surface has **no tests**. The migration plan calls for tests at every phase boundary. Open question: where do they live?

- Vitest under `projects/t-dsp_web_dev/test/`?
- Add a `pnpm test` script to package.json?

**Recommendation:** Vitest, colocated `*.test.ts` next to the modules. Keep CI-light; this is a bench tool.

---

## Summary

| Topic | Decision |
|---|---|
| Shell | Electron 33 (keep) |
| Reactivity | Solid for components; custom `Signal<T>` for dispatcher/store substrate |
| Rendering | Solid JSX, fine-grained reactivity |
| Styling | Tailwind v4 (already in) |
| State model | Address-keyed `AddressStore` of `Signal<T>` + small `UiState` for non-firmware UI |
| Topology | Server-driven via `/node ,s "/-info/topology"`; flat-list wire format; host rebuilds tree |
| Strip contract | `(node, store, dispatcher)` with `capabilities[]` driving render |
| Throttle | 33 ms per-address outbound (trailing-edge fire), `rafBatch` inbound, bridge 20 ms gap |
| Stereo link | Topology-declared partner; merged-pair render default; optimistic mirror on drag |
| Migration | Feature-flagged, 5-phase, legacy stays behind flag until last phase |

The redesign keeps the bench-tool ethos: small, hand-rolled where it pays, leaning on Solid only where its compiled fine-grained reactivity wins us perf. The biggest wins are (a) eliminating the per-leaf hand-typed dispatcher chains by going address-keyed, (b) topology-driven channel rendering so adding a strip is a firmware-side change only, and (c) Solid components with the existing `Signal<T>` bridge giving us real-time fader response without a framework rewrite.
