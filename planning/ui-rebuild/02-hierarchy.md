# 02 — Hierarchy

The navigation structure: workspaces, inner sub-tabs, persistent shell,
Sel state, Mode toggle, sends-on-faders.

## Top-level shell

```
┌──────────────────────────────────────────────────────────────────────────┐
│  ● Connect   Mode: [Engineer ▾]   Scene 01 ▾   ⏱ 120 BPM    ⚙           │   header
├──────────────────────────────────────────────────────────────────────────┤
│  [ MIX ]   [ PLAY ]   [ TUNE ]   [ FX ]   [ SETUP ]                      │   workspaces
├──────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│   active workspace content                                               │
│                                                                          │
├──────────────────────────────────────────────────────────────────────────┤
│   persona-aware persistent strip                                         │
│   • Engineer:  8-fader mini-bank + Sel + main meter                      │
│   • Musician:  keyboard + active-synth quick controls + main fader       │
└──────────────────────────────────────────────────────────────────────────┘
```

### Header items

| Item | Purpose | Notes |
|---|---|---|
| Connect indicator | Current bridge connection state | Reuses `state.connected` Signal |
| Mode toggle | Engineer / Musician | Persisted to localStorage |
| Scene picker | Save / load snapshot | Calls `/-snap/save` and `/-snap/load`. Scope: triggers + name display. No library browser. |
| Tempo readout | Current clock BPM | Read-only here; full controls in SETUP > Clock |
| Settings (⚙) | Quick access to: serial console, raw OSC, mode toggle (redundant), about | A drawer or modal, not a workspace. |

## Workspaces

### MIX (Engineer home)

```
┌──────────────────────────────────────────────────────────────────────────┐
│  Bus:  [Main ▾]  [Synth Bus]  [FX Send]    Sends-on-faders: [ off ◯on ] │
├──────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  [USB-L][USB-R][Line-L][Line-R][Mic-L][Mic-R][XLR1][XLR2][XLR3][XLR4]   │
│  [Synth Bus]                                                             │
│                                                          [ MAIN ][ HOST ]│
│                                                                          │
│  (full-height channel strips with meter, fader, mute, solo, link, rec)  │
└──────────────────────────────────────────────────────────────────────────┘
```

- Bus picker selects which bus the fader bank operates on.
  - When **Levels mode** (default): faders show that bus's main fader
    (only meaningful for Main currently; Synth-Bus and FX-Send each have
    one volume control). Most channel strips show their own fader.
  - When **Sends-on-faders mode**: every channel strip's fader represents
    that channel's send level to the picked bus. The meter still shows the
    channel's own level. Fader thumbs are tinted with the bus color so
    the user knows they are not in normal levels mode.
- Channel strip widgets: meter + fader + mute + solo + link + rec.
- Strip header is a **Sel button** — tap to set the global
  `selectedChannel` signal and (optionally) jump to TUNE.
- A long-press on a strip header opens a context menu (rename, copy
  settings to clipboard, paste, reset, link/unlink).

### PLAY (Musician home)

```
┌──────────────────────────────────────────────────────────────────────────┐
│  [ Synths ]  [ Arp ]  [ Beats ]  [ Loop ]                                │
├──────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│   Synths page:                                                           │
│     Engine:  [Dexed] [MPE] [Neuro] [Acid] [Supersaw] [Chip]              │
│     Engine controls (existing per-engine panel, ported)                  │
│                                                                          │
│   Arp page:                                                              │
│     Pattern + rate + gate + swing + octave                               │
│     Step mask grid (full-width)                                          │
│     Scale + root + transpose                                             │
│     Output channel + MPE mode + scatter                                  │
│                                                                          │
│   Beats page: existing beats-panel                                       │
│   Loop page:  existing looper-panel                                      │
│                                                                          │
└──────────────────────────────────────────────────────────────────────────┘
```

Inner-tab order is musician-flow: pick a sound (Synths) → modify it
(Arp) → drum it (Beats) → capture it (Loop). It is also signal-flow
ordered: modifier (Arp upstream of synths) → generators (Synths, Beats)
→ capture (Loop).

Arp is a **peer tab**, not nested under Synths, because the global Arp
filter routes to whichever synth currently catches the MIDI — the user
must be able to swap synth engines mid-performance without dismounting
the Arp surface. Reference: [project_two_personas.md](C:/Users/jaysh/.claude/projects/c--github-t-dsp-t-dsp-software/memory/project_two_personas.md)
in user memory.

The keyboard is the persistent bottom strip in Musician mode (see below),
so it is *not* duplicated inside any PLAY page. In Engineer mode, the
keyboard is shown at the bottom of the Synths page only.

### TUNE (Engineer-tier; shows Sel'd channel detail)

