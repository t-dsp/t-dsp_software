# Tick-Synced Playback — DAW-Quality Loop Lockstep

**Status:** design/handoff. Not yet implemented.
**Goal:** replace the free-running millisecond player timing with **tick/beat-driven
playback locked to the one master clock**, so drum loops + loop songs + arp stay in
sample-grid lockstep with **zero cumulative drift** and **seamless loops** (no jitter/
latency at the seam), across **any meter** (4/4, 3/4, 5/4, 6/8, 7/8…).
**Hard constraint:** RAM is tight (dexed-pool boards ~74–144 KB stack; event buffers
already pushed to DMAMEM/OCRAM). **No large new allocations, no per-event array growth
in v1.** Must **coexist** with the existing ms engine so full songs with tempo maps do
not regress.

This is "Option B" from the drift research. Option A (bar-edge re-anchor of the ms
player) was the lighter alternative; the user chose B for DAW quality.

---

## 0. Problem statement (why they drift)

The drum loop (e.g. `03 Pop Backbeat`, native 118 BPM, 1 bar) and a loop song (e.g.
`13 Loop Pop Changes`, native 120 BPM, 4 bars) are played by the **same**
`tdsp::MidiFilePlayer`, which is a **free-running millisecond accumulator**, not locked
to the master clock:

- Events are integer-millisecond deltas (`deltaMs`), rounded ticks→ms at parse
  (`lib/TDspMidiPlayer/src/MidiSmfParser.h`).
- `tick()` advances a float accumulator by *real elapsed ms × speed* and loops at the
  event-stream end (`lib/TDspMidiPlayer/src/MidiFilePlayer.h:137-163`).
- Time source is `elapsedMillis clock_` — **1 ms resolution** (`MidiFilePlayer.h:220`).
- The `Conductor`/`Clock` (24-PPQN, `micros()`-interpolated) is a **separate** time base;
  `PlayerFollower` only reads `onBpm()` to set the speed ratio.

Three stacked drift/jitter sources:
1. **Loop length ≠ exact bar.** `round(ticks→ms)` at the native tempo, retimed to master,
   is not an exact bar (e.g. 1 bar @118 = 2033.9 ms → 2034 → +0.1 ms/loop ≈ 90 ms / 30 min).
2. **Different natives round differently** → the two players separate at their own rates.
3. **ms-resolution clock** → ±1 ms jitter at each seam.

Key fact: the Conductor **already fires `onBarEdge()` to followers every bar**
(`lib/TDspTempo/src/Conductor.h:57-61`), but `ITempoFollower::onBarEdge()` is an empty
default (`lib/TDspTempo/src/TempoFollower.h:40`) and `PlayerFollower` never overrides it —
the shared grid exists, nothing snaps to it.

---

## 1. Design principles

- **Single source of truth:** `tdsp::Clock` (24 PPQN internal, micros-interpolated phase),
  owned by the `Conductor`, is the only tempo/position authority.
- **Position-driven, not delta-accumulator:** the synced player reads the master clock's
  continuous beat position and dispatches events up to it. Two players reading the SAME
  position cannot drift apart.
- **Loop length in BEATS** (meter-agnostic integer), snapped to the exact musical length →
  drift-free wraps.
- **Global-grid anchoring:** loop phase = `fmod(masterBeat, loopBeats)`, so every synced
  player aligns to the shared bar/beat grid; downbeats coincide.
- **Coexistence:** ms engine unchanged for one-shot / tempo-map songs; tick engine for
  looping ("synced") players. Mode chosen at `play()`.
- **RAM-safe v1:** reuse the existing `MidiFileEvent[]` (deltaMs) unchanged — interpret
  event spacing in beats via the file's single native tempo. A handful of doubles/flags
  per player. No array growth, no reparse.

---

## 2. The math (canonical position model)

- **Master position in beats** (continuous, monotonic, micros-accurate):
  `songBeat(t) = beatCount() + beatPhase()`. Needs a Clock accessor that returns this as
  a `double` computed in one shot (avoid `beatCount()`/`beatPhase()` tearing across a beat
  edge). `beatPhase()` already interpolates within the current tick using
  `(_lastUpdateMicros - _lastTickMicros)/_measuredIntervalUs` (`Clock.cpp:164-180`).
