# Tracks — one abstraction for every voice, and drums

## Context / why now

Across this work the same class of bug kept reappearing — the downbeat dropping, a running
player going silent, a launch stalling the beat — and every instance traced back to the *same
root*: **drums (and, before it was hardened, Voice 2) are special-cased instead of being peers
of Voice 1.** Each special case re-implemented start/stop/sync/loop slightly differently, and
each divergence was a bug.

Meanwhile Voice 1 and Voice 2 are already near-clones. The app has *already* collapsed their
UI (`makeSongDeck`, shared `handleArpLine`, the reused synth browser); the firmware has two
almost-identical `g_player`/`g_player2 → g_arpFilter*/→ g_synthSink*` chains. The pattern wants
to be generalized.

**Goal:** a single **Track** abstraction — *MIDI player + looper + arp + a synth engine binding
+ a mixer strip + its state/commands* — instantiated N times. **Drums become just another Track**
(a drum-capable engine playing a looping channel-10 MIDI file). Every track obeys one transport
contract, so the bug class we've been whacking disappears *structurally*, not patch-by-patch.

Related prior work this builds on: [[project_synth_slots]] (ISynthSlot/SynthSwitcher),
[[project_master_clock]] / [[project_tick_sync_playback]] (the Conductor grid), the
`planning/synth-voices-2/` split, `planning/audio-looper/`, and the UI-rebuild epic.

---

## The Track abstraction

A **Track** owns the full per-voice stack. One definition, N instances:

```
Track {
  MidiFilePlayer  song;      // its own @SONG feed (a .mid, looping or one-shot)
  MidiLooper      looper;    // its own live-capture loop (optional per build)
  ArpFilter       arp;       // its own arp (bypassed = pass-through)
  MidiRouter      router;    // live-MIDI entry (keyboard owner, DIN/app fan-in)
  MidiSink       *sink;      // the engine binding (see slots) — where notes land
  MixStrip        strip;     // level / ReplayGain trim / mute → the track's sub-bus
  TrackState      state;     // voice selection, arp params, loop/sync flags, level
}
```

Routing is uniform for **every** track:

```
  song ─┐
looper ─┤→ arp ─→ sink(engine) ─→ strip ─→ mixbus ─→ limiter ─→ out
 live ─┘
```

- **Voice 1** = `Track[0]`. **Voice 2** = `Track[1]`. **Drums** = `Track[k]` whose `sink` is a
  drum-capable engine and whose `song` is a ch10 loop. No `drumStartPath`, no synchro special
  case, no ch10 masking gymnastics — drums start/stop/loop/sync through the *same* code path.
- The keyboard has exactly one owner at a time; ownership = "this track's router receives live
  MIDI" (today's Voices-2 keyboard-owner switch, generalized to N).

---

## Slots & engines (the real constraint)

"A full synth per track" is the right *concept* but is bounded on a Teensy 4.1. The model is
**a fixed set of track slots, each bound to an engine (or an engine-window)** — not unlimited
synths. Two engine kinds already exist and both fit the model:

1. **Shared pool (Dexed).** The 8-engine pool (`DexedPoolSink`, `setEngineCount()` windows) is
   *already* divided 4/4 for Voices 1&2. Generalize the window into **up to 4 slots** over the
   8 engines — e.g. `2+2+2+2` (four 4-voice Dexed tracks) or `4+2+2`, etc. Polyphony per slot =
   engines × `voicesPerEngine`. The split ratio becomes a configurable table, not a constant.
2. **Parallel engines.** OPLL / TSF / SF2 / Plaits / Rings are separate audio objects on their
   own mix bus, each a slot.

**How many instances of an engine can coexist depends on WHICH resource that engine spends** —
this is the key sizing rule:

- **FM / synthesis engines are CPU-bound, RAM-cheap → multiple instances are fine.** OPLL (FM,
  no sample data) can be instantiated **more than once** — several OPLL slots (drums on one, a
  chiptune lead on another) cost CPU, not RAM. Same for the Dexed windows, and Plaits/Rings/VA.
  The cap here is DSP headroom.
