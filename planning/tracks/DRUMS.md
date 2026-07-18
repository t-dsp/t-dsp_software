# Tracks — Phase 2: drums as a Track

## Goal

Fold the special-cased drum groove player (`g_drumPlayer` + `drumStart*`/`drumStop`/synchro/ch10-mask)
into the **same `Track` abstraction** the two synth voices now use, so drums obey the ONE transport
contract (quantized launch from the bar downbeat, preload-off-beat, tick-before-launch). This kills
the bug the user just observed on the drumvoice env: **starting drums stole/overtook the melodic
player** — because drums start immediately/non-quantized today, off the shared launch path.

"The drum looper is a literal duplicate of the synths." A drum Track = `g_tracks[k]` whose engine
(sink) is drum-capable and whose content is a looping channel-10 `.mid`. It runs through
`trackPreload`→`trackFire`/`trackStartArg`/`trackLaunch`/`trackRestart` like Voice 1 & 2.

## Branch: `tracks-phase2-drums` (off master @ Phase-1 merge cfdd56c).

## The drum "specials" and where each lives in the Track model

The drum player diverges from a synth voice in these ways. Phase-1 already handles the easy ones
(no arp → `arp==null`; the drum sink → `t.sink`). The rest map to **new Track fields / caps / hooks**,
kept minimal (same caps style as Phase 1). Nothing gets a bespoke code path — it's data on the Track.

| Drum special (today) | Where it is now | Track home (P2) |
|---|---|---|
| ch10-ONLY channel mask | `g_drumPlayer.setChannelMask(1<<9)` | new `Track.chMask` (synth = kMaskNoDrums; drum = `1<<9`); `trackWireSetup` applies it |
| seamless internal loop | `setLooping(true)` | `caps.loopsSeamless` → `trackWireSetup` sets `player.setLooping(true)`; `*t.loop` pinned true |
| no arp | `setSink(g_synthSink)` direct | `arp == null` (already handled) |
| dedicated drum sink | `drumTsfSink`/`drumVoiceSink`/`g_synthSink` | `t.sink` (bound in tracksInit per build) |
| program-change OFF | `setProgramChangeEnabled(false)` | `caps.ownsPatch` (drum owns its kit; player must not honor the file's PCs) |
| GM/kit engine gating | `drumEngineOk()` guard | `caps.drumGated` → `trackFire` bails if `!drumEngineOk()` |
| kit select (ch10 prog change) | `drumApplyKit()` | prep hook: `caps.appliesKit` → `songPrep`/`trackFire` calls `drumApplyKit()` |
| mute the SONG's ch10 while playing | `muteSongDrums(true/false)` | start/stop hook: `caps.mutesSongDrums` → `trackFire`/`songStop` toggles `muteSongDrums` |
| tempo source when idle | `g_masterBpm = g_drumFileBpm` | `caps.tempoSourceWhenIdle` → `trackFire` sets master BPM iff transport idle |
| velocity = vol% | `setVelocityScale(vol/100)` | `t.setLevel` = a drum-vol setter (already a per-track fn ptr) |
| SYNCHRO start (PSS-140) | `g_drumArmed` + `maybeSynchroStart` | KEEP as-is for now (niche); a synchro-armed track just defers its fire. Revisit. |

Net new Track surface: `uint16_t chMask;` + caps `{loopsSeamless, ownsPatch, drumGated, appliesKit,
mutesSongDrums, tempoSourceWhenIdle}`. All default off/`kMaskNoDrums` so Voice 1 & 2 are unchanged.

## What stays OUTSIDE the Track (for now)

- `buildDrumList` / `g_drums[]` / `@DRUM=<index>` legacy flat menu, `@DRUMKIT`, `kDrumKits[]` — the
  *kit* and the *groove catalog* are separate concerns (the browser thread owns file selection).
- The drum SINK construction + `drumTsfBegin/drumVoiceBegin` in setup — unchanged; the Track just
  binds `t.sink` to whichever drum sink the build brought up.
- Path resolution (`/drums` vs `/midi/drums`) — owned by the **browser thread**; the drum Track calls
  the same `trackPreload` and inherits whatever path convention lands there. Clean seam.

## Phased (each green-buildable + HW-checkpointed, mirroring P1.x)

- **P2.1** Add the drum Track to `g_tracks[]` (bind `g_drumPlayer` + drum sink + drum state + drum
  caps + `chMask`), populated in `tracksInit()`. Extend `Track.h` (chMask + new caps). NO readers yet
  → provably no behavior change. Green build (opll + voice2 + drumvoice).
- **P2.2** Route drum STOP + the loop/tick/@STATE reads through the drum Track (`songStop` with
  `mutesSongDrums`; `trackLoopTick` is a harmless no-op while `loopsSeamless`). Keep `@STATE` "drums"
  key identical.
- **P2.3** Route drum START through `trackPreload`→`trackFire` (the big one): fold kit/mute/ch10/
  tempo-source/seamless-loop via the caps above; **drums now launch quantized on the bar downbeat**
  like the players. Retire `drumStartPath`/`drumStartFile`/`drumStart` bodies (thin shims over the
  Track path). `@DRUMF`/`@DRUM` dispatch → `trackLaunch`/`trackStartArg`/`songStop` on the drum Track.
- **P2.4** Retire the hand-wired drum block in `setup()` (`g_drumPlayer.setSink/setChannelMask/
  setLooping/setProgramChangeEnabled`) in favor of `trackWireSetup(drumTrack)`. Retire the separate
  drum `tick()` in `loop()` (drum Track ticks in the `g_tracks[]` range-for). Synchro revisited.

## Verify (HW, COM4 drumvoice + a GM env)

Drums start **on the next bar downbeat** under a running player (no more overtaking / stolen
downbeat); a groove started idle defines the grid + master tempo; kit switch still works; the song's
own ch10 stays muted while a groove plays and returns on stop; `@STATE` "drums" unchanged.

## Integration with the browser thread

The drum Track plays a `.mid` via `trackPreload(drumTrack, <path>)`. Today the path is `/drums/<f>`;
after the browser's `/midi` hard-cut it's `/midi/drums/<f>`. The drum module does not hardcode the
directory beyond what `trackPreload` resolves — so when the browser lands, drums follow for free.

## Cross-ref — drum-note-map (ch10 Roland->GM remap)

The ch10 groove stream now passes through a `tdsp::DrumNoteMapper` shim
(`firmware/mix-kit/src/DrumNoteMap.h`) inserted between `g_drumPlayer` and the real
drum sink in `setup()` — it rescues GMD's Roland hi-hat notes 22/26 (below GM range,
else dropped silent) by folding them to 42/46 in `GmReduce` mode. See
`planning/drum-note-map/DESIGN.md`.

**When P2.4 retires the hand-wired drum block for `trackWireSetup(drumTrack)`, the
mapper MUST stay in the drum Track's sink chain** (route the track's sink through
`g_drumNoteMapper`, not straight to `g_drumTsfSink`/`g_drumVoiceSink`/`g_synthSink`),
or the silent-hi-hat bug returns. The Track's `sink` field currently points at the
REAL sink (the mapper only wraps `g_drumPlayer.setSink`), because the drum Track is
still inert; that is the seam to reconcile.
