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
- **Loop length:** `loopBeats = round(totalBeats)` where
  `totalBeats = lastEventTick / filePPQN` (compute at **parse**, exact, avoids ms round).
  Fallback: `round(totalMs × nativeBpm / 60000)`. Integer beats, meter-agnostic.
- **Wrap:** `pos = fmod(songBeat, loopBeats)`. Dispatch the window `[prevPos, pos)` with
  wrap handling (if `pos < prevPos`, emit `[prevPos, loopBeats)` then `[0, pos)`).

Because tempo lives entirely in the clock, the synced player has **no `speed_`** and
**ignores the file's tempo** for playback rate; the file's positions are the musical grid,
the clock supplies wall-clock tempo. (This is why tempo-map songs stay on the ms engine —
see §7.)

---

## 3. Measure-count handling (4 / 3 / 5 / 7 …)

- Loop length is stored in **beats, not bars** → inherently handles any meter.
- `Conductor.beatsPerBar` (from the meter-detection work already merged —
  `smf::initialBeatsPerBar()` + `applyMeter()` in main.cpp) defines where bar **downbeats**
  fall; the loop wraps on **beats**. Examples: 5-beat loop in 5/4 = 1 bar; 6-beat loop in
  3/4 = 2 bars; 16-beat loop in 4/4 = 4 bars.
- **"Beat" = quarter note** in `Clock` (24 PPQN/quarter). For x/8 meters, `initialBeatsPerBar`
  already converts `nn/2^dd → quarter-beats-per-bar` (6/8 → 3). Keep `loopBeats` in
  **quarter-note beats** for consistency; document this. A 2-bar 6/8 loop = 6 quarter-beats.
- **Non-integer-bar loops** (e.g. a 4.5-beat groove): `round()` to the nearest integer beat
  (still drift-free, wraps on a beat). **Log/assert** when `loopBeats % beatsPerBar != 0`
  so odd content is visible; its bar-1 will rotate against the global bar grid (acceptable/
  rare). GMD grooves carry no time-sig meta (all report 4/4) — integer-beat quantization is
  the safe, meter-agnostic default there.

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
- Compute exact loop length at parse: add `smf::initialLoopBeats(buf,len)` (from
  `lastEventTick / division`, meter-agnostic, integer-beat rounded) OR extend
  `loadSmfFile(...)` to also output `double* outLoopBeats` alongside the existing `outBpm`.
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
- Any-meter loops (4/3/5/6-8) lock and wrap on the correct grid.
- No RAM regression (§8).

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
- **x/8 beat unit** decision (quarter vs eighth) — keep quarter, align `loopBeats` with
  `initialBeatsPerBar`.
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
6. Hardware verify (drift probe + audible) across meters; RAM check.
7. (Phase 2, optional) canonical-tick reparse for full fidelity.

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