- **Sample / soundfont engines are RAM-bound → instance count scales with (PS)RAM.** TSF/SF2
  hold sample data, so today a build ships **one**. On a **PSRAM-heavy core** (see
  [[project_core_opi_psram]] — the 32–64 MB OPI PSRAM roadmap) **two soundfont engines at once**
  becomes feasible (e.g. a GM piano track + a separate sampled-drums track), each with its own
  loaded font. The cap here is sample RAM, not CPU.

**Reference target config** (count stays configurable per board): **~4 Dexed slots** (dividing
the pool, fewer voices each — `2+2+2+2` or `4+2+2`) **+ one or more OPLL slots** (FM, so multi-
instance is cheap). Drums bind to an OPLL slot (rhythm mode) or a GM engine's ch10. On a PSRAM
core, add **up to two soundfont slots**. Slot count is thus **board-dependent**, gated by the
spending resource: DSP for FM, sample-RAM for soundfonts.

A **slot** is therefore `{ engine-kind, engine-or-window, polyphony, cost{cpu,ram}, caps(melodic/
drums/MPE) }`. A Track binds to a slot. Not every build compiles every backend, and RAM decides
how many sample-engine slots exist — so slot availability is a **build + board + runtime** fact
reported to the app (extend today's `caps`).

### Build-time engine inventory (per-type counts are build flags)

Teensy Audio objects and their `AudioConnection`s **must be statically declared at compile time**
— you cannot `new` an engine at runtime. So the *inventory* (how many engines of each kind, and
their wiring into the mix bus) is fixed by the build; only the **slot→track binding** is runtime.

The build therefore declares a **count per engine type** via flags, replacing today's
"one mutually-exclusive backend" selection:

```
-D TDSP_DEXED_ENGINES=8     ; N Dexed FM engines (the pool; windowed into slots)
-D TDSP_OPLL_ENGINES=2      ; N OPLL FM instances (FM = cheap to multiply; drums + chiptune)
-D TDSP_TSF_ENGINES=1       ; N TinySoundFont engines (RAM-bound; 2 needs PSRAM)
-D TDSP_SF2_ENGINES=0       ; N AudioSynthWavetable/SF2 engines
-D TDSP_PLAITS_ENGINES=0    ; N Plaits, etc.
```

- The build statically instantiates that many engine objects + the fixed `AudioConnection`s from
  each to its sub-bus (macro/`X-macro` expansion or a small codegen keeps the graph in sync with
  the counts — an engine array is easier to template than the connections).
- A build config is only valid if it fits **both** budgets (DSP for FM, sample-RAM for
  soundfonts); envs are named presets of these flags (e.g. `..._4dexed_2opll`,
  `..._psram_2sf2`). The default keeps today's shape (`TDSP_DEXED_ENGINES=8` split 4/4).
- At boot the firmware publishes the compiled inventory (kinds × counts) in `caps`; the app
  shows exactly the slots this board actually has and lets the user bind tracks to them.

So: **build flags = the engine inventory; runtime = which Track uses which slot.**

---

## The transport contract (this is the whole point)

Every track goes through **one** launch/tick path, encoding the timing discipline hardened this
session so it can never drift back into per-feature bugs:

1. **Master clock is the metronome.** The Conductor clock is the one grid; a track never
   re-zeroes it while anything else runs (only an idle transport defines the downbeat).
2. **Quantized launch, from the top.** Starting a track while the clock runs arms it for the
   **next bar downbeat** and starts it **from its first event** (`setSyncedMode(..., anchorAtNow)`),
   never mid-phase. Idle transport → define the downbeat and start now.
   - *This is exactly what fixes the "drums start 2 beats early and the loop clips its first
     beats" problem* — drums, as a track, launch on the downbeat like everything else. (Today
     drums start immediately/non-quantized; noted, not separately patched — the refactor makes
     it moot.)
3. **No blocking work on the beat.** A track's song is **pre-loaded when armed** (off the beat),
   so the bar-edge fire is `play()` + sync only — never an SD parse on the downbeat.
4. **Tick before launch.** In `loop()`, already-running tracks `tick()` (and dispatch their
   downbeat) **before** any launch fires; the just-launched track is re-ticked so it still lands
   its own downbeat in the same iteration. No launch can ever steal a running track's downbeat.
5. **Loop ⟂ Sync.** Loop (repeat vs stop-at-end) and grid-lock are independent per track; a
   synced one-shot plays once, grid-locked, then stops (`tickSynced` honours `loop_`).

