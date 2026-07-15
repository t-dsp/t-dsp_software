# TDspTempo — the master clock system

One **Conductor** is the tempo authority for a synth: it owns the master BPM,
the transport (start/stop), and a `tdsp::Clock` (the 24-PPQN timebase from
[`TDspClock`](../TDspClock)). Every tempo-aware module — song player, drum
groove, arpeggiator, LFO, tempo-synced delay — locks to it, so moving the one
BPM knob retimes everything together and a transport start gives them a shared
downbeat. It's the "master section" of a pro keyboard, as a reusable library.

```
                       ┌─────────────────────────────────────────────┐
   @BPM= / tap  ──────▶│  Conductor            setBpm() = one authority │
                       │   ├─ Clock  (24-PPQN, Internal | External)     │
   external 0xF8 ─────▶│   │        beatPhase / barPhase / edges        │
   (via ClockSink)     │   ├─ followers[]  → onBpm/onStart/onBarEdge    │
                       │   └─ tick hook   → MidiRouter::handleClock()    │
                       └─────────────────────────────────────────────┘
                                │ onBpm()              │ 24-PPQN tick
              ┌─────────────────┼──────────────┐       │
              ▼                 ▼              ▼        ▼
        PlayerFollower    PlayerFollower    LFO...   ArpFilter (onClock)
        (song player)     (drum groove)             (tick consumer)
```

## Two ways a module consumes the clock

Pick by how the module keeps time:

| Kind | Register how | Gets | Use for |
|------|--------------|------|---------|
| **Tempo follower** (scale-based) | `conductor.addFollower(&f)` where `f : ITempoFollower` | `onBpm()`, `onStart()`, `onStop()`, `onBarEdge()` | anything that times *itself* and just needs to be retimed — file players, an LFO reading `barPhase()` |
| **Tick consumer** (24-PPQN) | attach to `conductor.clock()`; be fed by `conductor.setTickHook(...)` | one call per 24-PPQN tick (via `MidiRouter::handleClock()` → `onClock()`) | the arp, a step sequencer, MIDI-clock-out — anything that must act *on the grid* |

**Design rule:** `Conductor::setBpm()` is the *single* tempo write path. Never
call a player's `setTempoScale()` (or a clock's `setInternalBpm()`) directly —
route every tempo change through the Conductor and the "one authority" guarantee
holds no matter how many modules subscribe.

---

## Per-module consumption recipes

### 1. Song player (`MidiFilePlayer`) — tempo follower

The player advances by elapsed ms × `setTempoScale()`. `PlayerFollower` does the
`masterBpm / nativeBpm` math for you.

```cpp
tdsp::PlayerFollower songFollow{g_player};
conductor.addFollower(&songFollow);          // once, at setup

// on every song load — feed the file's authored tempo:
songFollow.setNativeBpm(songNativeBpm);      // SD .mid = real tempo; baked = estimate
g_player.play(events, count);
```

That's it — `@BPM=` now retimes the song, because `setBpm()` calls
`songFollow.onBpm()` which re-derives the scale.

### 2. Drum groove (a second `MidiFilePlayer`, looping) — tempo follower + trim

Same adapter, plus the optional per-player **trim** for a "groove speed" control,
and a transport `start()` so the arp/grid lock to the groove's downbeat.

```cpp
tdsp::PlayerFollower drumFollow{g_drumPlayer};
conductor.addFollower(&drumFollow);

// on groove load:
drumFollow.setNativeBpm(grooveNativeBpm);
drumFollow.setTrim(drumSpeedPct);            // 100 = exactly master BPM
g_drumPlayer.play(grooveEvents, n);          // "beat 1 = now"
conductor.start();                           // zero the Clock → shared downbeat
```

### 3. Arpeggiator (`ArpFilter`) — tick consumer

The arp is **not** a follower. It steps on 24-PPQN ticks off the shared Clock:

