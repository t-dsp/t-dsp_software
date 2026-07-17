# Tracks — Phase 3: N slots + data-driven track cards

## Where we are (end of Phase 2)
- Firmware: `g_tracks[]` (Voice 1/2) + `g_drumTrack`, all sharing the unified `trackPreload/trackFire/
  songStop/trackLoopTick/trackWireSetup` family. Drums are a full Track peer (quantized launch).
- App: THREE cards (Synth A, Synth B, Drums), each **hand-instantiated** but built from ONE reusable
  deck (`makeSongDeck`/`playerSongBody`) + arp component. Wire is per-voice: `@SONG*`/`@SONG2*`/`@DRUMF`,
  `@ARP*`/`@ARP2*`. `@STATE` has scattered per-voice keys (`song`/`song2`/`drums`/`voice`/`voice2`/`arp`/`arp2`).

The Phase-1/2 goal ("one Track abstraction, N instances") is TRUE in the firmware helpers but the
**inventory is still hardcoded** — 2 synth voices + 1 drum, wired by hand, with per-voice statics and
per-voice wire commands. Phase 3 makes the inventory **data**: the firmware publishes what tracks exist,
the app renders a card per track, and the count becomes a build/board fact.

## The two halves of Phase 3

### A. Data-driven surface (this is the tractable, high-leverage first half)
1. **`@STATE` reports a `tracks[]` array** — one object per track describing it enough to render a card:
   `{ i, kind:"synth"|"drum", name, playing, sync, vol, hasArp, on }`. Additive alongside today's keys
   (the app migrates, the old keys retire later). `caps.tracks` = the count.
2. **Track-indexed wire aliases** — `@TRK<i>.SONGF=`, `@TRK<i>.SONGRESTART=`, `@TRK<i>.STOP`,
   `@TRK<i>.VOL=`, `@TRK<i>.ARPON=` … as thin dispatch over the existing per-voice handlers (index →
   `g_tracks[i]` / `g_drumTrack`). Keeps the ESP32 relay verbatim (still `@`-lines). Old `@SONG*` stay.
3. **App renders cards from `tracks[]`** — one `<TrackCard>` per entry, driven by `@TRK<i>.*`; a 4th/5th
   track becomes a firmware-config change, not an app edit. Reuses the deck/arp components already built.

Half A adds NO new engines — it generalizes the *plumbing* so the count is data. Low risk, all additive.

### B. Engine inventory (the deeper half — enables MORE real tracks)
Teensy Audio objects + their `AudioConnection`s are static, so the *inventory* is a build fact:
- Per-type COUNT flags: `-D TDSP_DEXED_ENGINES=N / TDSP_OPLL_ENGINES=N / …` replace today's single-backend
  selection. A macro / X-macro statically declares that many engines + wires each to its sub-bus.
- The Dexed pool windows into **up to 4 slots** (`2+2+2+2` / `4+2+2`) instead of today's 4/4; each window
  is a synth Track. OPLL/TSF/etc. are parallel-engine slots.
- Sizing rule (unchanged from DESIGN): FM/synthesis = CPU-bound (multi-instance fine); soundfont =
  RAM-bound (count scales with (PS)RAM). A config is valid only if it fits BOTH budgets.
- Boot publishes the compiled inventory in `caps`/`tracks[]`; the app shows exactly this board's slots.

Half B is a big refactor (per-voice statics → arrays, static-wiring macros, configurable pool split).
Do it AFTER Half A so the surface is already data-driven and a new engine just adds a `tracks[]` entry.

## Phased execution
- **P3.1** `@STATE tracks[]` inventory + `caps.tracks` (firmware, additive). ← START HERE
- **P3.2** Track-indexed wire aliases `@TRK<i>.*` (firmware, additive over existing handlers).
- **P3.3** App `<TrackCard>` rendered per `tracks[]` entry via `@TRK<i>.*` (replaces the 3 hand cards).
- **P3.4** Engine inventory build flags + configurable Dexed N-way split (adds real synth slots).
- **P3.5** Per-track mixer strip + keyboard-owner generalized to N.

## Open decisions (carry from DESIGN)
- Terse `@TRK<i>.*` vs keeping per-feature aliases + an index (P3.2 picks `@TRK<i>.*`).
- Pool split runtime-reconfigurable vs fixed per env (lean: fixed per env for P3.4, runtime later).
- Per-track sub-bus vs shared bus for the mixer (P3.5).
- Default env preset (`..._4dexed_1opll` etc.) once the engine-count flags exist.

## Non-goals (unchanged)
Not unlimited synths — a bounded, configurable slot set by RAM/CPU. Not a DSP-engine rewrite. Not a
big-bang cutover — each Pn independently green + shippable (`teensy41_opll` + `..._voice2`).
