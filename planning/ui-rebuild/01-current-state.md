# 01 — Current state

A factual audit of the current frontend at
`projects/t-dsp_tac5212_audio_shield_adaptor/tools/web_dev_surface/`.
Anything not listed here is also not changed.

## Inventory

### File layout

```
projects/t-dsp_tac5212_audio_shield_adaptor/tools/web_dev_surface/
├── electron/                     Electron main process
├── serial-bridge.mjs             Node WebSocket ↔ serial bridge
├── package.json                  Vite + electron + serialport + ws
├── vite.config.ts
├── tsconfig.json
├── index.html
└── src/
    ├── main.ts                   706 LOC — boot wiring, all tab plumbing
    ├── style.css                 2,110 LOC — single dark theme
    ├── osc.ts                    OSC 1.0 encoder/decoder
    ├── transport.ts              SLIP encoder + StreamDemuxer
    ├── state.ts                  Signal<T> + MixerState + per-engine state
    ├── dispatcher.ts             OSC ↔ state translation, address routing
    ├── codec-panel-config.ts     TAC5212 panel descriptor
    ├── adc6140-panel-config.ts   TLV320ADC6140 panel descriptor
    ├── raf-batch.ts              requestAnimationFrame coalescer
    └── ui/
        ├── connect.ts            22 LOC
        ├── channel-pair.ts       110 LOC
        ├── cells.ts              197 LOC — meter / fader / button cells
        ├── chip-panel.ts         281 LOC
        ├── clock-panel.ts        259 LOC
        ├── codec-panel.ts        183 LOC — descriptor renderer (codec + adc6140)
        ├── dexed-panel.ts        268 LOC
        ├── fx-panel.ts           138 LOC
        ├── host-strip.ts          86 LOC
        ├── input-host-strip.ts    86 LOC
        ├── keyboard.ts           303 LOC
        ├── looper-panel.ts       286 LOC
        ├── main-bus.ts            94 LOC
        ├── mpe-panel.ts          733 LOC
        ├── neuro-panel.ts        693 LOC
        ├── processing-panel.ts   172 LOC
        ├── raw-osc.ts            153 LOC
        ├── serial-console.ts     206 LOC
        ├── spectrum.ts           686 LOC — canvas-based spectrum
        ├── supersaw-panel.ts     255 LOC
        ├── synth-bus.ts           92 LOC
        ├── acid-panel.ts         273 LOC
        ├── arp-panel.ts          608 LOC
        ├── beats-panel.ts        374 LOC
        ├── beats-presets.ts      180 LOC
        └── util.ts                13 LOC
        — total ~9,500 LOC across 28 files
```

### Existing top-level tabs (flat, ten of them)

```
[ Mixer ][ Spectrum ][ Synth ][ FX ][ Processing ][ Loop ][ Beats ][ Clock ][ Arp ][ System ]
```

The Synth tab has its own inner sub-tabs for engine selection
(Dexed / MPE / Neuro / Acid / Supersaw / Chip), so a partial
two-level hierarchy already exists there. Everything else is flat.

### Reactive primitive

`state.ts` defines `Signal<T>` — a small subscribable getter/setter:

```ts
export class Signal<T> {
  get(): T;
  set(v: T): void;            // skips notify if Object.is(prev, v)
  subscribe(l: Listener<T>): () => void;  // calls l(currentValue) on subscribe
}
```

Every panel subscribes to the signals it needs and updates DOM imperatively
on each notification. There is **no reconciliation, no virtual DOM, no
component lifecycle** — just direct DOM mutation in response to signal
changes.

This shape is **identical to Solid's `createSignal`** (the get/set/subscribe
trio), which is the core reason Solid is the recommended migration target.
A 5-line adapter wraps any `Signal<T>` as a Solid signal.

### Wire layer

The wire layer is the load-bearing part that this rebuild **does not touch**:

- **`transport.ts`** — SLIP encoder + StreamDemuxer (separates SLIP frames
  from plain ASCII text on the same USB CDC stream).
