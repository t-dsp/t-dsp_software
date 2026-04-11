# Epic: OSC Mixer Foundation

**Status:** Planning → Spike 1 about to begin
**Owner:** Jay
**Last updated:** 2026-04-11

## What this epic delivers

The reusable building blocks for a **professional live-audio mixer control surface** running on Teensy 4.x, controlled over OSC, with the TAC5212 audio shield adaptor as the first ("small mixer") example. By the end of this epic:

- A shared `lib/TDspMixer/` framework that any future T-DSP mixer example consumes (8×8, 16×16, etc.)
- The existing `projects/t-dsp_tac5212_audio_shield_adaptor/` is refactored into a thin example that wires up TDspMixer + a `Tac5212Panel` codec settings panel
- A working OSC control surface with X32-flavored addresses that any OSC-aware client (Open Stage Control, TouchOSC, future Tauri app) can drive
- A vendored, version-pinned dependency stack: OSCAudio, OpenAudio_ArduinoLibrary, PaulStoffregen/Audio, PaulStoffregen/cores
- A debug-friendly serial transport: SLIP-OSC frames + plain `Serial.println` debug output + a dotted-path CLI escape hatch, all multiplexed on the same USB CDC stream

This epic does **not** include: the Tauri host-side GUI, scene library content, dynamics processing, multi-channel USB beyond stereo, USB Audio Class 2, or any of the future-example boards. Those are downstream of the foundation this epic establishes.

## How to read this epic

The documents are numbered to suggest a reading order, but they are individually self-contained — you can jump straight to the one you need.

| # | Document | What it covers |
|---|---|---|
| 00 | [Overview](00-overview.md) | Vision, scope, framing, what success looks like |
| 01 | [Architecture](01-architecture.md) | The four-layer stack, library/example split, the load-bearing **mixer-vs-audio-library** principle |
| 02 | [OSC Protocol](02-osc-protocol.md) | Full address tree, X32 conventions, CLI escape hatch, subscription model |
| 03 | [Transport](03-transport.md) | SLIP-OSC over USB CDC, multiplexing rules for input and output |
| 04 | [Dependencies](04-dependencies.md) | OSCAudio, OpenAudio, Audio, cores: roles, licenses, engagement plan with Jonathan |
| 05 | [Vendoring strategy](05-vendoring-strategy.md) | git subtree, `vendored.json`, `tools/vendor.py`, the cores overlay mechanism |
| 06 | [Small mixer model v1](06-mixer-model-v1.md) | Concrete scope: 5 channels, 2 buses, signal chain, EQ, solo, scenes |
| 07 | [Spike plan](07-spike-plan.md) | Three sequenced spikes: foundation, multi-channel USB, USB input feature unit |
| 08 | [Roadmap](08-roadmap.md) | Milestones, sequencing, open questions, future work |
| — | [Upstream PRs](upstream-prs.md) | Prepared materials for the four contribution candidates (OSCAudio bug fix, OSCAudio docs, OpenAudio peakingEQ, cores type_traits). Send when Spike 1 is green. |

## The load-bearing principles

If you only remember three things from this epic:

1. **TDspMixer is the mixer; OSCAudio is the audio library config back door.** The blessed client-facing OSC surface is X32-style (`/ch /bus /main /codec`) handled by TDspMixer's own dispatchers. OSCAudio's `/teensy*/audio/` namespace is a build-flag-gated debug surface for poking individual Audio Library objects, never the primary control interface. See [01-architecture.md](01-architecture.md).

2. **Address tree is the single source of truth — CLI is a thin shim over OSC.** The dotted-path serial CLI is a mechanical mirror of the OSC address tree, parsed by a ~50-line shim that converts text to `OSCMessage` and routes through the same dispatcher. There are no independent CLI handlers. See [02-osc-protocol.md](02-osc-protocol.md).

3. **Every example consumes the same TDspMixer; per-board code is a thin `CodecPanel` subclass.** Anything portable across future examples (faders, EQ, sends, scenes, meters, subscriptions) lives in the library. Anything codec-specific (preamp config, input mode, PDM source) lives in the project's `CodecPanel`. See [01-architecture.md](01-architecture.md).

## Execution status

- [x] Architecture decisions made and recorded in memory
- [x] OSC address conventions agreed (X32-flavored)
- [x] Transport committed (SLIP-OSC + multiplex + CLI escape hatch)
- [x] Vendoring approach decided (git subtree + `vendored.json`)
- [x] Multi-channel USB approach decided (Option B: vendor cores via overlay)
- [x] Planning documents written (this folder)
- [ ] **Spike 1 — foundation validation** ← next
- [ ] Spike 2 — multi-channel USB
- [ ] Spike 3 — USB input feature unit
- [ ] TDspMixer library implementation
- [ ] Small mixer example refactor
- [ ] Engagement with Jonathan (h4yn0nnym0u5e)