```cpp
g_router.addSink(&g_arpFilter);                       // arp sits on the router
g_arpFilter.setClock(&conductor.clock());             // reads the shared timebase
conductor.setTickHook(+[](void*){ g_router.handleClock(); }, nullptr);
// → in Internal mode, each internal 24-PPQN tick fans onClock() to the arp,
//   exactly as an external 0xF8 would. Its rate enum (1/8, 1/16, triplets…)
//   is relative to the master BPM automatically.
```

Optionally give the arp bar-quantized restart by implementing `onBarEdge()` (make
it *also* a follower) so a newly held chord snaps its pattern to the downbeat.

### 4. LFO / tempo-synced modulation — tempo follower *or* phase pull

Two styles:
- **Free-running, reads phase:** no registration — just call
  `conductor.clock().barPhase()` (0..1, interpolated smoothly between ticks) each
  block and map it to the mod shape.
- **Rate as a note value:** register a follower and recompute your increment in
  `onBpm()`.

### 5. Tempo-synced delay — tempo follower

Register a follower; in `onBpm()` set the delay time to a note division of the
tempo (e.g. dotted-eighth = `60000/bpm * 0.75` ms).

### 6. External MIDI clock (slave the whole kit) — via `ClockSink`

Register a `ClockSink` on the router and switch source to External; every module
above then follows the incoming tempo with **zero changes**, because they all read
the same Clock.

```cpp
tdsp::ClockSink clockSink{&conductor.clock()};
g_router.addSink(&clockSink);                // 0xF8/Start/Stop → Clock
// wire your MIDI stack's real-time handlers to the router, then:
conductor.setSource(tdsp::Clock::External);  // internal BPM becomes the fallback
```

### 7. The BPM knob / UI — the authority's front door

```cpp
void setMasterBpm(int bpm) { conductor.setBpm((float)bpm); }   // @BPM=
```

Read back `conductor.bpm()` for display; `conductor.clock().beatInBar()` /
`barPhase()` drive a metronome dot or beat LED.

---

## Full wiring example (the mix-kit)

```cpp
tdsp::Conductor      conductor;
tdsp::PlayerFollower songFollow{g_player}, drumFollow{g_drumPlayer};
tdsp::ClockSink      clockSink{&conductor.clock()};

// setup():
conductor.begin(120.0f);                     // Internal, free-running at 120
conductor.addFollower(&songFollow);
conductor.addFollower(&drumFollow);
g_router.addSink(&clockSink);                // external-clock seam (dormant until
                                             // real-time handlers + External source)
conductor.setTickHook(+[](void*){ g_router.handleClock(); }, nullptr);

// loop():
conductor.update(micros());                  // advance clock, fire bar edges
```

One `applyTempos()` becomes:

```cpp
static void applyTempos() {
    songFollow.setNativeBpm(g_songBpm);
    drumFollow.setNativeBpm(g_drumFileBpm);
    drumFollow.setTrim((float)g_drumSpeedPct);
    conductor.setBpm(g_masterBpm);           // fans to both followers
}
```

---

## Why this sits on `TDspClock` (and not uClock)

`TDspClock` is a **foreground-polled** 24-PPQN clock (`update(micros())` in
`loop()`) — deliberately, because the whole MIDI→router→arp→synth stack in these
projects is single-threaded (no ISR re-entrancy into Dexed/ymfm/TSF/SD). Libraries
like **uClock** generate rock-steady ticks from a hardware-timer ISR, but (a) they
replace only the *primitive*, not this follower/retiming layer — you'd still write
the Conductor — and (b) firing the arp chain from an ISR is unsafe here, so you'd
marshal back to `loop()` anyway and lose the timing edge.

Because every module above consumes the **Conductor + tick-hook interface**, the
tick *source* is swappable: if `loop()` jitter ever makes the arp audibly wobble
under load, drop an IntervalTimer generator (uClock-style, or uClock itself)
behind `setTickHook()` — the follower layer and all seven recipes are untouched.
