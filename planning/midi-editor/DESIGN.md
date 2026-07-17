# MIDI Note Editor (piano roll) — Design

Status: **design** (2026-07-17). Author: agent, research + design run.
Goal: edit a **recorded MIDI loop** — keyboard on the left, note grid to the right,
notes you can move, resize and quantize to the grid. Touch-first (no hover, no
right-click). The visual/editing sibling of the home-page **MIDI loop recorder**
(`lib/TDspMidiLoop` / `@REC*` / the per-synth "MIDI LOOPER" tab).

This document is self-contained: it records what exists, the constraints the
recorded-loop data model forces on any editor, the round-trip protocol (the real
work), the note model + pairing algorithm, the touch interaction model, firmware/app
integration, the prior art surveyed, and a test plan.

**The headline:** the editor UI is the smaller half of this job. The firmware today
has **no way to read a clip out or write one back** — `@REC*` carries state and a
progress bar and nothing else. The round-trip has to be designed first.

---

## 1. What already exists (don't reinvent)

- **`tdsp::MidiLooper`** (`lib/TDspMidiLoop`) — beat-aware MIDI loop recorder + player,
  one per voice (`g_loop1`/`g_loop2` in `firmware/mix-kit/src/main.cpp:216-218`). Captures at
  the `tdsp::MidiSink` seam **downstream of the arp** (so it records the arp's *baked* notes),
  plays back into the synth sink directly (no arp re-entry). States `Idle/Armed/Recording/
  Overdub/Playing` = 0..4. **Gaps for us:** no public clip accessor, no event-level mutation
  API (`insert`/`insertLive` are private), no per-note delete (only whole-clip `clear()`),
  no serialization, no SD persistence. A recorded loop is RAM-only and dies on reboot.
- **`LoopClip` / `LoopEvent`** (`lib/TDspMidiLoop/src/LoopEvent.h`) — the data. Flat 8-byte
  events, 1024/clip (8 KB), fixed static array (no heap), kept sorted by `(tick<<3)|rank`.
  **This struct already IS a wire format** — see §4.
- **`tdsp::LoopPlayer`** (`lib/TDspMidiLoop/src/LoopPlayer.h`) — clock-driven replay. Its
  cursor is a **raw index into a shifting array**; `resyncCursor()` (`LoopPlayer.h:46-56`) is
  mandatory after any mutation of a playing clip. `MidiLooper::insertLive()`
  (`MidiLooper.h:191-195`) is the reference implementation.
- **Master clock** (`tdsp::Conductor`/`tdsp::Clock`) — `positionBeats()`, `beatsPerBar()`,
  `bpm()`. The looper reads meter from the clock, **not** a private copy (`MidiLooper.h:51-55`),
  so loop downbeats line up with drums/song/metronome. Time in a clip is 24-PPQN ticks
  relative to the loop's downbeat — **the grid the editor draws is literally this**. No
  parallel time system, no conversion layer.
- **File transport** (`firmware/mix-kit/CATALOG_TRANSPORT.md`) — `@READ=<path>` streams any SD
  file as `@FB`/`@FD`/`@FE` base64 frames (≤360 raw bytes/frame, fits one ~512 B BLE MTU),
  paced `delay(6)` on non-USB streams. `streamFile()` at `main.cpp:1742-1778`. Client side:
  `Transport.readFile()` (`transport.ts:44`). Governing philosophy, quoted: *"The device is a
  dumb file server. The client owns all semantics."* **We follow it.**
- **`@WB`/`tdsp::SdWriteReceiver`** — host→device raw file write, ~5 MB/s, CRC32-verified.
  **USB CDC only** — the ESP32/BLE relay is 115200 and can't carry a raw byte stream
  (`SdWriteReceiver.h:8`). So the phone cannot use it. This is why §4.2 exists.
- **`@REC*` protocol** (`main.cpp:2089-2115`) — `@RECV` (voice select), `@RECBARS` (per-voice
  length), `@RECSIG` (global meter), `@REC`/`@RECDUB`/`@RECCLR`/`@RECPLAY`. Replies echo.
  `@STATE` carries `rec:{v,bars1,st1,p1,bars2,st2,p2}` + `caps.rec`; `@RECP=` pushes live
  state/progress ~4×/sec (USB-only).
- **App UI** — the looper is **inline in `App.tsx`**, not a component: `recDeck(v)`
  (`App.tsx:1003-1022`, the per-voice action bundle) and `recRow(v)` (`App.tsx:1024-1050`, the
  rendered row), mounted as a `BodyTabs` tab gated on `caps.rec` (`App.tsx:1087-1092`).
  **That tab body is where the editor mounts.** State is one flat object
  (`App.tsx:316`) — status and progress only, zero note data.
- **`ArpStepGrid.tsx` / `arpSeq.ts`** — the repo's only extracted UI component, and the
  architectural template: fully controlled, holds no model state, pure model+codec in a
  **sibling-free** module. See §7.3 for what carries over and what doesn't.
- **`planning/audio-looper/DESIGN.md`** — the audio-domain sibling, and the style template
  for this doc.

## 2. Constraints that shape everything: the data model

Five properties of `LoopEvent`/`LoopClip` are non-negotiable. Every one of them will
produce a visible bug if the editor ignores it.

1. **Notes aren't notes.** There is no duration field anywhere. `LNoteOn` and `LNoteOff`
   are separate 8-byte events (`LoopEvent.h:20-45`). The editor **pairs** them by
   `(channel, d1)` to build rectangles and **re-splits** on commit. See §5.2.
2. **A note-off can precede its note-on, and that is correct.** A note held across the loop
   seam has its off stored at the **wrapped** tick (`LoopEvent.h:12-14`), which is what lets
   replay be a plain sorted scan with no pending-off carry (`LoopPlayer.h:11-13`). Naive
   `dur = off - on` yields a negative. These render as wrapping notes.
3. **The sort invariant is load-bearing.** `keyOf = (tick << 3) | loopTypeRank(type)`, with
   **releases and expression ordered before attacks at equal ticks** (`LoopEvent.h:32-36`) —
   that's what stops a wrapped note-off from landing after the next iteration's note-on.
   A rebuilt clip must re-sort under this exact key, or be fed through `insert()` per event.
4. **Mutating a playing clip requires `resyncCursor()`.** `insert()` shifts array elements
   under the player's raw index cursor. Skipping the resync desyncs playback for the rest of
   the iteration.
5. **1024 events, hard cap, silent drop.** `insert()` just returns `false` when full
   (`LoopEvent.h:69`). An editor that *adds* notes must surface headroom — and
   `eventCount()` (`MidiLooper.h:58`) is **not currently on the wire**.

Two more, less obvious:

6. **24 PPQN is coarse.** A 1/16 note = 6 ticks, 1/32 = 3, 1/32-triplet = 2. That's the floor
   on editor resolution and it bounds useful horizontal zoom. Fine for loop material; don't
   design a 1/64 grid.
7. **Expression is per-channel and absolute, not bound to notes.** Bend/timbre/pressure/mod/
   sustain are captured as standalone events at their own ticks, decimated by `ctrlGate()`
   (`MidiLooper.h:284-293`). **Moving a note does not move its expression.** v1 preserves
   expression verbatim in absolute time and documents the wart (§9, deferred).

## 3. Constraint: the app has no gesture stack

`app/tdsp-control/package.json` has **no `react-native-gesture-handler` and no
`react-native-reanimated`**. Adding either is a new native dependency → a new **EAS cloud
build** (no local Android toolchain; see `reference_eas_tdsp`) → friction on every dev
iteration until the dev-client is rebuilt.

RN's built-in **`PanResponder`** covers drag-to-move and drag-to-resize on both native and
react-native-web with **zero new dependencies**, and exposes `evt.nativeEvent.touches` /
`gestureState.numberActiveTouches` so a two-finger pinch is hand-implementable if needed.

**Decision (§9.6): v1 uses `PanResponder`; zoom is +/− buttons, not pinch.** Revisit
gesture-handler only if pinch proves essential in hardware testing. This keeps the editor a
pure-JS change shippable over Metro/OTA.

## 4. The round-trip — the actual work

### 4.1 Read-out: reuse the file transport, from memory

`@READ` already streams bytes to both surfaces with client reassembly written. We reuse the
**framing**, not the SD card:

- Refactor `streamFile(Print&, const char* path)` into `streamBytes(Print&, const char* name,
  const uint8_t* buf, size_t len)`; `streamFile` becomes a thin SD-backed caller. Same
  `@FB`/`@FD`/`@FE`/`@FERR` frames, same 360-byte chunk, same `delay(6)` BLE pacing.
- **`@RECDUMP=<v>`** → `streamBytes(reply, "mem:/loop<v>", (const uint8_t*)&clip, clipBytes)`.
- The app reassembles with the **existing** `readFile()` reassembler (generalized to accept a
  `mem:` pseudo-path, or a sibling `readBytes()` sharing the frame parser).

Cost: a `const LoopClip& clip() const` accessor on `MidiLooper`, one `@RECDUMP` case, and a
serializer. **No new protocol, no ESP32 change, works over BLE today.** A worst-case clip is
8 KB raw → ~11 KB base64 → ~24 frames → ~150 ms over BLE, instant over USB. Typical clips are
a few hundred events.

### 4.2 Write-back: `@WB` can't do it, so mirror the frames upstream

`@WB` is USB-only, so the phone is locked out. Add the **mirror** of §4.1 in the `@REC*`
namespace — same shape, opposite direction:

| Line | Direction | Meaning |
|---|---|---|
| `@RECLOAD=<v>\x1f<bytes>\x1f<crc32hex>` | client→dev | Begin: stop the player, clear the clip, stage for `<bytes>` |
| `@RECOK=<v>\x1f<bytes>` | dev→client | Accepted, stream may begin |
| `@RD=<v>\x1f<seq>\x1f<b64>` | client→dev | Data frame (≤360 raw bytes, mirrors `@FD`) |
| `@RECEND=<v>` | client→dev | Commit: verify CRC + count, re-anchor, resume |
| `@RECE=<v>\x1f<n>` | dev→client | Committed, `<n>` events live |
| `@RECERR=<v>\x1f<reason>` | dev→client | crc / overflow / seq gap / busy / timeout |

**Decode straight into the target clip — no staging buffer.** A second `LoopClip` is another
8 KB of DTCM we don't have. The app sends events **already sorted** under `keyOf`, so the
firmware appends via the `insert()` O(1) fast path and the sort invariant holds for free
(and `insert()` re-sorts anyway if the app ever lies). A failed transfer leaves a **truncated
clip**, which is recoverable: the app just re-sends. Guards: reject unless
`state ∈ {Idle, Playing}` (never mid-`Recording`/`Overdub`), and a stall watchdog like
`SdWriteReceiver`'s.

**Phase alignment across the swap:** `@RECLOAD` stops the player but **keeps `clipAnchor_`**;
`@RECEND` calls `player_.play(&clip_, clipAnchor_)`. Because the player derives its position
from `positionBeats()` rather than an accumulator, the loop resumes **at the correct grid
phase** — the swap costs a brief event dropout (≈150 ms over BLE, ~0 over USB), not a timing
break. Honest and simple; §9 defers granular edits.

### 4.3 Why not `.mid` on SD?

Tempting (the audio looper does exactly this with `@ALSAVE=` → `/loops/<name>.wav`), but SMF
is **lossy against this data model**:

- No natural home for `loopTicks`/`bars`/`beatsPerBar` (you'd need a tempo + time-sig meta
  event *plus* a convention).
- **It cannot express the wrapped note-off** (§2.2) — the one thing that makes seams work.
- And the write direction still hits the USB-only `@WB` wall.

The 8-byte struct is already a wire format. Dump it raw. (An **export**-only `.mid` for
sharing a loop to a DAW is a fine phase-2 feature — one-way, lossy-by-design, no round trip.)

### 4.4 On-wire format

Version the dump so the app can reject a mismatched firmware rather than misparse 8-byte
structs:

```
struct LoopClipHdr {        // 12 bytes, little-endian (both ends are ARM/x86 LE)
    uint8_t  magic[4];      // 'T','L','C','1'
    uint16_t count;         // event count
    uint16_t loopTicks;     // <= 65535 (8 bars @ 16/4 = 3072)
    uint8_t  beatsPerBar;
    uint8_t  bars;
    uint16_t reserved;      // 0
};                          // followed by count * LoopEvent (8 B each), sorted by keyOf
```

`LoopEvent` is 8 bytes with no padding on either end (`uint16,u8,u8,u8,u8,int16` — natural
alignment 2, size 8), so a raw `memcpy` round-trips. Add a
`static_assert(sizeof(LoopEvent) == 8)` and pack the header explicitly.

## 5. The note model — `loopClip.ts`

### 5.1 Module placement (a trap the repo has already been bitten by)

`loopClip.ts` **must be sibling-free**. A runtime value imported from a module that has
`.web`/`.native` siblings resolves back to the importing platform's file — this is what
killed `parseDxls` (`reference_metro_platform_resolution`), and it's exactly why `arpSeq.ts`
and `dxls.ts` exist as standalone modules. `loopClip.ts` follows them: pure model + codec,
no platform siblings, imported by both `PianoRoll.tsx` and both transports.

```ts
export type LoopNote = {
  id: string;        // editor-local identity (survives re-sort); NOT persisted
  tick: number;      // 24-PPQN, [0, loopTicks)
  dur: number;       // ticks, always > 0, may exceed the seam (see wraps)
  note: number;      // MIDI 0..127
  ch: number;        // 1..16
  vel: number;       // 1..127
  relVel: number;    // note-off release velocity (preserved, not edited in v1)
  wraps: boolean;    // tick + dur > loopTicks — rings into the loop top
};

export type LoopModel = {
  notes: LoopNote[];
  others: LoopEvent[];    // bend/timbre/pressure/mod/sustain/program — PRESERVED VERBATIM
  loopTicks: number;
  beatsPerBar: number;
  bars: number;
};
```

`others` is not optional. Dropping it on a round trip would **silently delete a performance's
MPE expression** the first time you nudge a note.

`LoopModel` is **source-agnostic by construction** — no field records where it came from, and
the editor must never branch on provenance. That is the seam §5.4 hangs the `.mid` importer
on, and it is the reason that importer is a deferred *feature* rather than a deferred
*rewrite*.

### 5.2 Pairing — the algorithm that matters

Two passes, because a wrapped note's off is seen *before* its on in sorted order:

```
pass 1: scan events in order
  LNoteOn  -> pending[(ch,note)] = ev            (a re-press of a held note: close the old one first)
  LNoteOff -> if pending has (ch,note):  emit { tick: on.tick, dur: off.tick - on.tick }
              else:                      orphanOffs.push(ev)      // its on is later = wrapped
  others   -> others.push(ev)

pass 2: leftover pending (ons with no off yet) pair with orphanOffs by (ch,note)
  emit { tick: on.tick, dur: off.tick + loopTicks - on.tick, wraps: true }

pass 3 (defensive): any still-unmatched on -> dur = loopTicks - on.tick
                    any still-unmatched off -> drop (log)
```

The recorder guarantees a matching off for every on (`forceReleaseHeld()`,
`MidiLooper.h:315-321`), so pass 3 should never fire in practice — but a truncated transfer
or a future firmware can produce one, and a piano roll must never render a negative-width
rect.

`encode()` is the inverse: split each note into `LNoteOn` at `tick` and `LNoteOff` at
`(tick + dur) % loopTicks`, merge with `others`, sort by `keyOf` (**mirror
`loopTypeRank` exactly** — releases and expression before attacks at equal ticks), serialize.

Round-trip identity is a testable property: `encode(decode(bytes)) === bytes` for any clip
the recorder produces. Make it a test (§8).

### 5.3 Quantize

Native units, no beat conversion:

```ts
const GRIDS = { '1/4': 24, '1/8': 12, '1/8T': 8, '1/16': 6, '1/16T': 4, '1/32': 3 };
quantizeStart(n, g) => ({ ...n, tick: Math.round(n.tick / g) * g % loopTicks })
quantizeDur(n, g)   => ({ ...n, dur: Math.max(g, Math.round(n.dur / g) * g) })
```

`Math.max(g, …)` is what prevents zero-length notes — a note-on and note-off at the same
tick would order off-before-on under `loopTypeRank` and produce a silent, undeletable-looking
artifact. **Minimum duration is 1 tick, enforced everywhere.**

Quantize applies to the **selection**, or to all notes if nothing is selected. Start-only by
default (a "Quantize ends too" toggle is cheap; ends-only is not a thing anyone wants).

### 5.4 Sources — the import seam

Editing an existing `.mid` **is a wanted feature** (deferred to phase 2, §10). It is designed
in now because retrofitting it later means rewriting the model; designing for it now costs one
rule. `LoopModel` is the canonical editor currency and every source is just a decoder:

```
    @RECDUMP bytes ──decodeClip──┐
                                 ├──▶ LoopModel ──▶ PianoRoll   (knows nothing about sources)
    .mid bytes ─────decodeSmf────┘         │
                                           ├──encodeClip──▶ @RECLOAD bytes
                                           └──encodeSmf───▶ .mid (export)
```

**The rule: the editor talks only to `LoopModel`.** No source-conditional code above the codec
layer, no `if (fromFile)` anywhere. Break it once and the seam is gone.

**Import needs zero new firmware.** `@READ` already streams any SD file to both surfaces
(`/songs/*.mid`, `/drums/*.mid`, a future `/loops/*.mid`), `decodeSmf` is pure TS, and
`encodeClip`+`@RECLOAD` is the path the looper editor already requires. So the whole feature is
`decodeSmf` + a file picker. That is a direct payoff of `CATALOG_TRANSPORT.md`'s "the device is
a dumb file server; the client owns all semantics."

#### The SMF mapping is already specified — port it, don't invent it

`MidiFilePlayer::dispatchEvent()`/`dispatchCC()`
(`lib/TDspMidiPlayer/src/MidiFilePlayer.h:310-341`) **already decided** how an SMF maps onto
the `MidiSink` convention — which is exactly what `LoopEvent` stores. `decodeSmf` must **mirror
it**, or an imported loop sounds different from the same file under the song player. That's a
consistency bug users would report as "the editor changed my song."

| SMF | `MidiFilePlayer` does | `LoopEvent` |
|---|---|---|
| note on / off | `onNoteOn/Off(ch, note, vel)` | `LNoteOn`/`LNoteOff`, `d2 = vel` |
| CC#1 | `onModWheel(ch, val/127)` | `LModWheel`, `d2 = round(val/127*255)` |
| CC#74 | `onTimbre(ch, val/127)` | `LTimbre` |
| CC#64 | `onSustain(ch, val >= 64)` | `LSustain`, `d2 = 0/1` |
| CC#101 / 100 / 6 | RPN 0,0 → `pbRange_[ch]` | **consumed, not stored** |
| pitch bend | `onPitchBend(ch, (v-8192)/8192 * pbRange_[ch])` | `LPitchBend`, `bend = round(semis*256)` |
| channel pressure | `onPressure(ch, val/127)` | `LPressure` |
| program change | `onProgramChange(ch, prog)` | `LProgram` |
| CC#120 / 123 | `onAllNotesOff(ch)` | `LAllOff` |
| everything else (expression, pan, …) | dropped | dropped |

**The bend-range trap.** `LoopEvent` stores **semitones**; an SMF stores **14-bit** plus a
*per-channel* range set by RPN 0,0 (`CC101=0, CC100=0, CC6=semis`), default ±2. `decodeSmf`
must track that RPN exactly as the firmware does — the firmware's own comment
(`MidiFilePlayer.h:75-78`) warns that many songs raise it to ±12 and *"ignoring that renders
bends at a fraction of their intended depth."* `encodeSmf` must invert it: pick a range, emit
the RPN, then scale. **Consequence: `.mid → LoopModel → .mid` is NOT byte-identical**, unlike
the clip path (§8.1). Don't claim or test for it.

#### Time conversion — the decisions import forces

1. **PPQN.** `tick24 = round(smfTick * 24 / smfPpqn)`. Lossy, and two events can collide on one
   tick. Accepted: 24 PPQN **is** the target grid, so import implies quantize (§2.6).
2. **Loop length.** An SMF has none. Derive it: the smallest of {1,2,4,8} bars that holds the
   content at the current `beatsPerBar`; otherwise the user picks a bar range. `loopTicks =
   bars * beatsPerBar * 24`.
3. **Tempo map: dropped.** A clip rides the master `Clock` and has nowhere to put tempo events.
   Offer "set master BPM from the file's first tempo" as a UI affordance (`@BPM=`), never a
   model field.
4. **Multi-track / multi-channel:** flatten to absolute time, merge-sort, then filter to a
   channel or import all — the 1024 cap decides.
5. **The 1024 cap.** A song will not fit (Scriabin is ~15.5k events). Import **fails loudly**
   with `N events, cap 1024` and offers a track/bar-range filter. **Never silently truncate** —
   that's the `insert()` failure mode (§2.5) surfacing as lost music.
6. **A note extending past the chosen loop end WRAPS** — `off = (on + dur) % loopTicks`, with
   `wraps: true` — matching how the recorder treats a deliberate trail (`MidiLooper.h:250`).
   Do not clip it to `loopTicks-1`.

Rule 6 has a useful consequence, and it corrects a claim made earlier in this design's
research: an SMF cannot *store* a wrapped note-off, but **importing a file whose note crosses
the chosen loop boundary CREATES one.** So `.mid` import is a legitimate way to synthesize the
hardest fixture case (§2.2) without performing a take — once the wrap rule above is what
defines it. It still isn't a substitute for a real recorder dump (§8.3), because it can't prove
the firmware and the codec agree about the bytes.

## 6. The editor UI

### 6.1 Layout

```
┌─ toolbar ────────────────────────────────────────────────────────┐
│ [Select][＋ Add][Erase]   Grid ▾ 1/16   [Quantize]  [↶]  [Commit]│
├──────┬───────────────────────────────────────────────────────────┤
│  C5 ▮│ ░░░░│░░░░│░░░░│░░░░│  ← bar lines, beat lines             │
│  B4 ▯│     │▓▓▓▓▓▓▓▓│     │  ← notes (absolute-positioned Views)  │
│  A4 ▯│▓▓▓▓ │        │  ▓▓▓│  ← playhead (a 2px View, @RECP-fed)   │
│ …    │                                                            │
├──────┴───────────────────────────────────────────────────────────┤
│ vel  │ ▍  ▎     ▊   ▍                              (lane)         │
└──────────────────────────────────────────────────────────────────┘
```

- **Keyboard rail (left, ~56 px):** white/black keys, C-octave labels, **tap to audition**
  the pitch on the live synth. It scrolls in lockstep with the grid's vertical scroll.
- **Grid:** vertical scroll for pitch, horizontal scroll for time, both plain RN
  `ScrollView`s. Bar lines heavy, beat lines light, subdivision lines only above a zoom
  threshold (drawing 24-PPQN lines at low zoom is noise).
- **Auto-range on open:** show only the pitch span the clip uses, padded to octave
  boundaries. Nobody wants to scroll 128 empty rows to find their 2-octave riff. Default row
  height 28 px (zoom 16–48); keys are wide, so a 28×56 target is comfortable enough by Fitts
  even though it's under the 44 px guideline for square targets.
- **Velocity lane (bottom, ~64 px):** vertical bars aligned to note starts; drag a bar to set
  velocity. Velocity **never** goes on the note rect — that's the universal convention across
  every touch DAW surveyed (§7.1), and it's the only way to make note-drag unambiguous.

### 6.2 Interaction: select-then-act, with an explicit tool palette

The research (§7.1) converges hard on one principle: **nothing is grabbable until it's
selected**, so hit-test zones never compete. GarageBand iOS is the proven model. We take it,
but make the modes an **explicit palette** rather than a latchable button — the palette *is*
GarageBand's lockable "Add Notes" made visible, and it removes create/select ambiguity
entirely instead of resolving it with a gesture.

| Tool | empty tap | note tap | note drag | handle drag |
|---|---|---|---|---|
| **Select** (default) | deselect all | select | move (snap to grid) | resize (right edge) |
| **＋ Add** | place a note at grid, length = grid, vel = 100 | select | move | resize |
| **Erase** | — | delete | — | — |

- **Resize handle appears only on the selected note**, at its right edge, with a **≥44 px hit
  slop** (`hitSlop` on the handle View, so the *visual* handle stays note-height but the
  *touchable* area doesn't). webaudio-pianoroll's 8 px edge is 5–7× too small for a finger —
  that alone disqualified it as a dependency.
- **Dead zone before committing to a drag:** `Math.hypot(dx,dy) > 8` px (raise the usual 4 px
  mouse threshold — fingers wobble). Below it, it's a tap.
- **Drag state machine:** `'none' | 'move' | 'resize' | 'create'`. Marquee/multi-select is
  deferred (§9).
- **Floating context bar** on selection (Delete · Duplicate · Velocity) — the touch
  replacement for right-click. Ships with the Erase tool as the redundant path.
- **Undo:** a bounded snapshot stack of `LoopNote[]` (the model is ≤512 notes; snapshotting
  is cheaper than a command log and can't drift). 20 deep.

### 6.3 Local edit, explicit Commit

Edits are **local to the app** until you press Commit. Three reasons, all load-bearing:

1. **The bridge throttle.** `TX_GAP_MS=20 ms`; burst senders must stay ≤25 msg/sec
   (`project_serial_bridge_throttle`). Live-syncing every drag frame would tail the queue
   instantly.
2. **The swap has a cost** (§4.2) — a ~150 ms dropout over BLE. Once per commit is fine; once
   per drag is not.
3. **You want the old loop playing while you edit.** It's the reference you're editing
   against. Commit swaps it on the next `@RECEND`.

So: `@RECDUMP` on open → edit locally → `@RECLOAD`/`@RD`/`@RECEND` on Commit. The Commit
button shows dirty state and event headroom (`n/1024`).

## 7. Prior art surveyed

### 7.1 Interaction models (the valuable part)

| Source | Pattern worth taking |
|---|---|
| **GarageBand iOS** | **Select-then-act.** Tap = select; handles appear only on the selected note; touch-hold empty + drag = marquee; **Add Notes is a latchable modal button**; floating menu → Cut/Delete/Velocity. The whole model. |
| **FL Studio Mobile** | Tap empty = create; tap note = floating Copy/Delete/Snap menu; velocity in a **separate drag-up panel**; snap short-tap toggles / long-press opens options. |
| **AudioKit/PianoRoll** (SwiftUI, MIT, stale) | Touch-first by design; wrong platform, read for hit-testing structure. |

Convergent, across all of them: floating context menu replaces right-click · modal/latchable
tools beat gesture overloading · handles only on selection · **velocity in its own lane** ·
long-press = "reveal more" · pinch-zoom is table stakes. Touch targets: Apple 44×44 pt,
Android 48×48 dp, NN/g ~1 cm (finger pads are 10–14 mm).

### 7.2 Codebases

| Project | License | State | Verdict |
|---|---|---|---|
| **[signal](https://github.com/ryohey/signal)** | **MIT** | active, 2.3k★ | **Best code source.** Real web piano roll, React+TS, WebGL canvas. Monolithic DAW, nothing on npm — but MIT means we can *copy* coordinate math / event model, not just admire it. |
| **[webaudio-pianoroll](https://github.com/g200kg/webaudio-pianoroll)** | **Apache-2.0** | pushed 2024 | **Best reference implementation.** ~800 readable LOC, zero deps. But it's a Web Component (Expo web only, and wrapping it fights Metro resolution) and its touch model is `touches[0]`-only with 8 px edges — the exact part we'd rewrite. Read it; don't depend on it. |
| **[AudioKit/PianoRoll](https://github.com/AudioKit/PianoRoll)** | MIT | stale (2022) | SwiftUI. Ideas only. |
| GridSound | ⚠️ **AGPL-3.0** | active | **Do not read closely.** Worst-case license for a product. |
| MidiEditor · LMMS · Ardour · Rosegarden | ⚠️ GPL | active | Interaction ideas only, at arm's length. |
| burns-audio-wam · theobourgeois/PianoRoll | ⚠️ **no LICENSE file** | — | No license = all rights reserved. Unusable. |
| react-piano-roll (dpren) · pixi-piano-roll | MIT | dead (6 yrs) | — |
| @minagishl/react-piano-roll | MIT | maintained | Falling-notes **visualizer**, not an editor. Wrong tool. |

**Verdict: build, don't vendor.** Vendoring fails on the platform axis before it fails on
anything else — nothing here runs on RN native, and the one usable library hands us a
mouse-era touch model we'd throw away. What we take is *design* (GarageBand) and *algorithms*
(signal, MIT).

### 7.3 In-repo: `ArpStepGrid.tsx`

**Take the architecture, not the rendering.**

- **Take:** the controlled-component + pure-model-module split (`ArpStepGrid` ↔ `arpSeq`) →
  `PianoRoll.tsx` ↔ `loopClip.ts`. The palette-mirroring convention (`K` ≈ `App.tsx`'s `C`)
  so the surface reads as one app. The preset/shape row pattern → grid + quantize presets.
  The `i % 4 === 0` downbeat emphasis → bar/beat lines.
- **Doesn't carry:** it's a **flex-wrapped 1-D list** of ≤32 fixed 44 px `Pressable`s
  (`MAX_SEQ_STEPS = 32`) that **tap-to-cycle** — no drag, no pan, no long-press, by explicit
  design (`ArpStepGrid.tsx:8-14`). A piano roll is a 2-D absolutely-positioned pitch×time
  plane with scroll/zoom on both axes. And `SeqStep` is `{degree, octave, velocity}` —
  **degrees into held notes, every step one grid unit long**. The piano roll's central object
  (pitch + start + **duration**) has no analogue there at all.

## 8. Test plan

1. **Off-target codec test (host, no hardware) — the real correctness check.** `loopClip.ts`
   is pure TS: assert `encode(decode(b)) === b` byte-for-byte over fixtures; assert the
   pairing pass handles (a) a plain note, (b) a **wrapped** note (`off.tick < on.tick`),
   (c) a re-pressed held note, (d) an orphan off, (e) a note at tick 0 and at `loopTicks-1`;
   assert quantize never yields `dur < 1`; assert the emitted order matches `loopTypeRank`.
2. **Firmware green build** with `teensy41_opll` (fits anywhere + GM-capable —
   `feedback_test_with_opll`), plus `teensy41_dexed_pool_nobt_voice2` for the voice-2 path.
3. **Loopback on hardware:** record a loop → `@RECDUMP` → `@RECLOAD` the **unmodified**
   bytes back → `@CRC`-style compare + confirm it still plays identically. This proves the
   round trip before any editing is involved. Do it over **both** USB and BLE.
4. **Overflow:** load a 1024-event clip; confirm `@RECERR=…overflow` rather than a silent
   drop, and that the app's `n/1024` readout gates the Add tool.
5. **Phase:** commit an edit while the loop plays next to a drum groove; confirm it resumes
   on the correct downbeat (not just "sounds fine" — watch `@RECP` permille against the
   groove).
6. **Guard:** `@RECLOAD` while `Recording`/`Overdub` must be rejected, not raced.
7. **Expression preservation:** record an MPE phrase (bend + pressure), open the editor,
   commit **without** touching anything, confirm the expression is bit-identical.
8. Flash caveat: COM4 held by `control.html`/serial monitor → write `web/release.flag=1`, or
   press PROGRAM.

## 9. Decisions (locked 2026-07-17)

1. **Whole-clip replace, not granular edits.** `@RECDUMP` on open, `@RECLOAD` on Commit. A
   per-event edit protocol needs stable event identity across an array that `insert()`
   reshuffles — real complexity for a ~150 ms saving on an explicit user action.
2. **Raw `LoopClip` bytes over the existing frame protocol.** Not `.mid` (§4.3), not JSON
   (~4× the bytes over a 115200 relay for zero benefit). Versioned by a 12-byte magic header.
3. **Decode straight into the clip; no staging buffer.** 8 KB of DTCM we don't have. A failed
   transfer truncates and the app retries.
4. **Local edit + explicit Commit.** Forced by the bridge throttle, and better UX anyway.
5. **Expression preserved verbatim in absolute time.** The editor shows notes only in v1, but
   `others[]` round-trips untouched. Moving a note does **not** move its bend — documented
   wart, not a bug, until §10.
6. **`PanResponder`, +/− zoom buttons, no pinch in v1.** Zero new native deps → no EAS
   rebuild → the whole editor ships over Metro. Revisit gesture-handler after hardware
   testing says pinch is essential.
7. **Explicit tool palette (Select / ＋ Add / Erase)** over gesture overloading, plus the
   GarageBand selection model underneath it. Redundant paths (Erase tool *and* a floating
   Delete) are a feature on touch.
8. **Mounts as a third `BodyTabs` tab** next to `MIDI PLAYER` / `MIDI LOOPER`
   (`App.tsx:1087-1092`), gated on `caps.rec` **and** a new `caps.recedit` bit (the dump/load
   commands are their own build-flag surface — a board can have the recorder without the
   editor).
9. **`LoopModel` is source-agnostic; a source is a decoder, not a mode** (§5.4). Editing an
   existing `.mid` is a wanted feature, deferred but **designed in**: it costs one rule now
   (no source-conditional code above the codec) and a rewrite later. `decodeSmf` **mirrors**
   `MidiFilePlayer::dispatchCC()` rather than inventing a second mapping.

## 10. Deferred to phase 2

- **Marquee / multi-select** (long-press empty + drag) and multi-note transforms
  (transpose, nudge, scale-duration).
- **Expression follows its note** — bind bend/pressure to notes on MPE channels so a move
  carries the performance with it. The right fix for §9.5.
- **Expression lanes** (draw/edit bend + pressure curves under the velocity lane).
- **Granular edit ops** (`@RECEV=` insert/erase/modify) for gap-free live editing, if the
  commit dropout proves annoying in practice.
- **`.mid` import — edit an existing MIDI loop** (`decodeSmf` + a file picker). Architecturally
  ready per §5.4, and it needs **no new firmware** — `@READ` already streams the file and
  `@RECLOAD` already lands the result. Deferred only because the recorder path must prove the
  round trip first, and because a from-scratch TS SMF parser (chunks, VLQ deltas, running
  status, tempo map, RPN bend range) is real work that shouldn't gate the editor. The
  scope-shaped question to answer when it lands: which files are targets? A purpose-made
  `/loops/*.mid` is a clean fit; a dense multi-track `/songs/*.mid` needs the track/bar-range
  filter of §5.4/5.
- **`.mid` export** (`encodeSmf`, one-way, lossy-by-design — §5.4's bend-range inversion) for
  sharing a loop into a DAW.
- **SD persistence** — `@RECSAVE=<name>` → `/loops/<name>.tlc`, mirroring `@ALSAVE=`. Cheap
  once §4.1's serializer exists, and it's what stops a good take dying on reboot.
- **Note preview on drag** (audition the pitch while moving), pending a spare synth path that
  won't fight the loop's own playback.
- **Pinch-zoom** via `react-native-gesture-handler` (costs an EAS build).