```
┌──────────────────────────────────────────────────────────────────────────┐
│  Selected: Mic L  (Sel set in MIX or via bottom strip)                   │
├──────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│   ┌─ HPF ────────┐  ┌─ EQ ───────────────────────────────────────────┐   │
│   │ Freq:        │  │  ┌─ frequency response curve ────────────┐    │   │
│   │ Slope:       │  │  │                                       │    │   │
│   └──────────────┘  │  └───────────────────────────────────────┘    │   │
│                     │  Band 1: F G Q   Band 2: F G Q ...            │   │
│   ┌─ Dynamics ───┐  └────────────────────────────────────────────────┘   │
│   │ Gate         │                                                      │
│   │ Compressor   │  ┌─ Pan ─┐  ┌─ Sends ────────────────────────────┐   │
│   │ (graph)      │  │       │  │ → Main:    fader     send tap pre  │   │
│   └──────────────┘  └───────┘  │ → Synth:   fader                   │   │
│                                │ → FX:      fader                   │   │
│                                │ → Aux 1:   fader  (when wired)     │   │
│                                └────────────────────────────────────┘   │
│                                                                          │
└──────────────────────────────────────────────────────────────────────────┘
```

Status: this workspace is **partly stubbed** until the firmware exposes
per-channel HPF / EQ / dynamics. The Sends matrix and Pan can ship today.
HPF and EQ have placeholders that read from `processing.shelf*`-style
addresses if available, otherwise show "wired pending firmware support."

The codec already has its own EQ (per-codec biquads) — that lives in
SETUP > Codec, not here. TUNE EQ is per-channel mixer EQ, which is a
different stage in the signal chain.

### FX (post-bus processing + diagnostics)

```
┌──────────────────────────────────────────────────────────────────────────┐
│  [ Bus FX ]  [ Main Processing ]  [ Spectrum ]                           │
├──────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│   Bus FX page:    chorus + reverb (existing fx-panel)                    │
│   Main Processing page:  shelf + limiter (existing processing-panel)     │
│   Spectrum page:  canvas spectrum (existing spectrum.ts)                 │
│                                                                          │
└──────────────────────────────────────────────────────────────────────────┘
```

### SETUP (rarely-touched)

```
┌──────────────────────────────────────────────────────────────────────────┐
│  [ Codec ]  [ Clock ]  [ System ]  [ Raw OSC ]  [ Serial ]               │
├──────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│   Codec page:      TAC5212 panel + ADC6140 panel (existing codec-panel)  │
│   Clock page:      tempo source, BPM, beats/bar, metronome               │
│   System page:     codec mute toggles, hostvol controls, hardware diag  │
│   Raw OSC:         existing raw-osc input                                │
│   Serial:          existing serial-console                               │
│                                                                          │
└──────────────────────────────────────────────────────────────────────────┘
```

## Mode toggle

Stored as `state.mode: Signal<'engineer' | 'musician'>` and persisted to
`localStorage.getItem('t-dsp.ui.mode')` on change.

| Behavior | Engineer mode | Musician mode |
|---|---|---|
| Default landing workspace | MIX | PLAY |
| Top-tab order | MIX, PLAY, TUNE, FX, SETUP | PLAY, MIX, FX, SETUP, TUNE |
| Persistent bottom strip | Fader mini-bank + Sel + main | Keyboard + synth quick controls + main |
| Long-press synth pill | Engine info menu | Engine info menu (same) |
| Default new-channel Sel target | First input (USB L) | None |

The **mode is a preference, not a permission**. Both personas can reach
every workspace and every feature. Mode just changes defaults and the
bottom strip.

## Sel (selected channel) state

Added to `MixerState`:

```ts
selectedChannel: Signal<number>;   // 0..channelCount-1, default 0
```

Behavior:

- Tapping a channel strip's Sel button writes to `selectedChannel`.
- The bottom strip's Sel indicator subscribes to this signal and renders
  the highlight on the corresponding mini-fader.
- TUNE subscribes to this signal and renders the appropriate channel's
  detail page.
- On Sel change, MIX optionally pulses a brief outline on the newly
  selected strip (200 ms) for visual feedback that the change was
  registered.

**Sel is local-only state** — it is not echoed to the firmware. (X32
firmware doesn't track per-client Sel either; Sel is a client-side
notion.)

## Sends-on-faders

A render mode of the existing fader bank, not a separate widget.

### State

Added to MixerState:

```ts
sofMode: Signal<boolean>;          // false = levels, true = sends-on-faders
sofBus: Signal<string>;            // 'main' | 'synth' | 'fx' | 'aux1'... (when wired)
```

Both are local-only state.

### Behavior in Levels mode (default)

- Each fader reads from / writes to `channels[i].fader` as today.
- Meter shows that channel's `peak` / `rms`.

### Behavior in Sends-on-faders mode