- **Event spacing (v1, no reparse):** `eventBeatDelta_i = deltaMs_i × nativeBpm / 60000`.
  Walk incrementally (running beat cursor + `idx_`), don't re-sum.
- **Loop length:** `loopBeats` is the **exact musical length in quarter-note beats**,
  a `double` (need NOT be an integer). From `lastEventTick / filePPQN` at **parse**
  (exact, avoids the ms round), or the authored bar count for baked loops. Fallback for
  lossy/ms-derived lengths: `totalMs × nativeBpm / 60000` **snapped to the nearest 0.5
  beat** (eighth-note granularity) to absorb tick-quantization noise. Meter-agnostic —
  and, crucially, NOT rounded to a whole beat: a 6/8 bar = 3.0, a 7/8 bar = 3.5, a 5/8
  bar = 2.5. See §3 for why this matters musically.
- **Wrap:** `pos = fmod(songBeat, loopBeats)`. Dispatch the window `[prevPos, pos)` with
  wrap handling (if `pos < prevPos`, emit `[prevPos, loopBeats)` then `[0, pos)`).
  Drift-freeness comes from computing `pos` **absolutely** with `fmod` (not from an
  integer `loopBeats`), so any exact fractional length wraps without cumulative error.

Because tempo lives entirely in the clock, the synced player has **no `speed_`** and
**ignores the file's tempo** for playback rate; the file's positions are the musical grid,
the clock supplies wall-clock tempo. (This is why tempo-map songs stay on the ms engine —
see §7.)

---

## 3. Measure-count handling — the three grids (musical correctness)

Getting this musically right means **not conflating three different grids**. They are
computed from the same clock but answer different questions:

| Grid | Question it answers | Unit | Authority |
|------|--------------------|------|-----------|
| **Loop-wrap** | when does the loop repeat? | exact quarter-beats (`double`, may be fractional) | `loopBeats` per player |
| **Downbeat/bar** | where is beat 1 (the accent)? | integer quarter-beats | `Clock._beatsPerBar` (uint8) |
| **Metronome pulse** | where do you tap your foot / click? | musical beat (see §13) | derived from the meter |

- **"Beat" = quarter note** everywhere in `Clock` (24 PPQN / quarter). `initialBeatsPerBar`
  converts the time-sig `nn/2^dd → quarter-beats-per-bar` (4/4→4, 3/4→3, 6/8→3, 12/8→6).

- **Loop-wrap grid handles ANY meter** because `loopBeats` is an exact `double`
  (§2). Examples — all wrap drift-free:
  - 5/4, 1 bar → `loopBeats = 5.0`
  - 3/4, 2 bars → `6.0`; 4/4, 4 bars → `16.0`
  - **6/8, 1 bar → `3.0`** (three quarter-beats); 2 bars → `6.0`
  - **7/8, 1 bar → `3.5`**; **5/8, 1 bar → `2.5`** — these were *wrong* under the old
    "round to integer beats" rule and are now correct.

- **Downbeat grid is integer-only** — `Clock._beatsPerBar` is a `uint8_t` count of
  quarter-beats, so it is **exact for x/4 and for compound meters that land on a whole
  number of quarters** (6/8→3, 12/8→6, 2/2→4). It **cannot** represent a bar of 3.5 or
  2.5 quarters, so `initialBeatsPerBar` deliberately **falls back to 4** for 7/8, 5/8,
  9/8, etc. (see `MidiSmfParser.h:131-134`). Consequence to state honestly: a 7/8 loop
  **wraps correctly** (loopBeats 3.5) but its **bar-1 accent grid is approximate** (the
  Clock still counts 4/4 bars underneath). This is acceptable for v1 — do **not** claim
  "exact downbeats for any meter." Making 7/8 downbeats exact would require a
  tick-defined bar length in `Clock` (a `_ticksPerBar` instead of `_beatsPerBar`); note
  as a future item, out of scope for v1.

- **Compound-meter felt beat ≠ Clock beat.** 6/8 is *felt in 2* (the pulse is the
  **dotted quarter**), not in 3 quarters. The loop-wrap and downbeat grids don't care —
  but the **metronome does** (§13): clicking every Clock quarter in 6/8 gives the "in 3"
  subdivision, not the idiomatic "in 2" pulse. v1 metronome clicks Clock beats (correct
  for x/4); the compound dotted-quarter pulse is a documented phase-2 (§13).

