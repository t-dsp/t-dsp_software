# 00 — Overview

## What we are building

**The reusable building blocks for a professional live-audio mixer control surface** running on Teensy 4.x.

This is not a hobby USB sound card project, and the existing `projects/t-dsp_tac5212_audio_shield_adaptor/` firmware is not "the" mixer — it is the **first example** in a planned family of mixers of increasing channel count. Future examples include 8×8 and 16×16 boards using different codecs. Everything portable across that family belongs in a shared library (`lib/TDspMixer/`); each example is a thin wiring file plus a codec-specific settings panel.

The framing matters because it changes nearly every architectural decision. A hobby project can hardcode three channels and call it done. A reusable framework has to think in terms of channel strips, buses, sends, scenes, subscriptions, multi-client sync, and metering — all the things a real mixing engineer needs — even when the first example only exercises a fraction of them.

## Why this matters

Pro live audio has well-established conventions for how a control surface behaves and how it talks to its host:

- **Channel strips.** Each input is a strip with preamp → HPF → EQ → dynamics → fader → mute → pan → sends. Engineers know this shape. Software that doesn't match it is software that fights its users.
- **Buses.** Auxes, subgroups, matrix mixes, mains. Inputs send to buses; buses sum and route.
- **Scenes / snapshots.** The entire mixer state can be saved and recalled instantly. Critical for show files.
- **Subscriptions and metering.** Multi-client sync, with the device pushing meter blobs at 30–50 Hz to whichever clients are listening.
- **OSC.** The lingua franca of remote control for pro consoles. X32, dLive, SD, S6 — they all speak OSC variants. Tools like Open Stage Control, TouchOSC, QLab, Reaper, Lemur, and dozens of phone/iPad surfaces are OSC-native.

If we build the small mixer following these conventions, every existing OSC tool in the pro audio world can drive it on day one, the engineers who use those tools feel at home, and the framework scales naturally to bigger examples without redesign.

If we don't — if we build a bespoke text protocol over serial, with magic key commands and ad-hoc state — then every future example reinvents wheels, no existing tool helps, and engineers find it frustrating to use.

This epic is the structural commitment to do it the right way from the start, while the surface area is still small.

## What success looks like

**End of this epic, the following are true:**

1. The small mixer's `main.cpp` is a thin wiring file (~150 lines, mostly configuration). All the interesting logic lives in `lib/TDspMixer/`.
2. A pro audio engineer can launch Open Stage Control with a JSON preset committed to this repo, see five faders + mutes + solos + a 4-band EQ + a TAC5212 settings tab, and mix audio with it.
3. The same engineer can `git pull` next month, see a new mixer example for an 8-channel board in `projects/`, launch Open Stage Control with a different preset, and feel that it is "the same software, more channels."
4. A firmware developer can SSH into the Teensy over SLIP-OSC, type `codec.tac5212.reg.get 0x14` in a serial monitor, and get a register read back — without the OSC tooling needing to be installed.
5. All external libraries (OSCAudio, OpenAudio, Audio, cores) are vendored as git subtrees, version-pinned in `vendored.json`, and contributable back upstream via subtree push.
6. The Teensy core has been modified (via the vendored cores subtree) to enable multi-channel USB audio and a USB Audio Class feature unit on the input terminal so Windows shows a recording volume slider that the firmware can read.

## What is explicitly out of scope for this epic

- **Tauri host-side GUI.** The validation client is Open Stage Control. Tauri is a downstream epic.
- **USB Audio Class 2.** UAC1 is sufficient for v1. UAC2 (24-bit, more channels, 96 kHz) is future work.
- **Scene library content.** The scene save/load mechanism ships, but with only one or two example scenes for testing — not a curated library.
- **Dynamics processing on the signal path.** The mixer model has slots reserved for dynamics, but no compressor/gate is wired into the small mixer's signal chain in v1.
- **Multiple solo modes.** Solo-in-place (SIP) only. AFL/PFL is reserved for the future.
- **EQ FFT spectrum visualization.** EQ visualization in v1 is curve-only (computed on the client from band parameters). FFT overlay streaming is future work.
- **Future example boards.** 8×8 and 16×16 mixer examples are out of scope for this epic — they consume the framework this epic builds.

## Why now

Three converging reasons:

1. **The current `main.cpp` is at the limit of what a single-file firmware can carry.** Adding faders, mutes, sends, EQ, scenes to it without structural change would produce a thousand-line tangle. Refactoring is cheaper now than later.
2. **OSCAudio (Jonathan Oakley) and OpenAudio_ArduinoLibrary (Chip Audette) both exist, are MIT-licensed, and do most of the heavy lifting we'd otherwise be writing from scratch.** The community has converged on these. Adopting them is cheap; reinventing them is expensive.
3. **Multi-channel USB audio for Teensy is sitting in unmerged forks** (mcginty, acumartini), and the right time to vendor the Teensy cores and start carrying those changes is *before* we depend on multi-channel USB at the application layer, not after.

## Open framing questions deferred to later docs

- How exactly the address tree is shaped → [02-osc-protocol.md](02-osc-protocol.md)
- How transport multiplexing works in detail → [03-transport.md](03-transport.md)
- What gets vendored and how → [04-dependencies.md](04-dependencies.md), [05-vendoring-strategy.md](05-vendoring-strategy.md)
- Concrete v1 mixer model for the small example → [06-mixer-model-v1.md](06-mixer-model-v1.md)
- Sequencing and milestones → [07-spike-plan.md](07-spike-plan.md), [08-roadmap.md](08-roadmap.md)
