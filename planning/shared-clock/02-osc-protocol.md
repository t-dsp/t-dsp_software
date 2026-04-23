# 02 — OSC protocol

All endpoints follow the project's read/write/echo convention: empty
args = read-back, typed args = write, reply always echoes the current
value. Writes arrive throttled (~30 Hz) from the UI via
`Dispatcher.sendThrottled()` for continuous params.

## `/clock/*` — clock state

| Address              | Types | Values             | Direction    |
|----------------------|-------|--------------------|--------------|
| `/clock/source`      | `s`   | `"ext"` \| `"int"` | read / write |
| `/clock/bpm`         | `f`   | 20..300            | read / write¹ |
| `/clock/running`     | `i`   | 0 / 1              | read only    |
| `/clock/beatsPerBar` | `i`   | 1..16              | read / write |

¹ `/clock/bpm` only *takes effect* when `source == "int"`. In External
mode the value is measured from incoming ticks; the firmware will
accept a write but it'll be overwritten on the next 0xF8.

Writes are echoed; a malformed `source` string is ignored (firmware
only accepts the 3-char prefixes `int` / `ext`).

### Source transitions

| From \ To | `ext`                          | `int`                           |
|-----------|--------------------------------|---------------------------------|
| `ext`     | no-op                          | `running` flips to true, tempo switches to setpoint |
| `int`     | `running` flips to false, awaits upstream Start | no-op                           |

Tick count is preserved across source changes.

## `/loop/*` — clock-aware looper additions

Existing `/loop/record|play|stop|clear` (no args) were unchanged in
shape but gained *quantize-aware* behavior. New endpoints:

| Address           | Types | Values    | Direction    |
|-------------------|-------|-----------|--------------|
| `/loop/quantize`  | `i`   | 0 / 1     | read / write |
| `/loop/armed`     | `i`   | 0..4      | read only    |

`armed` encoding:

```
0 = none
1 = record
2 = play
3 = stop
4 = clear
```

### Quantize flow

When `/loop/quantize 1` and the clock is running, a transport action
arms instead of firing:

```
client            firmware
  │    /loop/record    │
  │ ─────────────────▶ │  g_looperArmedAction = 1
  │                    │  (no state change on the looper yet)
  │ ◀───────────────── │  /loop/armed 1
  │                    │
  │                    │   ... clock advances ...
  │                    │
  │                    │  on next consumeBeatEdge():
  │                    │    g_looper.record()
  │                    │    g_looperArmedAction = 0
  │ ◀───────────────── │  (UI polls /loop/state to observe the flip)
```

If the clock is stopped (no ticks + no internal start), quantize
falls through to an immediate fire — otherwise the armed action
would hang forever and the looper would feel broken.

Toggling quantize off while armed cancels the pending action. This
is cleaner than leaving a ghost that would fire on the next Start.

## Snapshot

`/clock/running` is the only endpoint not covered by the existing
`/snapshot` reply — it's a pure transport-state probe. The Clock tab
fires a one-shot query (`queueMicrotask(() => dispatcher.queryClockRunning())`)
on mount so the transport LED shows the current state without waiting
for a transport event.

## Sink dispatch (server side)

System Real-Time messages are channelless and fan out to every
MidiSink via `MidiRouter::handleClock/Start/Continue/Stop()`. The
default `MidiSink` overrides are no-ops, so existing sinks (Dexed,
MPE, MidiViz) continue to compile untouched. Only `ClockSink`
consumes the events today.