- **Odd content:** `round`/snap `loopBeats` only via the §2 0.5-beat fallback (never to a
  whole beat). **Log** when `fmod(loopBeats, beatsPerBar) != 0` so a loop whose length
  isn't a whole number of the *current* bars is visible (its bar-1 rotates against the
  global bar grid — rare, acceptable). GMD grooves carry no time-sig meta (all report
  4/4); their exact `lastEventTick/division` length is still the right wrap value.

---

## 4. Files to change

### 4.1 `lib/TDspClock/src/Clock.h` + `Clock.cpp`
- Add `double positionBeats() const` — absolute, monotonic beat position = integer beats +
  intra-beat phase, computed from `_tickCount + micros` in one shot (reuse the `beatPhase`
  math but return `beatCount + frac`). Clamp intra-beat frac to `[0,1)`; guarantee
  monotonicity across the beat edge.
- (Optional) `double positionBars() const` for UI.
- RAM: none. Keep 24-PPQN tick emission (MIDI-clock contract, arp depends on it).

### 4.2 `lib/TDspMidiPlayer/src/MidiFilePlayer.h` (+ `.cpp` if you split it)
- Add synced mode:
  - Members (all in the player object, not per-event): `bool _synced; tdsp::Clock* _clock;
    double _loopBeats; float _nativeBpm; double _lastMasterBeat; double _evCursorBeat;`
    plus a couple of flags. ~48–64 B/player.
  - `setSyncedMode(tdsp::Clock* clk, double loopBeats, float nativeBpm)` / `clearSyncedMode()`.
  - `tick()`: branch — `_synced ? tickSynced() : <existing ms path unchanged>`.
  - `tickSynced()`: read `positionBeats()`; compute advance; walk events converting
    `deltaMs→beat` via `nativeBpm`; dispatch every event in the crossed window (handle
    catch-up of multiple events and the seam wrap); **seamless** (no all-notes-off at wrap).
  - `positionPermille()` from `_evCursorBeat / _loopBeats`.
  - `consumeLooped()` already exists — set on wrap.
- Do **not** change `MidiFileEvent` (stays 6 bytes) in v1.

### 4.3 `lib/TDspMidiPlayer/src/MidiSmfParser.h` / `MidiSmfFile.h`
- Compute exact loop length at parse: add `smf::initialLoopBeats(buf,len)` returning a
  `double` = `lastEventTick / division` (meter-agnostic; **exact fractional**, NOT
  integer-rounded — see §2/§3, this is what makes 6/8→3.0, 7/8→3.5 correct) OR extend
  `loadSmfFile(...)` to also output `double* outLoopBeats` alongside the existing `outBpm`.
  Apply the 0.5-beat fallback snap only on the lossy ms path.
- Baked songs (no parse): compute `loopBeats` from the authored bar count / `seq()` length.

### 4.4 `lib/TDspTempo` (`Conductor.h`, `PlayerFollower.h`, `TempoFollower.h`)
- `PlayerFollower`: for a synced player, tempo is the clock; make `setNativeBpm/onBpm`
  no-ops (or bypass the follower entirely for synced players). Optionally override
  `onBarEdge()` as a belt-and-suspenders resync/assertion (compare `_evCursorBeat` to the
  expected grid position; log if off).
- `Conductor`: no structural change; confirm `start()`/`running()` support "join without
  re-zero" (see §5).

### 4.5 `firmware/mix-kit/src/main.cpp`
- **Transport start policy:** call `g_conductor.start()` (zero the clock) **only when the
  transport is idle** (nothing playing). Subsequent synced players **join** the running grid
  (no re-zero). Add a helper `ensureTransportStarted()`.
- `drumStartPath()`: put the drum player in synced mode (`loopBeats` from the groove,
  `nativeBpm`); grid-anchored. It currently calls `g_conductor.start()` unconditionally —
  change to the idle-only policy.
- Song start: when **loop enabled + clock available**, use synced mode; else ms. Wire
  `loopBeats/nativeBpm`.
- `applyTempos()`: keeps setting the master BPM on the Conductor (the authority). For synced
  players it no longer scales `speed_` (they read the clock). Keep it for ms players.