- **`osc.ts`** — minimal OSC 1.0 encoder/decoder (i, f, s, b, T, F, N, I).
- **`dispatcher.ts`** — single source of truth for OSC ↔ state translation.
  Address routing, type coercion, send-side throttling, snapshot-on-connect
  handshake, meter subscription, MPE telemetry routing.
- **`state.ts`** — the MixerState model. Per-channel `ChannelState`,
  per-bus `BusState`, per-engine state blocks (`DexedState`, `MpeState`,
  etc.), shared modules (`ProcessingState`, `FxState`, `LooperState`,
  `ClockState`, `BeatsState`, `ArpState`, `SynthBusState`).
- **`codec-panel-config.ts`** — descriptor for the TAC5212 panel.
- **`adc6140-panel-config.ts`** — descriptor for the TLV320ADC6140 panel.
- **`raf-batch.ts`** — `requestAnimationFrame`-driven coalescer for
  high-rate updates (e.g. fader drag, meter blob).

This list is the contract with the firmware. Nothing crossing the
WebSocket bridge changes shape during this rebuild. If a panel rewrite
seems to require a protocol change, **stop and reconsider** — the rebuild
is a presentation rebuild, not a protocol rebuild.

## Touch deficiencies (the structural problem)

From `style.css`:

```
.cell           { width: 26px }                   ← 26 px columns
.fader          { height: 160px }                  ← native vertical range
                                                     thumb is ~24px wide
.mute, .solo,
.link-btn,
.rec-btn        { height: 24px; font-size: 11px }  ← 24 px tall
.strip-name     { font-size: 9px }                 ← 9 px labels
.view-tab       { padding: 10px 18px;              ← OK-ish (~38 px)
                  font-size: 12px }
#app            { max-width: 1200px }              ← caps width
```

Reference targets:

| Source | Minimum tap target |
|---|---|
| Apple HIG | 44 × 44 pt (≈44 px @ 1×) |
| Microsoft Fluent / Surface guidance | 9 mm (≈40 px @ 100% scaling, ≈48 px @ 125%) |
| Material Design | 48 × 48 dp |

The current UI is at 24 px. A finger pad covers roughly four neighboring
controls. Faders are ~24 px wide, so dragging them with a fingertip is
unreliable. Labels at 9 px are unreadable at arm's length.

## Hierarchy deficiencies (the navigation problem)

The flat ten-tab structure mixes semantic categories at the same level:

| Tab | True role | Should live in |
|---|---|---|
| Mixer | Bus mixing | MIX |
| Spectrum | Diagnostic | FX (or SETUP) |
| Synth | Sound generator | PLAY > Synths |
| FX | Bus FX | FX |
| Processing | Bus processing | FX |
| Loop | Capture / playback | PLAY > Loop |
| Beats | Sound generator (drums) | PLAY > Beats |
| Clock | Tempo source | SETUP (peer of Codec/System) |
| Arp | Performance modifier | PLAY > Arp (own peer tab) |
| System | Hardware / debug | SETUP |

Codec lives inside Mixer currently (or sometimes broken out — see
`tac5212-hw-dsp/scaffolding/main.ts.diff.md`). It belongs in SETUP. The
Raw OSC and Serial console are inside the mixer view — they belong in
SETUP too.

There is also **no place for per-channel processing**. HPF, EQ,
dynamics, and a sends matrix all need a "selected channel" detail page,
which the current flat structure doesn't allow for. This is the TUNE
workspace's reason to exist.

## What stays

- **Wire layer** (listed above) — every byte unchanged.
- **`state.ts`** — kept. Adapter wraps `Signal<T>` as Solid signals.
- **Codec panel descriptors** — kept. The descriptor-driven renderer is
  one of the better parts of the existing code; only the renderer
  (`codec-panel.ts`) gets re-implemented in Solid + Tailwind, and even
  that should be straightforward because the descriptor's data shape is
  trivially mappable.
- **`raf-batch.ts`** — kept for high-rate paths (meters, MPE telemetry,
  beats cursor).
- **`keyboard.ts`** — likely kept; the on-screen piano is a reusable
  component that just needs a Solid wrapper. May need touch-gesture
  improvements (slide between keys, velocity-from-Y) but the core is fine.
