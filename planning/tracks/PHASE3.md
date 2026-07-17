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

## P3.4 executable plan — 4 independent Dexed voices (decided: 2+2+2+2, own busses)
Branch `tracks-phase3-voices`. Generalize the pool header to **N synth voices** (`kSynthVoices`,
default preserves today: drumvoice=1 unified, voice2=2 split; NEW `teensy41_dexed_pool_4voice`=4).
8 engines / N voices, 2 engines each at N=4 (poly ~2 MPE / 4 normal per voice — thin, accepted).

**SynthBackendDexedPool.h (the intricate part — audio-verify by ear):**
- Graph: replace `dxpMixA/dxpMixB` (2× 4-in) with `dxpMix[N]` (each sums its 2 engine converters:
  dxpc[2i],dxpc[2i+1] → dxpMix[i]); `dxpTrim[N]` (dxpMix[i]→dxpTrim[i]); `dxpSum` 4-in (dxpTrim[i]→
  dxpSum(i)). At N<4 keep today's exact wiring (guarded) so drumvoice/voice2 are byte-identical.
- Sinks: `g_poolSink[N]` over `&g_pool[2i]` (2 engines, kPoolVpe). `g_synthSink`=&g_poolSink[0];
  main.cpp binds track i's sink = &g_poolSink[i].
- State → arrays: `g_synthInstrument[N]`, `g_curCart{Rel,Voice,Name}[N]`, `g_voiceVolPct[N]`.
  `loadInstrumentRange`/`pickCartVoiceRange` already take (start,count) → call with (2i,2). Per-voice
  `synthSetInstrumentV(i,idx)` / `synthPickCartVoiceV(i,...)` / `synthSetVoiceVolV(i,pct)` fold the
  existing voice-1/voice-2 twins into one indexed family. `applyPoolVols`: dxpTrim[i].gain =
  replayGain[i]·userVol[i]; slot-3 = makeup. Audition/ClipProbe tap dxpSum; `synthAuditionTrim()` →
  &dxpTrim[currentAuditionVoice]. Retire the runtime @VOICE2 split toggle at N=4 (fixed 4-way).
**main.cpp:** `g_player[N]`, `g_arpFilter[N]`, `g_kbdRouter[N]` (or 1 owner-routed), `g_tracks[N]`;
bind + `trackWireSetup(g_tracks[i])` in a loop (already generic). @STATE tracks[] loops 0..N-1 (already
data). @TRK<i>.* already index-routes. Keyboard owner: which voice gets live MIDI (generalize the
Voices-2 owner switch to N). Recorder loops per voice (g_loop[N]). MPE: each g_poolSink[i] independent.
**Env:** `teensy41_dexed_pool_4voice` = pool + `-D TDSP_SYNTH_VOICES=4` (+ drumvoice + serial as fits).
**Verify:** green all envs; serial: @TRK0..3.PLAY each drives its own voice, @STATE tracks[]=4 synth+drum;
USER audio-tests balance/timbre/MPE/no-clip before merge.

## P3.3 executable plan — data-driven app cards (decided: yes)
Build the synth+drum card sections from a `trackDefs[]` derived from `@STATE tracks[]`: one
`<TrackCard>` per entry, reusing the existing components indexed by i (deck[i]=makeSongDeck with
`@TRK<i>` wire, arpSlot[i], voiceBrowser target=i). New firmware voice → new tracks[] entry → new
card, no app edit. Needs @TRK extended to cover voice-select + full arp params (P3.2 did transport
only) — add `@TRK<i>.DXPICK=`, `@TRK<i>.ARP<PAT|RATE|OCT|LATCH>=`. tsc-verified here; USER UI-tests.

## Non-goals (unchanged)
Not unlimited synths — a bounded, configurable slot set by RAM/CPU. Not a DSP-engine rewrite. Not a
big-bang cutover — each Pn independently green + shippable (`teensy41_opll` + `..._voice2`).