- `@STATE`/logs: expose `loopBeats`, bar count, synced flag, `_evCursorBeat` for debugging
  (see §9). Keep drum velocity/kit/mute intact.

### 4.6 `firmware/mix-kit/src/test_songs.h` + `tools/gen_test_songs.py`  ⚠️ COORDINATE
- Loop songs 13–17 are authored bar-exact via `seq()`; give each a derivable `loopBeats`
  (add a field to `TestSong`, or compute from the `seq()` `end_beat`).
- **A concurrent session is actively editing these two files** (per-song native BPM +
  beat-locked authoring). Coordinate / rebase to avoid clobbering their work; do not sweep
  their uncommitted changes.

### 4.7 Metronome (new, build-flag-gated) — see §13 for the full spec
- **New:** `src/Metronome.h` (or `lib/TDspMetronome/`) — a pure master-clock consumer:
  in `loop()` reads `g_conductor.clock().consumeBeatEdge()`, fires a click, accents when
  `clock().beatInBar() == 0`. On/off flag. No tempo of its own (validates §1's single
  authority). Compiled only when `-D TDSP_METRONOME=1`.
- `firmware/mix-kit/platformio.ini`: new build flag `-D TDSP_METRONOME=1`, opted into the
  test envs first (`teensy41_dexed_pool_nobt_drumvoice`, `teensy41_opll`). Gated so
  slot-starved builds don't pay the mix-slot cost.
- `main.cpp`: instantiate the click generator, sum it into a spare/second mixer input
  (reuse the local **test-tone `AudioSynthWaveformSine_F32` slot** where present — it's a
  bring-up aid, not production audio), wire `@METRO`/`CMD.SET_METRO`, poll the metronome in
  `loop()`, expose state in `@STATE`. All under `#ifdef TDSP_METRONOME`.
- Shared control (`handleControlLine()`): `@METRO=0|1` (+ optional `@METROVOL=`). Add a BLE
  `CMD.SET_METRO` byte opcode (ESP32 firmware + app `src/tdspBle.ts`).
- App (`app/tdsp-control/`): `tp.metronome(on)` in `src/transport.native.ts` (relay
  `@METRO=`), `setMetronome` in `src/tdspBle.ts`, and a **Metronome card** in `App.tsx`
  near the Tempo card (§13). ⚠️ Expo pinned to v57 — read
  https://docs.expo.dev/versions/v57.0.0/ before touching app code (per app `AGENTS.md`).

---

## 5. Transport / start / launch semantics

- **One transport zero-point:** the first started player (drum or loop song) zeroes the
  Conductor (downbeat = now). Others join in-phase on the global grid.
- **Grid anchoring:** `pos = fmod(masterBeat, loopBeats)`. Joining mid-bar → joins mid-loop
  (drift-free, bar-aligned). Starting drum + song **together** → both from beat 0 (aligned
  downbeats — the common test case).
- **Launch-quantize (optional, coordinate):** the concurrent App.tsx work added a
  "launch-quantize toggle". If wanted, defer a synced start to the next bar boundary so it
  begins from its beat 0 on the grid. Design the hook; out of scope for v1 unless that
  toggle is ready.
- **Stop:** player stop releases held notes; the Internal clock is always-on (free-running)
  so leave it running and just mark players inactive.

---

## 6. Loop-seam correctness (no jitter/latency, no stuck notes)

- **Seamless wrap:** do NOT all-notes-off at the seam (preserves drum tails). Authored loops
  must **release before the seam** — `seq()` already does chord release-then-attack on the
  bar line; the drum barline-marker + one-shot semantics cover grooves. For sustained notes
  that legitimately cross the seam, track "notes on at wrap" and either re-trigger or hold;
  document the authoring rule.
- **Jitter bound = one `loop()` iteration** (foreground dispatch). State this hard limit.
  True sample-accurate would require ISR-scheduled note events (out of scope; note as
  future — the synth API is immediate/foreground by design).
- **Catch-up:** if `loop()` stalls, the clock fires catch-up ticks (capped at one bar,
  `Clock.cpp:92`). The player must dispatch **every** event in the crossed window in order
  (never skip), including multiple events and a seam crossing.

---

## 7. Fidelity upgrade path (phase 2, document only)