- Each fader reads from / writes to a per-channel send signal.
  Currently `state.ts` does not have a sends matrix; it has only the
  global `recSend: Signal<boolean>` per channel.
  - **Phase 4 firmware dependency**: extend `state.ts` to model sends
    `channels[i].sends: Map<BusId, Signal<number>>` once the firmware
    exposes per-channel send addresses (`/ch/NN/mix/Nn/level` style).
  - Until then, sends-on-faders is gated to the existing buses:
    Main (which is just `channels[i].fader` again — same as levels mode
    for Main bus), Synth-Bus (no per-channel send today, falls back to
    a single global `synthBus.volume` fader visible across the bank),
    FX (same — `mpe.fxSend`, `dexed.fxSend`, `neuro.fxSend`, etc., one
    fader per source channel where applicable).
- Meter continues to show channel level (not send level), so the user
  can see "the source is hot" while adjusting how much of it goes to
  the bus.
- Fader thumb gets a colored stripe matching the bus.
- Bus name appears in a corner of each strip.

### UI affordance order of operations

1. Tap **Sends-on-faders** toggle in MIX → mode flips.
2. Pick a bus from the bus picker (or tap the highlighted bus's Sel-like
   button).
3. Adjust faders. They drive sends to that bus.
4. Tap toggle again → return to Levels mode.

This matches the X32 idiom: hold a Bus's Sel → faders show that bus's
sends.

## Persistent bottom strip

The single most important navigation primitive. **Always visible**
across all workspaces. Two variants chosen by `mode`.

### Engineer variant

```
┌──────────────────────────────────────────────────────────────────────────┐
│  ◀ │ ▌ ▌ ▌ ▌ ▌ ▌ ▌ ▌  │ ▶          [Sel: Mic L]    ━━●━━ Main  ▌▌▌▌    │
│    │ mini-fader bank   │            current Sel     main meter+fader    │
│      USBL USBR LineL... ↑                                                │
│      8-channel page                                                      │
└──────────────────────────────────────────────────────────────────────────┘
```

- 8 mini-faders (height ~80 px, thumb ~60 px wide) so each fader is
  touch-large.
- Tap any Sel button → set `selectedChannel`. Optional Mode-controlled
  behavior: jump to TUNE, or stay on current workspace.
- Swipe left/right on the bank → next/prev page of 8 (channels 1-8,
  9-16, FX returns, etc.).
- Long-press on bank → expand to full-height "peek mixer" overlay (does
  not change workspace).

### Musician variant

```
┌──────────────────────────────────────────────────────────────────────────┐
│ [◀Oct▶] │  piano keys spanning the width                  │ Synth:Dexed▾  │
│                                                  Vol ●━━ │ Main ▌▌       │
└──────────────────────────────────────────────────────────────────────────┘
```

- Octave control on the left (current `keyboard.ts` already has this).
- Piano keyboard fills the width (existing `keyboard.ts` ported into a
  Solid wrapper).
- Right side: synth picker + active synth's volume + main meter+fader.
- Tapping the synth picker swaps the active synth without leaving the
  current workspace — useful when editing Beats while wanting to play
  a melodic sketch on top.

### Both variants share

- Main meter + main fader pinned right.
- The bottom strip has its **own** height (configurable; default ~120 px
  in Engineer mode, ~140 px in Musician mode for the keyboard).
- Pull-up gesture from the strip handle → expand to half-height (peek);
  pull-up again → full-height takeover. Same gesture downward dismisses.

## Gestures (vocabulary)

| Gesture | Action |
|---|---|
| Tap | Primary action (button click, fader-set-to, etc.) |
| Long-press (≥ 400 ms) | Context menu / fine-mode for sliders |
| Swipe ←→ on workspace tabs | Next/prev workspace |
| Swipe ←→ on bottom mini-bank | Next/prev fader page |
| Swipe ←→ on a fader strip | Reset to default (with confirmation flash) |
| Two-finger tap on a fader | Snap to unity |
| Two-finger drag | Fine mode (scale 0.1×) for that drag |
| Pull-down from header | Settings drawer |
| Pull-up from bottom strip | Peek mixer (Engineer) / peek synth (Musician) |
| Pinch | Currently unused; reserved |
| Double-tap on any control | Reset to default |

Gestures are implemented via Pointer Events (no library). For
multi-touch, track `pointerdown`/`pointermove`/`pointerup` with
`pointerId`. See [03-design-system.md](03-design-system.md) for
implementation guidance.

## Cross-workspace state summary

Signals that are global state (added to MixerState):

```ts
selectedChannel: Signal<number>;   // 0-based; default 0
mode: Signal<'engineer' | 'musician'>;
sofMode: Signal<boolean>;
sofBus: Signal<string>;
activeWorkspace: Signal<'mix' | 'play' | 'tune' | 'fx' | 'setup'>;
activePlayTab: Signal<'synths' | 'arp' | 'beats' | 'loop'>;
activeFxTab:   Signal<'busfx' | 'processing' | 'spectrum'>;
activeSetupTab: Signal<'codec' | 'clock' | 'system' | 'rawosc' | 'serial'>;
bottomStripExpanded: Signal<boolean>;
```

The active-tab signals replace the current ad-hoc `'mixer' | 'spectrum' |
'synth' | ...` string pattern in `main.ts`.
