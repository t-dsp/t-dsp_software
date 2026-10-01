# LinnStrument control panel — PLAN

A **Settings › LinnStrument** page in `tdsp-control` that shows whether a LinnStrument is plugged into
the Teensy's USB-host port and lets you read and change every LinnStrument setting from the app, using
the NRPN control surface the instrument exposes over MIDI. Nothing here changes how notes reach the
synths; it adds a *control* path back to the instrument.

Status: **plan only** (2026-10-01). Nothing built.

---

## 1. What the LinnStrument accepts (reference)

Source of truth: the LinnStrument firmware's MIDI handler
([`ls_midi.ino`, `receivedNrpn()`](https://github.com/rogerlinndesign/linnstrument-firmware/blob/master/ls_midi.ino))
and the manual's [panel settings](https://www.rogerlinndesign.com/support/linnstrument-support-panel-settings)
page. Everything below was taken from the firmware switch, so the numbers are exact.

**Transport.** A setting is an NRPN: `CC 99` = parameter MSB, `CC 98` = parameter LSB, `CC 6` = value
MSB, `CC 38` = value LSB (14-bit each, MSB·128+LSB), on any channel. **Per-split parameters are
0–99 for the LEFT split and the same number +100 for the RIGHT split.** Globals start at 200.
`NRPN 299` with a parameter number as its value makes the LinnStrument *send back* that parameter as
an NRPN message (same four CCs), which is how the page reads the current state. The LinnStrument's
own **MIDI I/O setting must be USB** (NRPN 234 = 1, the factory default) for any of this to reach it
from the Teensy; the Teensy has no DIN out.

| NRPN (L / R = +100) | Setting | Values |
|---|---|---|
| 0 | MIDI Mode | 0 One Channel · 1 Channel Per Note (MPE) · 2 Channel Per Row |
| 1 | Main channel | 1–16 |
| 2–17 | Per-note channels 1–16 enabled | 0/1 each |
| 18 | Per-row lowest channel | 1–16 |
| 19 | Bend range | 1–96 semitones (panel shows 2/3/12/24) |
| 20 · 21 · 22 · 23 | Pitch/X send · quantize · quantize-hold (0–3) · reset on release | 0/1 (22: 0–3) |
| 24 · 25 · 26 · 39 | Timbre/Y send · CC number · relative · expression type (0 poly pres, 1 chan pres, 2 CC74) | |
| 27 · 28 · 29 · 58 | Loudness/Z send · expression type (0 poly pres, 1 chan pres, 2 CC11) · CC number · 14-bit | |
| 54–57 · 59 | Y min/max CC value, Z min/max CC value, Relative-Y initial | 0–127 |
| 30 · 31 · 32 · 33 | Colours: main 1–11 · accent 1–11 · played 0–11 · low row 1–11 | see colour codes |
| 34 · 48 · 49 · 50 · 51–53 | Low Row mode 0–7 · X behaviour · CC for low row · XYZ behaviour · XYZ CCs | |
| 35 | Special | 0 normal · 1 arpeggiator · 2 CC faders · 3 strum · 4 sequencer |
| 36 · 37 · 38 | Octave (0–10 ⇒ −5…+5) · transpose pitch (0–14 ⇒ −7…+7) · transpose lights | |
| 40–47 | CC faders 1–8 CC numbers | 0–128 |
| 60 · 61 | Channel-per-row order (0 normal/1 reversed) · touch animation 0–14 | |
| 62–66 | Sequencer: toggle play · prev · next · select pattern 0–3 · toggle mute | triggers |
| 200 · 201 · 202 | Split active · selected split (0 L/1 R) · split point column 2–25 | |
| 203–214 / 215–226 | Note lights main / accent, one bit per chromatic note (C…B) | 0/1 |
| 227 · 253 | Row offset (0 none, 3–7, 8–10 = octave/guitar/no-overlap) · custom row offset 0–33 (⇒ −17…+16) | |
| 228 · 229 · 230 · 231 | Switch 1 · Switch 2 · Foot L · Foot R assignment | octave down/up, sustain, CC65, arpeggiator, alt split, both splits, tap tempo … |
| 239–242 | "Both splits" flag per switch | 0/1 |
| 248 · 255–262 | CC numbers for the CC65 / sustain switch actions | 0–127 |
| 232 · 233 · 244 | Velocity sensitivity 0–3 (low/med/high/fixed) · pressure sensitivity 0–2 · pressure aftertouch | |
| 249 · 250 · 251 | Velocity min · max · fixed value | 1–127 |
| 235 · 236 · 237 | Arp direction 0–4 · arp note value 1–7 · arp octave extension 0–2 | |
| 238 | Clock BPM | 1–360 |
| 243 | Load settings preset | 0–5 |
| 234 · 254 · 252 | MIDI I/O (0 DIN / 1 USB) · MIDI through · min µs between USB MIDI bytes 0–512 | |
| 245 · 246 · 247 | User-firmware mode · left-handed · note-lights preset 0–11 | |
| 263–270 | Guitar tuning, one MIDI note per row | 0–127 |
| 299 | **Query**: value = parameter number to read back | |

**Pad lights, directly by CC** (no NRPN): `CC 20` column (0 = control column, 1–25 play columns),
`CC 21` row (0–7), `CC 22` colour: 0 default · 1 red · 2 yellow · 3 green · 4 cyan · 5 blue · 6 magenta
· 7 off · 8 white · 9 orange · 10 lime · 11 pink. `CC 23/24` store/clear the custom light pattern.
`CC 9–12` and `CC 13` only matter in user-firmware mode (not used here).

What is **not** controllable this way: the Actions column (calibrate, OS update, reset) and
"notes off" (use T-DSP's own PANIC). Preset *save* is a panel gesture; load is NRPN 243 or a
Program Change on the preset screen.

---

## 2. Where it fits today

- The Teensy already hosts the LinnStrument: `USBHost g_usbHost; MIDIDevice g_usbMidi(g_usbHost);`
  in `firmware/mix-kit/src/main.cpp`, callbacks `usbHostNoteOn/Off/CC/Pitch/Pressure` tag the source
  `SrcUsbHost` and hand off to the MIDI hub → MPE `MidiRouter` → the track sinks. **Nothing is ever
  sent back to the device**, and nobody checks *what* is plugged in.
- `@MIDIMODE=0|1` (`applyMidiMode`) switches T-DSP between GM and MPE; `TDSP_MPE_BEND_RANGE` (24, 48 on
  the kitchen-sink env) assumes the LinnStrument's per-note bend range matches.
- The app has a Settings submenu (`parent: 'settings'` sections: Connection, Device Wi-Fi, Firmware,
  TAC5212, MPE Monitor). The MPE Monitor page already watches LinnStrument traffic (`@MPEMON`).
- Control-line conventions: `@NAME` / `@NAME=` / `@NAME.VERB=` commands, `@STATE` JSON with `caps`,
  `ctrl` broadcasts to every lane (USB, web, ESP32), the app's `transport.ts` has one method per
  command family.

---

## 3. Design

### 3.1 Firmware: `lib/TDspLinn` (LinnStrument controller) + `@LINN` protocol

A small library that owns one `MIDIDevice&` and knows the LinnStrument dialect; `main.cpp` wires it
to `g_usbMidi` under a `TDSP_LINN` build flag (on wherever `TDSP_HAS_USB_MIDI_HOST` is).

**Detection.** `USBHost_t36::MIDIDevice` exposes `operator bool()` (a device is attached and
claimed), `idVendor()`, `idProduct()`, `manufacturer()`, `product()`, `serialNumber()`. Poll in
`loop()` (after `g_usbHost.Task()`): on an attach edge read the strings; `isLinn = product contains
"LinnStrument"` (log VID/PID on first sight and pin them in a constant once seen; the 128 and the
full-size differ only in column count, which the device does not report — expose it as a user
setting, default 25). Broadcast `@LINN={"connected":1,"name":"LinnStrument","vid":…,"pid":…}` on
attach/detach and carry the same object as `@STATE.linn` (+ `caps.linn` = build has the host port).
Any other USB MIDI device shows as connected-but-not-a-LinnStrument (page greys the controls, still
shows the name).

**Write.** `set(param, value)`: four `sendControlChange` calls (99/98/6/38) on channel 1, then
`send_now()`. Per-split helper `setSplit(side, param, value)` adds 100 for RIGHT. Rate-limit to a few
ms between NRPNs (the LinnStrument's USB parser is fine, but slider drags should coalesce: last value
wins, ≤ 50 writes/s).

**Read.** `query(param)` = `set(299, param)`. Replies arrive on the *input* side as the same CC
quartet. Add an NRPN assembler **in front of** the hub for the host source: CC 99/98 latch the
parameter, CC 6 the MSB, CC 38 completes a message → `onNrpn(param, value)` → update the shadow
table, broadcast `@LINN.V=<param>,<value>`. **Those four CCs are consumed, not forwarded** to the
synths (today they would land on the sinks as CC 6/38 data-entry noise). All other CCs (CC faders,
CC74 timbre, CC11, sustain) pass through unchanged. If the device turns out to answer on a channel
other than 1, the assembler is per-channel anyway.

**Sync.** `syncAll()` queries every parameter in the table above (≈ 150 numbers incl. both splits),
paced ~5 ms apart (~1 s total); the page shows a progress bar and goes live as values land. `@LINN.SYNC`
runs it; it also runs automatically on attach (and on `@MIDIMODE` change, see 3.3).

**Shadow + protocol.**

| Command | Meaning |
|---|---|
| `@LINN?` | status + whole shadow table as one JSON (`{"connected":…, "v":{"0":1,"1":1,…}}`) |
| `@LINN.SYNC` | re-read everything from the device |
| `@LINN.GET=<n>` | query one parameter (reply comes as `@LINN.V=`) |
| `@LINN.SET=<n>,<v>` | write; the shadow is updated optimistically and the value re-read ~50 ms later so the page shows the device's clamped result |
| `@LINN.PRESET=<0..5>` | NRPN 243 then `SYNC` |
| `@LINN.LIGHT=<col>,<row>,<colour>` | CC 20/21/22 |
| `@LINN.LIGHTS=<pattern>` | bulk pad lighting (see 3.3) / `@LINN.LIGHTS=clear` |
| `@LINN.COLS=<16|25>` | model hint (128 vs full) for the page's layout |

Broadcasts: `@LINN=` (connection), `@LINN.V=<n>,<v>` (one value), `@LINN.SYNC=<done>/<total>`.

### 3.2 App: Settings › LinnStrument

New section `{ id: 'linn', parent: 'settings', show: caps.linn }`, built like the other settings
pages, body = `LinnPanel`. Transport gains `linn(cmd: string)` (one method, `@LINN.<cmd>`), the line
handler parses `@LINN=`, `@LINN.V=`, `@LINN.SYNC=`, `@LINN?`'s JSON into a `linn` store
`{ connected, name, vid, pid, cols, syncing, values: Record<number, number> }`.

Page layout (phone-first, cards in the house style):

1. **Status header** — big dot + "LinnStrument connected (USB host)" / "No LinnStrument on the USB-host
   port" (and "USB MIDI device: <name>" for anything else) + Sync button with progress. Hint line when
   disconnected: *"Plug the LinnStrument into the T-DSP's USB-host jack; its Power/MIDI setting must be
   USB."* The Settings submenu card shows the same connected/disconnected status as its `value`.
2. **Split tabs: LEFT / RIGHT** (the per-split block, mirrored to the hardware panel columns):
   MIDI Mode (3-way) · Main channel (1–16) · per-note channel grid (16 toggles) · per-row lowest
   channel · Bend range (2/3/12/24 chips + a free 1–96 field) · Pitch/X (send, quantize, hold mode,
   reset on release) · Timbre/Y (send, type, CC, relative, min/max) · Loudness/Z (send, type, CC,
   min/max, 14-bit) · Colours (main/accent/played/low-row swatches) · Low Row (mode + CC/XYZ) · Special
   (normal/arp/CC faders/strum/sequencer; faders expand to 8 CC fields) · Octave / Transpose /
   Lights transpose steppers.
3. **Global**: Split on/off + split point · Row offset (chips incl. octave/guitar/no-overlap, custom
   offset field) · Note lights (12 chromatic toggles × main/accent, + the 0–11 lights preset) ·
   Switches: Switch 1, Switch 2, Foot L, Foot R assignment + "both splits" + their CC numbers ·
   Velocity (sensitivity + min/max/fixed) · Pressure (sensitivity, aftertouch) · Arpeggiator
   (direction, note value, octaves) · Clock BPM · MIDI I/O (read-only badge, with a warning if DIN) ·
   MIDI through · Left-handed · Guitar tuning (8 notes, shown when row offset = guitar).
4. **Presets**: six "Load preset 1–6" buttons (NRPN 243, then re-sync).
5. **Pad lights (optional, phase 3)**: "Light T-DSP's scale", "Clear", and a small 8×25 grid to paint.

Every control writes through `@LINN.SET` (optimistic), re-renders from `@LINN.V` echoes. Controls are
disabled while `!connected || syncing`. Values that the shadow has not received yet render as "—".

### 3.3 T-DSP integrations (what makes this more than a remote panel)

- **MPE handshake.** When `@MIDIMODE=1` is applied (or the page's "Match T-DSP MPE" button is pressed):
  set both splits to MIDI Mode 1 (channel per note), main channel 1, bend range =
  `TDSP_MPE_BEND_RANGE`, Loudness/Z = channel pressure, Timbre/Y = CC74 → exactly what the
  `MidiRouter` expects, so "Plaits bends less than Dexed"-style mismatches cannot happen. `@MIDIMODE=0`
  offers the GM-friendly inverse (one channel, bend 2). Opt-in toggle in the page ("Follow T-DSP MIDI
  mode"), remembered in the `@APP` store.
- **Tempo.** Push the Conductor's BPM to NRPN 238 when it changes (the LinnStrument's arp/sequencer then
  agree with the box). Toggle in the page.
- **Scale lights.** Light the pads of the key/scale the song or groove is in via the note-light bits
  (203–214), or paint arbitrary pads via CC 20–22 (e.g. flash the pad of the note the arp is playing).
  Phase 3.

---

## 4. Phases

| Phase | Deliverable | Effort |
|---|---|---|
| **0 — Detect & show** | `TDspLinn` attach/detach detection, `@LINN=` broadcast + `@STATE.linn` + `caps.linn`; Settings › LinnStrument page with the status header only; the Settings card shows connected/disconnected. Log VID/PID/product on jay-mint. | ½ day |
| **1 — Read/write core** | NRPN write + the inbound assembler (CCs 99/98/6/38 consumed), `@LINN.GET/SET/SYNC/?`, shadow table, `@LINN.V=` pushes; page shows the LEFT/RIGHT MIDI block + Global basics with live sync. Verify: change Bend Range on the device → page updates; change on the page → the panel's LED shows it. | 1–1½ days |
| **2 — Full panel** | Every parameter in §1 with proper widgets (chips, steppers, 16-channel grid, 12-note toggles, colour swatches); presets; MIDI I/O warning. | 1½ days |
| **3 — Integrations** | MPE handshake, tempo push, scale lights / pad painting. | 1 day |
| **4 — Polish** | `@APP`-remembered toggles, EAS update, FONTS.md-style doc (`planning/linnstrument-panel/README`), memory note. | ½ day |

Phases 0–1 are the ones to do first; they are independent of the rest of the roadmap.

---

## 5. Risks / unknowns to retire early

- **VID/PID and product string**: not in the firmware source I read; log on first attach (phase 0).
  Match on the product string, not the IDs.
- **NRPN 299 reply format**: `sendNrpnParameter()` in `ls_midi.ino` sends parameter MSB/LSB then value
  MSB/LSB — confirm the channel and ordering against a real device in phase 1 with the MPE Monitor.
- **CC leakage**: today CC 6/38/98/99 from the host would reach the synth sinks; the assembler fixes
  that, but check the LinnStrument's *CC faders* special mode still passes its CCs (1–8 by default).
- **Hot-plug**: `USBHost_t36` re-enumerates on plug; `g_usbHost.Task()` already runs in `loop()`. A
  LinnStrument on **DIN MIDI-in** plays but cannot be controlled (no DIN out on the board) — the page
  must say so rather than look broken.
- **Value clamping**: the device clamps out-of-range NRPNs silently; the re-read after each write shows
  the truth.
- **Two models**: LinnStrument 128 has 16 play columns; split point and pad painting must respect the
  `cols` hint.
- **Firmware versions**: the table is from current master; older device firmware may lack the newest
  numbers (253–270). Unknown parameters simply never answer — render "—".

## 6. Verification plan (jay-mint)

1. Phase 0: plug/unplug → `@LINN=` lines over USB (`esp_probe`), the app page flips state; Firmware
   page shows nothing new (no RAM impact worth noting).
2. Phase 1: `@LINN.GET=19` → `@LINN.V=19,24`; `@LINN.SET=19,12` → device panel shows 12 → `@LINN.V=19,12`;
   `@LINN.SYNC` → ~150 values in ≈ 1 s; MPE Monitor shows no CC 6/38/98/99 reaching the synths.
3. Phase 2: walk every control once; compare against the device's panel.
4. Phase 3: `@MIDIMODE=1` → the device switches to ChPerNote/bend 48 by itself; tempo change on the box
   moves the LinnStrument arp rate.