v1 keeps event positions at the 1 ms parse grid (inaudible for loop content on 8th/16th
grids). For finer fidelity:
- **Reparse synced content to canonical ticks:** repurpose `deltaMs → deltaTicks` (uint16,
  **same 6-byte struct → no RAM growth**) for synced buffers; keep `deltaMs` for tempo-map
  songs. Choose the field's meaning **per load** (synced=ticks+nativeBpm scalar;
  ms=deltaMs+resident tempo map only where needed). Do NOT try to make one shared array be
  both.
- Keep 24-PPQN for the arp (its `kRateTicks` table is in 24-PPQN ticks); use micros
  interpolation for fine position (already the v1 plan). Only bump internal PPQN if a real
  need appears, and then rescale the arp tables.

---

## 8. RAM budget

- Per player: ~5–7 doubles/floats + flags ≈ 48–64 B. Two players ≈ ~128 B. Clock: 0.
  Parser: 0 (compute-only). **No event-array growth** (v1 reuses `deltaMs`). Net < 0.25 KB,
  in existing globals/DMAMEM — **no DTCM/stack growth**.
- **Verify** with `arm-none-eabi-size -A` before/after on `teensy41_dexed_pool_nobt_drumvoice`
  and `teensy41_dexed_pool_jaymint_drumvoice_serial`: target **zero change to `.text.itcm`
  rounding** (the ITCM 32 KB-block cliff — see the ITCM boundary memory) and **< 256 B `.bss`**.

---

## 9. Testing / verification

- **Off-target unit tests** (parser + math are pure, no SD/Arduino): `loopBeats` for
  4/4, 3/4, 5/4, 6/8; position→event mapping; wrap window incl. seam crossing + catch-up of
  multiple events; `positionBeats()` monotonicity.
- **On-hardware drift probe:** add a `@SYNCPROBE` serial command that prints each synced
  player's `_evCursorBeat` and the master beat once/second. Run drum `03 Pop Backbeat` +
  song `13 Loop Pop Changes` for 10–30 min; assert **relative phase constant** (zero drift).
  Reuse the earlier loop-wrap-counting harness to confirm period stability.
- **Meter coverage:** add a 3/4 and a 5/4 test loop; confirm clean lockstep and seams.
- **Regression:** full songs (Schubert) still play with tempo map (ms path untouched); arp
  still steps; drums still follow `@BPM` live (change tempo mid-loop → wrap stays clean).
- **Boards:** local (COM4, `teensy41_dexed_pool_nobt_drumvoice`) has a keyboard for live
  tests; jay-mint (`…_jaymint_drumvoice_serial`, USB_SERIAL over SSH) is line-out/headless —
  good for the long-run drift probe. See jay-mint flash runbook memory (`U`→HalfKay,
  `teensy_loader_cli --mcu=imxrt1062`, retry once).

### Acceptance criteria
- Relative drift between drum loop and loop song **≈ 0** over 30 min (bounded only by double
  precision), vs the current ~90 ms/30 min.
- Loop seam within one `loop()` iteration; no audible gap/overlap; no stuck notes on
  loop/stop/start.
- Any-meter loops (4/4, 3/4, 5/4, 6/8, 7/8) **wrap** on the correct grid (fractional
  `loopBeats`); **downbeat accent** exact for x/4 + whole-quarter compound meters, 4/4
  fallback (documented) for 7/8/5/8/9/8.
- **Metronome** (when built with `-D TDSP_METRONOME=1`): on/off from the app card; clicks on
  every beat with a downbeat accent; follows `@BPM` + the playing content's meter live; stays
  phase-locked to the drum loop across the 30-min drift run.
- No RAM regression (§8), including the metronome build.

---

## 10. Risks / open questions (for the research agent)

- `Clock.positionBeats()` **monotonicity/tearing** across beat edges — stress-test.
- `beatPhase()` right after `start()` / a tempo change: `frac` clamp + `_lastTickMicros==0`
  path — verify no position jump on the first beat.
- **Catch-up cap = one bar** vs long `loop()` stalls (SD load, catalog build): a stall > 1
  bar desyncs. Ensure no SD-heavy work runs during playback, or handle a larger gap.
- **`@BPM` change mid-loop:** clock retimes; synced players follow automatically — verify the
  wrap stays clean when BPM changes mid-bar.
- **External MIDI clock** (`Clock::External`): `positionBeats()` must also work when slaved to
  0xF8 (`beatPhase` uses `_measuredIntervalUs`, set from external too). Confirm lockstep holds
  when following an external clock.
