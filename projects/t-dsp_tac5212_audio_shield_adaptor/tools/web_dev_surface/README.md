# t-dsp web dev surface — TAC5212

A small browser-based mixer that talks to the TAC5212 audio shield adaptor's
firmware over WebSerial. **Bench tool, not a product.** Per-board, intentionally
non-portable, deliberately undercooked.

This is one of two client surfaces for the OSC mixer foundation epic. The other
is a committed Open Stage Control preset (M13). They serve different users:

| Surface | Audience | When useful |
|---|---|---|
| Open Stage Control preset | Audio engineer running the board | After firmware is stable, when you want a real mixing UI |
| **This (web_dev_surface)** | Firmware dev mid-iteration | Any time the dispatcher is alive — moves a fader in Chrome and pokes the audio graph without launching anything else |

See [planning/osc-mixer-foundation/README.md](../../../../planning/osc-mixer-foundation/README.md) for the full epic context.

## Quickstart

Three ways to run this: **desktop app** (one command, recommended for day-to-day
bench work), **browser + bridge** (two terminals, useful when the dev surface
should share a host with OSC clients), or **static build** (for CI/preview).

### Desktop app (Electron)

```bash
cd projects/t-dsp_tac5212_audio_shield_adaptor/tools/web_dev_surface
pnpm install

pnpm app:dev       # dev — Vite + bridge + window, devtools open
pnpm app:build     # packaged app in release/win-unpacked/
```

`app:build` produces `release/win-unpacked/T-DSP Dev Surface.exe` (181 MB)
alongside its supporting DLLs/paks — the standard Electron distribution
shape. Double-click to launch; the whole `win-unpacked/` folder is
self-contained and can be copied anywhere.

The Electron main process boots the serial bridge in-process and opens a
window on the Vite dev server (dev) or the packaged `dist/` (prod). The
bridge auto-reconnects after a firmware upload, so no manual restart.

Build notes (for future changes):
- `npmRebuild: false` in the electron-builder config — `@serialport/bindings-cpp`
  ships N-API prebuilts that are ABI-stable across Node and Electron, so the
  native-module rebuild is unnecessary and only runs into Python 3.12+ /
  `node-gyp@9` / missing `distutils` on Windows.
- `win.target: "dir"` (not `portable` / `nsis`) because `makensis.exe` can't
  resolve `!include` paths longer than 260 chars, and pnpm's nested
  `.pnpm/app-builder-lib@.../...` layout blows past that limit. The unpacked
  directory is the distributable.

### Browser + bridge (two terminals)

```bash
# Terminal 1 — serial bridge (Node.js ↔ COM port)
pnpm bridge

# Terminal 2 — Vite dev server
pnpm dev
```

Open the URL Vite prints (default `http://localhost:5173`) in **Chrome, Edge,
Brave, or any other Chromium-based browser**. Click **Connect** and the UI is
live — the bridge handles the serial port, so there's no WebSerial picker.

**After a firmware upload** the Teensy reboots and re-enumerates USB, which
drops the bridge's COM handle. Restart the bridge (`Ctrl+C`, then `pnpm bridge`)
before reconnecting in the browser.

### Static build

```bash
pnpm build      # → dist/
pnpm preview    # serve dist/ for a local sanity check
```

## Browser support

The web client connects to a local WebSocket (`ws://localhost:8765`), so any
modern browser works — Chrome, Edge, Firefox, Safari, etc. The old direct-
WebSerial path was removed because Chrome's renderer-process USB handling
caused audio buzz on the composite device (see `usb_cdc_audio_contention.md`
in the project memory for the full investigation).

## What's in here

```
serial-bridge.mjs           Node.js WebSocket-to-serial bridge (pnpm bridge)
src/
  main.ts                  entry: wires WebSocket → demuxer → OSC ↔ UI
  style.css                dark theme, single file
  osc.ts                   minimal OSC 1.0 encoder/decoder (i/f/s/b + T/F/N/I)
  transport.ts             SLIP encoder + stream demultiplexer (SLIP frames vs ASCII text)
  state.ts                 tiny Signal class + mixer state model
  dispatcher.ts            mixer state ↔ OSC translation, address routing
  codec-panel-config.ts    TAC5212 panel descriptor (mirrors 02-osc-protocol.md)
  ui/
    connect.ts             Connect/Disconnect button
    channel-strip.ts       fader + mute + solo + peak/RMS meter
    main-bus.ts            main bus strip
    codec-panel.ts         tabbed enum/toggle/action renderer for the descriptor
    serial-console.ts      scrollback for plain-text side of the multiplexed stream
    raw-osc.ts             free-form OSC input field with auto-typing
    util.ts                shared formatters
```

No framework dependencies. The `pnpm install` cost is Vite (dev server +
TypeScript transpile), `serialport` + `ws` (for the serial bridge), and type
definitions. No React, no Svelte, no OSC library, no SLIP library — all
hand-rolled because the surface area is small enough that the dependencies
cost more than the code.