- **`spectrum.ts`** — kept; canvas drawing is framework-agnostic. Just
  re-mounted into the FX workspace.
- **`beats-presets.ts`** — kept; pure data.

## What gets rewritten

- **`main.ts`** — replaced. New shell does workspace routing, mode toggle,
  Sel state, persistent bottom strip mounting, and Solid root setup.
- **`style.css`** — retired in favor of Tailwind v4 with a `@theme` block
  carrying the design tokens. A small CSS file for things Tailwind can't
  express (e.g. `<input type="range">` track styling fallbacks) may
  remain.
- **All `ui/*-panel.ts` panels** — ported to Solid `.tsx` components.
  Panel logic (which signals to subscribe, what to send on change) is
  preserved; only rendering is rewritten.
- **`ui/cells.ts`** — replaced with new touch-first fader and meter
  components (custom pointer-event-driven, not native `<input>`).
- **`ui/channel-pair.ts`, `main-bus.ts`, `host-strip.ts`,
  `input-host-strip.ts`, `synth-bus.ts`** — rewritten as a single
  unified strip component parameterized by role.

## What is added

- **Workspace shell** with the five-workspace tab bar.
- **`selectedChannel` Signal** added to MixerState.
- **`mode` Signal** (`'engineer' | 'musician'`) persisted to
  localStorage.
- **Persistent bottom strip** component with two variants.
- **TUNE workspace** with HPF / parametric EQ / dynamics / pan / sends
  layout — partly stubbed until firmware exposes those controls.
- **Sends-on-faders mode toggle** in MIX with bus picker.
- **Gesture handlers** — long-press, swipe, two-finger reset, pull-down
  drawer.
- **Tailwind v4 setup** + design-token theme block.
- **`signal-bridge.ts`** — wraps existing `Signal<T>` as Solid signals.

## Performance baselines to preserve

These are observed-good behaviors of the current code that the rebuild
must not regress:

- **30 Hz meter updates.** Per `style.css` line 268 comment, meter fill is
  driven by `transform: scaleY(0..1)` instead of `height` to avoid
  forced reflow. Solid components must use the same approach (style
  binding to `--scale` CSS var, transform driven by var).
- **Coalesced fader sends.** Per `serial-bridge throttle` memory:
  TX_GAP_MS=20 ms throttle on the bridge means UI burst senders must
  emit ≤ 25 msg/sec. The existing `raf-batch.ts` enforces this. Solid
  panel handlers must continue to route through it for any signal that
  can drag.
- **MPE voice telemetry at 30 Hz.** 4 voices × 6 signals each. The
  fine-grained Signal model means only the voice that changed re-renders.
  Solid's fine-grained reactivity preserves this; React would lose it
  without manual memoization.
- **16-step beats grid.** Per-step `Signal<boolean>` and `Signal<number>`
  means a single-cell click only re-renders that cell. Same fine-grained
  reactivity assumption.
- **Spectrum at 60 fps.** Canvas-based; framework-agnostic.

## Existing OSC address conventions (do not break)

Per `02-osc-protocol.md` in the OSC mixer foundation epic, the address
tree uses X32-flavored conventions:

- `/ch/NN/mix/fader` — float 0..1 normalized
- `/ch/NN/mix/on` — int (X32-inverted: 1 = unmuted)
- `/main/st/mix/...` — main bus
- `/codec/tac5212/...` — codec panel subtree
- `/codec/adc6140/...` — secondary codec subtree
- `/synth/{dexed,mpe,neuro,acid,supersaw,chip}/...` — per-engine
- `/clock/...`, `/loop/...`, `/beats/...`, `/arp/...`, `/fx/...`,
  `/processing/...`
- `/-snap/save`, `/-snap/load` — scenes
- `/sub` — meter subscription handshake (provisional)
- `/meters/input`, `/meters/output`, `/meters/host` — meter blobs

Address construction is **only** in `dispatcher.ts`. Panels never build
addresses; they call typed setters on dispatcher. This stays true after
the rebuild.
