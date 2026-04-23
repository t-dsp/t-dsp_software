# Shared clock

One musical-time reference drives every tempo-aware module on the
device: looper quantize-to-beat, future tempo-synced LFOs,
arpeggiators, delay sync, and the beats drum machine once its
firmware lands.

Without this, every module would have to detect MIDI clock on its own,
or the user would have to set BPM in three places and hope they stay
in sync. With it, the clock is one shared resource: the MidiRouter
fans 0xF8 / 0xFA / 0xFB / 0xFC into `tdsp::Clock`, and any consumer
reads phase / beat-count / edge-latches off a single instance.

## Sources

- **External** (default) — slaved to incoming MIDI Timing Clock at
  24 PPQN. BPM is estimated from the inter-tick interval with an IIR
  window of 8 samples. Transport follows the upstream (Start / Stop /
  Continue). A 500 ms stall watchdog auto-flips transport to stopped
  so consumers don't lock to a phantom grid if the upstream vanishes.
- **Internal** — device is master. `update()` emits catch-up ticks
  from `micros()`, so LFOs and loop quantize still work without any
  external controller attached. BPM clamp: [20, 300].

Switching sources preserves `_tickCount` so phase doesn't snap
under a running pattern; only the stamp scaffolding resets.

## What's in this folder

- [01-architecture.md](01-architecture.md) — the Clock class, MidiSink
  additions, ClockSink, where things get called from.
- [02-osc-protocol.md](02-osc-protocol.md) — the `/clock/*` surface
  and the `/loop/quantize` / `/loop/armed` additions.
- [03-ui.md](03-ui.md) — the Clock tab structure and signal flow.

## Current consumers

- **Looper quantize** ([main.cpp:2640](../../projects/t-dsp_tac5212_audio_shield_adaptor/src/main.cpp#L2640)).
  When `/loop/quantize 1`, transport actions are held in
  `g_looperArmedAction` and committed on the next `consumeBeatEdge()`.

## Not-yet-consumers (follow-ups)

- **MPE LFO** ([MpeVaSink::tick(millis())](../../lib/TDspMPE/src/MpeVaSink.cpp))
  — still wall-clock. Adding an `lfoSync` option that reads
  `g_clock.beatPhase()` is a natural next step.
- **Beats drum machine** — currently carries its own
  `BeatsState.bpm`. Once the firmware side lands, point it at
  `g_clock` instead so there's one tempo on the device.
- **Device-side `usbMIDI`** — only USB-host MIDI is forwarded today.
  Forwarding the device-side realtime handlers is a small addition if
  a DAW ever needs to clock the Teensy directly.