## How the wire protocol works

Two streams multiplexed on the same USB CDC connection, discriminated by the
first byte of each unit:

- **`0xC0` (SLIP END)** → start of a SLIP frame. Read until the closing `0xC0`,
  unescape, hand the payload to the OSC decoder.
- **Any other byte** → plain ASCII text. Accumulate until `\n`, hand the line
  to the serial console pane.

This matches what the firmware emits per
[planning/osc-mixer-foundation/03-transport.md](../../../../planning/osc-mixer-foundation/03-transport.md)
and [02-osc-protocol.md](../../../../planning/osc-mixer-foundation/02-osc-protocol.md#the-cli-escape-hatch).
The decoder is in `transport.ts` (`StreamDemuxer`).

OSC addresses follow the X32-flavored convention from
[02-osc-protocol.md](../../../../planning/osc-mixer-foundation/02-osc-protocol.md):
faders are normalized `0..1`, `mix/on` is X32-inverted (1 = unmuted), the
codec panel lives under `/codec/tac5212/...`. The dispatcher in `dispatcher.ts`
is the only place these conventions are encoded; widgets call typed setters
and never construct addresses themselves.

## Adding controls to the codec panel

Edit [src/codec-panel-config.ts](src/codec-panel-config.ts). Each tab has groups,
each group has controls, each control is one of `enum` / `toggle` / `action`.
The renderer in `ui/codec-panel.ts` handles all three kinds — no rendering code
needs to change when you add a leaf.

**Important:** keep this file in lockstep with the canonical
`/codec/tac5212/...` subtree in
[02-osc-protocol.md](../../../../planning/osc-mixer-foundation/02-osc-protocol.md).
The design notes in that document explain why specific leaves were dropped
(`/out/N/level`, `/out/N/drive`, `"se_mic_bias"`, `/adc/N/pga`,
`/micbias/gpio_control`). Don't reintroduce them here without revisiting the
underlying rules.

## What this surface intentionally doesn't have

- **EQ.** No curve display, no band controls. The raw OSC input field can poke
  `/ch/NN/eq/B/...` if you need to test handlers; a real UI is the OSC preset's
  job.
- **Scenes.** `/-snap/save` and `/-snap/load` can be triggered from the raw
  field. No scene browser.
- **Pan, sends, HPF, dynamics.** Not in the strip. Add via the raw field, or
  edit `state.ts` + `channel-strip.ts` if you genuinely need a widget.
- **Theming.** One dark theme.
- **Multi-tab layouts, layout persistence, keyboard shortcuts, drag-and-drop
  channel reordering, MIDI learn, accessibility audits, mobile breakpoints,
  i18n.** Not in scope. This is a testing ground.
- **Cross-browser support.** Chromium only by design. See above.

The rule for adding a feature: does the firmware-side dispatcher already have
the address? Then the raw OSC input handles it. Promote to a real widget only
when the friction of typing the address every time is hurting iteration speed.

## Relationship to firmware milestones

This tool is most useful starting at roadmap **M7** (OscDispatcher). Before
that, there's nothing on the firmware side to talk to.

- **M7 (OscDispatcher)** — fader/mute/solo/main work end-to-end. This is when
  the connect-and-mix loop first lights up.
- **M8 (MeterEngine + SubscriptionMgr)** — Meters: ON button starts streaming.
  The exact wire format of the `/sub` handshake will be confirmed in M8;
  `dispatcher.ts` `subscribeMeters()` is the one place to update if it changes.
- **M11 (Tac5212Panel)** — codec panel tab becomes wired to real registers.

The tool reaches "feature complete for its scope" alongside M11. It does **not**
block M13 (Open Stage Control preset) — both ship.

## Known limitations

- **Fader curve is a single-segment log approximation,** not the real X32
  piecewise curve. The dB readout next to each fader is rough. Replace
  `formatFaderDb` in `ui/util.ts` if you need exact display.
- **No reconnect-on-port-loss.** If the Teensy reboots mid-session (e.g. after
  a firmware upload), restart the bridge (`Ctrl+C`, `pnpm bridge`) and click
  Connect again in the browser.
- **Subscription wire format is provisional.** The `/sub addSub` argument shape
  in `dispatcher.ts` matches the convention documented in
  [02-osc-protocol.md](../../../../planning/osc-mixer-foundation/02-osc-protocol.md#subscriptions-follow-the-x32-xremote-idiom)
  but the exact types/order will be locked when M8 implements the wrapper.
- **Meter blob layout is provisional.** Per-channel `(peak, rms)` float32 BE
  pairs in declared channel order — placeholder until M8 publishes the
  authoritative spec.
- **No auto-typing for negative integers in raw field.** `5` parses as `i 5`,
  `5.0` parses as `f 5.0`, `-5` parses as `i -5`. If you want a negative float
  to be `f` rather than `i`, write `-5.0` or use explicit-typed mode
  (`/path f -5`).
