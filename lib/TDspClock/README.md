# TDspClock — the timebase primitive

`tdsp::Clock` is the shared musical-time reference: a 24-PPQN clock with two
sources —

- **Internal** — free-running at `setInternalBpm(...)`; `update(nowMicros)` in
  `loop()` emits catch-up ticks and fires the internal-tick hook.
- **External** — slaved to incoming MIDI Timing Clock (`0xF8`), fed via a
  `ClockSink` registered on a `MidiRouter`.

It exposes `beatPhase()` / `barPhase()` (smoothly interpolated between ticks),
`consumeBeatEdge()` / `consumeBarEdge()` latches, and a `setInternalTickHook()`
fan-out for driving `onClock()` consumers (e.g. an arp via
`MidiRouter::handleClock()`).

## How modules consume it

`Clock` is a *timebase*, not a coordinator. In practice modules don't wire to it
directly — they go through **[`TDspTempo`](../TDspTempo)**, whose `Conductor` owns
the BPM/transport authority and fans it out to followers (song player, drum
groove, arp, LFO, external clock). **See
[`TDspTempo/README.md`](../TDspTempo/README.md) for the per-module consumption
recipes.**

Use `Clock` directly only when you need the raw timebase — e.g. an ISR-free LFO
that just reads `barPhase()`.

## Files
- `Clock.h` / `Clock.cpp` — the clock.
- `ClockSink.h` — `MidiRouter` → `Clock` bridge for external `0xF8`/Start/Stop.