- **x/8 beat unit** decision (quarter vs eighth) — keep the Clock beat = quarter; carry
  `loopBeats` as an **exact fractional** quarter-beat count so x/8 loops wrap right (6/8=3.0,
  7/8=3.5). The downbeat grid stays integer (7/8/9/8 fall back to 4/4 bars — §3); a
  tick-defined bar for exact odd-meter downbeats is future work.
- **Compound/odd-meter metronome pulse** (§13): v1 clicks the Clock quarter (right for x/4).
  6/8 "in 2" (dotted-quarter) and 7/8 groupings (2+2+3) need a subdivision/grouping control —
  documented phase-2, not v1.
- **Concurrent session** owns `test_songs.h` / `gen_test_songs.py` / `main.cpp` right now —
  coordinate; rebase; never sweep their uncommitted work into a commit.
- **Launch-quantize** (concurrent App.tsx) — align start semantics if that toggle ships.

---

## 11. Rollout sequence

1. `Clock.positionBeats()` + off-target test.
2. `loopBeats` at parse + loader/test-song output.
3. `MidiFilePlayer` synced mode (`tickSynced`) + off-target tests.
4. `main.cpp` wiring (drum first; transport-idle zero policy).
5. Loop songs → synced; coordinate `test_songs.h` `loopBeats`.
6. Metronome (§13): firmware click + `@METRO`/BLE + app card. Build-flag-gated. Doubles as
   the audible lock probe for step 7 — bring it up before the hardware verify.
7. Hardware verify (drift probe + audible metronome) across meters; RAM check.
8. (Phase 2, optional) canonical-tick reparse for full fidelity; compound/odd-meter
   metronome subdivision + exact odd-meter downbeats (`_ticksPerBar` in `Clock`).

---

## 12. Fast-ramp pointers (current code)

- `lib/TDspClock/src/Clock.{h,cpp}` — 24 PPQN, `beatCount()/beatPhase()/barPhase()`,
  `consumeBarEdge()`, `update(micros)`. Internal mode free-runs from boot.
- `lib/TDspTempo/src/{Conductor,PlayerFollower,TempoFollower}.h` — master section;
  `onBarEdge()` fan-out already fires (unused by players).
- `lib/TDspMidiPlayer/src/{MidiFilePlayer.h,MidiSmfParser.h,MidiSmfFile.h,MidiFileEvent.h}`
  — the ms player, the pure SMF parser, the SD loader, the 6-byte event struct.
- `firmware/mix-kit/src/main.cpp` — `drumStartPath()` (calls `g_conductor.start()`),
  `songStartSd()/songStartBuiltin()`, `applyTempos()`, `applyMeter()`, `@STATE`,
  `g_conductor.update(micros())` in `loop()`. Meter detection (`initialBeatsPerBar`,
  `applyMeter`, `song = meter master`) already merged.
- `firmware/mix-kit/src/test_songs.h` — loop songs 13–17 (`seq()`-authored, bar-exact).
- Drift research + the Option A/B analysis are in the conversation that produced this doc.

---

## 13. Metronome (build-flag feature + app section)

A metronome is the natural first consumer of the master clock and the **audible proof**
that the tick-sync work locks: an on-beat click that stays phase-locked to the drum loop +
loop song *is* the acceptance test you can hear. It reads the same `tdsp::Clock` as
everything else — no separate timebase — so it can never drift from the players.

### 13.1 Behaviour
- **On/off** only (plus optional volume). Default **off**.
- **Follows the master automatically.** Tempo = the Conductor's BPM (already driven by
  `@BPM` / a song's detected tempo); meter/downbeat = `Clock.beatsPerBar()` (already set by
  `applyMeter()` from the playing song or groove). Change `@BPM` or start a 3/4 song and the
  click retimes and re-accents with **zero extra wiring** — it just polls the clock.
- **Downbeat accent:** click on every `consumeBeatEdge()`; when `beatInBar() == 0`, use the
  **accent** click (higher pitch / louder). So 4/4 = "**1** 2 3 4", 3/4 = "**1** 2 3",
  5/4 = "**1** 2 3 4 5" — the accent tracks whatever meter the content set.
