# 01 — Architecture

## Layout

```
lib/TDspMidi/src/
  MidiSink.h          onClock / onStart / onContinue / onStop virtuals (no-op defaults)
  MidiRouter.h/.cpp   handleClock / handleStart / handleContinue / handleStop
                      — fan channelless Real-Time to every sink

lib/TDspClock/src/
  TDspClock.h         umbrella header
  Clock.h/.cpp        tdsp::Clock — PPQN phase, BPM estimator, internal/external source
  ClockSink.h         tdsp::ClockSink — MidiSink bridge forwarding into a Clock
```

## MidiSink extension

Added four no-op virtuals so existing sinks keep compiling untouched:

```cpp
virtual void onClock()    {}  // 0xF8 — one 24-PPQN tick
virtual void onStart()    {}  // 0xFA — reset to bar 1 and run
virtual void onContinue() {}  // 0xFB — resume from current position
virtual void onStop()     {}  // 0xFC — halt transport, keep position
```

`MidiRouter` fans each to every registered sink regardless of channel.
`ClockSink` is typically the only consumer today; other sinks may
adopt realtime hooks later (e.g., MidiVizSink drawing a beat
indicator).

## Clock state

```cpp
enum Source : uint8_t { External = 0, Internal = 1 };

class Clock {
  // Feed (from MidiSink)
  void onMidiTick();      // ignored when Source==Internal
  void onMidiStart();
  void onMidiContinue();
  void onMidiStop();

  // Foreground tick (from loop())
  void update(uint32_t nowMicros);

  // Query
  float    bpm()        const;          // last-measured (ext) or setpoint (int)
  bool     running()    const;
  uint32_t tickCount()  const;          // since last Start
  uint32_t beatCount()  const;
  uint8_t  beatInBar()  const;
  float    beatPhase()  const;          // 0..1, interpolated
  float    barPhase()   const;

  // Edge latches (consumed by foreground)
  bool consumeBeatEdge();
  bool consumeBarEdge();
};
```

## BPM estimator

One-pole IIR: `new = (old * 7 + sample) / 8`. Intervals outside
`[2 ms, 250 ms]` (corresponds to tempos outside `[20, 1250]` BPM) are
rejected as transient noise — protects against buffered-MIDI bursts
at connect time. BPM formula: `60_000_000 / (24 * micros_per_tick)`.

## Internal source

`update(nowMicros)` computes `microsPerTick(bpm)` and emits catch-up
ticks when `nowMicros - lastTickMicros >= perTick`. Catch-up is
capped at one bar's worth so a debugger pause doesn't flood the tick
stream when execution resumes. `_measuredIntervalUs` is kept in sync
with the setpoint so `bpm()` reads the user's chosen value, not a
stale external number.

## Stall watchdog

External mode only. If running and no tick arrives for 500 ms, flip
transport to stopped. `_bpm` pins at its last value so the UI shows
a useful number until the next Start. 500 ms is generous at our
slowest supported tempo (20 BPM = 125 ms per tick).

## Edge latches

`consumeBeatEdge()` / `consumeBarEdge()` return `true` at most once
per boundary and clear on read. Single-consumer by design — fan-out
would need external bookkeeping. The looper quantize path is the only
consumer today. `onMidiStart()` pre-arms both edges for tick 0 so a
quantize arm lands on the downbeat.

## Interpolated phase

Between ticks, phase interpolates using wall-clock time:

```
phase = (tickCount % 24) / 24 + (microsSinceTick / microsPerTick) / 24
```

Gives LFOs and UI beat indicators a smooth ramp rather than a
24-step staircase.

## Thread-safety note

Everything runs on loop() today: USB host MIDI is drained there via
`g_midiIn.read()`, not from an ISR. `onMidiTick()` therefore uses
`_lastUpdateMicros` captured at the top of the most recent `update()`
rather than calling `micros()` directly — keeps the header Arduino-
agnostic. Sub-ms jitter vs real `micros()` is well inside tick-to-tick
variance, but if a future path delivers clock from an ISR the
counters would want `__disable_irq` guards.

## Wiring in main.cpp

```cpp
tdsp::Clock     g_clock;
tdsp::ClockSink g_clockSink(&g_clock);

// setup():
g_midiIn.setHandleClock   (onUsbHostClock);    // 0xF8 → router → sink → clock
g_midiIn.setHandleStart   (onUsbHostStart);
g_midiIn.setHandleContinue(onUsbHostContinue);
g_midiIn.setHandleStop    (onUsbHostStop);
g_midiRouter.addSink(&g_clockSink);

// loop():
g_clock.update(micros());       // BEFORE draining MIDI
g_usbHost.Task();
while (g_midiIn.read()) {}
if (g_looperArmedAction != 0 && g_clock.consumeBeatEdge()) { /* fire */ }
```

Calling `g_clock.update()` *before* the MIDI drain means the external
path sees a fresh `_lastUpdateMicros` when 0xF8 bytes fold in during
`g_midiIn.read()`.