Encoding these as **properties of the Track system** (not scattered `if`s) is the design's main
payoff.

---

## App

Componentize a **TrackCard / TrackPage** (the app has most pieces already):

- Tabs: **Player** (song select, level, end-mode/loop) + **Looper** (record/overdub) — reuse
  `makeSongDeck` + `recRow`.
- **Synth/voice selector** — reuse the folder browser (`voiceBrowserBody`), parameterized by
  track. A **drum track** swaps the melodic voice browser for a **kit selector** (or unify:
  "instrument" = a GM/drum patch).
- **Arp** per track — reuse `arpBody`/`handleArpLine`.
- **Mixer strip** — level/mute (today's `songVol`/`voice2Vol` generalized).
- N tracks → N cards, ordered under the existing menu>submenu nav. The global transport bar
  (Play/Stop/Mute/BPM) already sits above all of them.

State/protocol: generalize `@SONG*`/`@ARP*`/`@VOICE2*`/`@DRUM*` into a **track-indexed** command
family (e.g. `@TRK<n>.SONG=`, or keep terse aliases). `@STATE` reports a `tracks[]` array; the
app renders cards from it. Keep the relay verbatim (ESP32 BLE/WiFi forward `@`-lines untouched).

---

## Phased migration (never leave `master` broken)

- **Phase 0 — Foundation (DONE this session).** Metronome = master transport; quantized
  from-top launch; pre-load; tick-before-launch; loop⟂sync. This is the contract Tracks formalize.
- **Phase 1 — Track struct, no behavior change.** Introduce `Track`; migrate Voice 1 → `Track[0]`,
  Voice 2 → `Track[1]` over the existing Dexed split. Prove parity (same @STATE, same audio).
- **Phase 2 — Drums as a Track.** Bind a drum track to the OPLL (or GM ch10) slot; route its ch10
  loop through the Track launch path. **Retire `drumStartPath`/synchro/ch10-mask specials.** This
  is where the drum-timing flakiness dies for good.
- **Phase 3 — N slots + config.** Turn the 4/4 split into a configurable slot table (up to 4
  Dexed + OPLL); app renders `tracks[]` cards; per-track keyboard-owner + mixer strip.
- **Phase 4 (optional) — Per-track engine assignment.** Let a slot's backend be chosen (Dexed
  window / OPLL / TSF) where the build compiles it in.

Each phase is independently shippable and green-buildable (`teensy41_opll` +
`teensy41_dexed_pool_nobt_voice2`).

---

## Open decisions (to settle before Phase 1)

1. **Engine inventory flags + default env** — settle the `TDSP_*_ENGINES=N` flag set and the
   named preset envs (e.g. `..._4dexed_1opll`, `..._psram_2sf2`); and whether the Dexed-pool
   window split (`2+2+2+2` vs `4+2+2`) is runtime-reconfigurable or fixed per env. Includes the
   macro/codegen approach for statically wiring N engines' `AudioConnection`s.
2. **Command namespace** — track-indexed `@TRK<n>.*` vs keeping terse per-feature aliases +
   an index. (Affects the ESP32 relay and the app transport layer, but both are verbatim so
   it's mechanical.)
3. **Mixer topology** — per-track sub-bus → master limiter → out. Confirm each track gets its
   own trim/level node (the Dexed `dxpTrimA/B` pattern generalized) vs a shared bus.
4. **Drums identity** — is a drum track just "a track whose patch is a kit + MIDI is ch10," or
   does it keep a distinct card style (kit browser, groove picker)? Recommend: same Track,
   different *patch type*.
5. **Resource budget (two axes)** — measure and publish per-slot cost on BOTH axes so the slot
   table can be sized per board: **DSP %** (bounds how many FM/synthesis slots — Dexed windows,
   OPLL×N, Plaits/Rings) and **sample RAM** (bounds how many soundfont/sample slots — 1 today,
   2 on a PSRAM core). The runtime should refuse/grey a slot config that exceeds either.

---

## Non-goals

- Not unlimited synths — a fixed, configurable slot set bounded by RAM/CPU.
- Not a rewrite of the DSP engines — Tracks reuse existing backends (Dexed pool, OPLL, TSF, SF2);
  the reusable part is the per-track *stack + transport discipline*, not the synthesis.
- Not a big-bang cutover — phased, parity-checked, always shippable.