- **Musical scope (v1):** clicks the **Clock quarter-note beat**, which is idiomatically
  correct for all **x/4** meters. For **compound** meters (6/8, 9/8, 12/8) v1 clicks the
  quarter subdivision ("in 3/4/6"), not the dotted-quarter felt pulse ("in 2/3/4"); for
  **irregular** meters (7/8, 5/8) the Clock is in 4/4 fallback (§3) so the click is a plain
  4/4. **Phase-2:** a subdivision/grouping control (dotted-quarter compound pulse; 2+2+3
  groupings) — noted, not in v1. This is the same quarter-vs-compound distinction called
  out in §3; keep them consistent.

### 13.2 Firmware (`src/Metronome.h`, `#ifdef TDSP_METRONOME`)
- **State:** `bool _on`, optional `float _vol`, a short click envelope.
- **Poll in `loop()`** (after `g_conductor.update(micros())`): if `_on &&
  clock.consumeBeatEdge()` → trigger a click; pitch/level by `clock.beatInBar() == 0`.
  Foreground dispatch, so jitter bound = one `loop()` iteration (same as the players, §6).
  **Note:** `consumeBeatEdge()` is a single-consumer latch — the metronome must be the only
  reader, or add a fan-out (mirror of the `onBarEdge()` fan-out in `Conductor::update`).
- **Click generator (RAM-tight):** cheapest is to **retune + gate the existing local
  test-tone `AudioSynthWaveformSine_F32`** per beat (accent ≈ 1500 Hz, normal ≈ 1000 Hz,
  ~15 ms decay), so **no new mix slot** on builds that carry the test tone. Where that slot
  is absent, add a tiny generator (sine/triangle + `AudioEffectEnvelope`) into a spare
  mixer input, or a second `AudioMixer4_F32` stage. Keep it F32 to match the bus. RAM: one
  oscillator + one envelope ≈ well under 1 KB; flag-gated so non-metronome builds pay zero.
- **No interaction with the synced players:** the metronome neither starts nor zeroes the
  transport — it only observes. It can run with nothing else playing (Internal clock
  free-runs from boot), which makes it a handy standalone practice click.

### 13.3 Control surface
- **Serial/web (shared `handleControlLine()`):** `@METRO=0|1`, optional `@METROVOL=<0..150>`.
  Report state in `@STATE` (`metro:on/off`, current `bpb`).
- **BLE:** new `CMD.SET_METRO` byte opcode (0/1) in the ESP32 GATT control service and the
  app's `CMD` table; optional `CMD.SET_METRO_VOL`.

### 13.4 App section (`app/tdsp-control/`)
- **New "Metronome" card** in `App.tsx`, placed next to the **Tempo** card (they share the
  master-BPM mental model). Contents:
  - A **Switch** (on/off) → `tp.metronome(v)`.
  - Read-out of the **current meter** so the user sees what it's counting, e.g. "4/4 · 120
    BPM" (meter from `@STATE` `bpb`, BPM from existing state). No tempo control of its own —
    it points at the Tempo card.
  - (Optional) a small volume slider → `tp.metronomeVol(pct)`.
- **Transport plumbing:** `metronome(on)` in `src/transport.native.ts`
  (`this.relay('@METRO=' + (on?1:0))`, mirroring `launchQuantize`) and `setMetronome` in
  `src/tdspBle.ts` (`writeByteCmd(CMD.SET_METRO, on?1:0)`, mirroring `setArpOn`).
- ⚠️ **Expo v57** — read https://docs.expo.dev/versions/v57.0.0/ before writing app code
  (app `AGENTS.md`). Follow the existing card/`Switch` idiom (see the Launch-quantize row).

### 13.5 Testing
- **Audible lock probe:** metronome ON + drum `03 Pop Backbeat` + song `13 Loop Pop Changes`
  — the click must sit exactly on the drum downbeat and stay there for the full 30-min drift
  run (§9). Drift = the click sliding off the kick; this is the acceptance test you can hear.
- **Meter re-accent:** start a 3/4 and a 5/4 test loop; confirm the accent moves to beat 1 of
  the new meter live, and `@BPM` changes retime the click with no restart.
- **RAM:** re-run the §8 `arm-none-eabi-size` check with `-D TDSP_METRONOME=1`; confirm no
  ITCM-block cliff crossing and `.bss` growth < the click generator's footprint.
