# 07 — Spike Plan

## Why spikes (and why three)

A "spike" in this epic is a small, throwaway-or-promotable codebase whose only job is to validate one architectural assumption end-to-end before we commit to building on top of it. Spikes are not deliverables; they're risk reduction.

We have three independent risk areas to validate:

1. **The library and transport foundation.** Can OSCAudio + OpenAudio + the stock Audio library + the cores overlay all coexist in a single PlatformIO build? Does SLIP-OSC over USB CDC actually multiplex with debug text the way the design says? Do the X32 mixer-domain handlers and the OSCAudio debug surface really coexist on the same dispatcher?

2. **Multi-channel USB audio.** Do the existing forks (mcginty, acumartini) work on our hardware? Can we make a 4 or 8 channel USB device enumerate on Windows and stream audio? What core-file changes do we actually need?

3. **USB Audio Class feature unit on the input terminal.** Can we make Windows show a recording volume slider that the Teensy can read? What descriptor changes and SET_CUR callbacks are needed?

Each risk area is **independent** of the others. If we tried to validate all three in one big integration, a failure could be attributed to any of them and we'd have to debug a tangled mess. Three small spikes give us **clean attribution** — when something breaks, we know exactly which assumption was wrong.

The spikes are **sequenced**, not parallel: Spike 1 must be green before Spike 2 starts, and Spike 2 must be green before Spike 3. Each spike provides the foundation the next builds on.

## Spike 1 — Foundation

**Goal:** validate that the OSC + F32 + library vendoring + cores overlay + SLIP multiplex foundation actually works on real hardware.

### What it builds

A minimal new project at `projects/spike_osc_foundation/`:

- A trivial F32 audio graph: `AudioInputUSB_F32` → `AudioMixer4_F32` → `AudioOutputUSB_F32`. Pure software loopback. **No TAC5212 hardware involvement.** The TAC5212 is intentionally excluded so a failure in this spike points at the OSC stack or the build, not at codec init code or I²C wiring.
- `SLIPEncodedUSBSerial` wrapping the USB CDC `Serial` for OSC framing.
- A `Serial.println("Spike booted")` heartbeat at startup and every 30 seconds, to prove plain text + SLIP coexistence over time.
- One **mixer-domain handler** at `/test/gain f` that directly calls `mainMix_F32.gain(0, value)`. This bypasses OSCAudio entirely — it's the proof that the X32 mixer-surface pattern works.
- One **OSCAudio passthrough handler** at `/teensy1/audio/...` that calls `OSCAudioBase::routeAll`. Instantiates one `OSCAudioAmplifier amp("amp")` (an I16 stock-Audio-Library wrapper, since no F32 wrappers exist yet) so OSCAudio has something to dispatch to. Proves the debug surface compiles and works.
- A dotted-path **CLI shim** that accepts `test.gain 0.5` from a serial monitor and routes it through the same dispatcher as the OSC frame would.

### What it validates

End-to-end checklist:

- [ ] All 4 vendored libraries (OSCAudio, OpenAudio, Audio, teensy_cores) compile together for Teensy 4.1 in PlatformIO.
- [ ] The cores overlay mechanism (`tools/cores_overlay.py` running as a `extra_scripts` hook) successfully replaces `framework-arduinoteensy/cores/teensy4/` with our `lib/teensy_cores/teensy4/`. Verified by either a binary diff against a baseline build, or by intentionally adding a marker comment in our cores copy and confirming it appears in the compiled binary.
- [ ] The `lib_ignore = Audio` mechanism makes PlatformIO use `lib/Audio/` instead of the framework-bundled Audio library. Verified by adding a marker in our copy and seeing it picked up.
- [ ] The F32 audio path actually runs (USB → F32 mixer → USB).
- [ ] SLIP-OSC frames over USB CDC work in both directions.
- [ ] Plain `Serial.println` text and SLIP frames coexist on the same CDC stream — the host-side Python harness reads both correctly.
- [ ] The mixer-domain handler (`/test/gain f`) actually changes the F32 mixer gain and the change is audible / measurable.
- [ ] The OSCAudio passthrough handler (`/teensy1/audio/amp/g f`) actually invokes `amp.gain()` via OSCAudio's dispatch.
- [ ] The CLI shim accepts `test.gain 0.5` typed in a serial monitor and produces the same effect as the OSC frame.
- [ ] The 0xC0 first-byte discriminator correctly routes between SLIP-OSC and CLI on the input side.
- [ ] Python test harness can drive both surfaces and assert PASS/FAIL.

### Files

```
projects/spike_osc_foundation/
├── platformio.ini             with lib_ignore = Audio, extra_scripts = ../../tools/cores_overlay.py
└── src/
    └── main.cpp               ~100 lines: F32 audio graph + dispatcher + handlers + CLI shim

tools/
├── cores_overlay.py           PlatformIO extra_scripts hook
├── vendor.py                  vendored.json manager
├── vendored.schema.json       JSON schema for the manifest
└── spike_osc_test.py          ~100 lines: pyserial + python-osc + SLIP, drives the spike

vendored.json                  manifest of all subtree pinned commits
```

### Out of scope for Spike 1

- TAC5212 hardware. No I²C, no codec init, no audio shield interaction. Pure software.
- Multi-channel USB. Stock stereo.
- The mixer model, channels, buses, EQ, scenes, meters, subscriptions. None of TDspMixer.
- F32 OSCAudio wrappers. The debug surface only exposes the I16 `OSCAudioAmplifier` instance for the smoke test.
- A real OSC address tree. Just `/test/gain` and `/teensy1/audio/amp/g`.
- The Tac5212Panel codec settings panel.

### Success criteria

Spike 1 is "green" when:

1. `pio run` produces a successful build for `projects/spike_osc_foundation`.
2. The binary uploads to a Teensy 4.1 and boots.
3. `python tools/spike_osc_test.py` runs against the connected Teensy and prints `PASS` for all assertions.
4. A serial monitor opened against the same port (after the test exits) shows the `Spike booted` and heartbeat messages.
5. Typing `test.gain 0.5` into the serial monitor changes the audio loopback gain.

### What we learn either way

- **If Spike 1 succeeds:** the foundation works; we can build TDspMixer on top with confidence. We have a baseline to compare future builds against.
- **If Spike 1 fails on the cores overlay:** we need a different mechanism. Investigate `platform_packages` with a vendored framework directory, or commit to vendoring the entire `framework-arduinoteensy` package.
- **If Spike 1 fails on `lib_ignore = Audio`:** we need a different way to make PlatformIO use our copy. Investigate alternatives like dropping `lib/Audio/library.json` files to assert priority.
- **If Spike 1 fails on the SLIP multiplex:** revisit transport assumptions. Maybe `Serial.print` and `SLIPSerial.write` interact in unexpected ways under USB CDC's framing.
- **If Spike 1 fails on OSCAudio dispatch:** read OSCAudio more carefully, possibly file an issue with Jonathan.

Each failure mode points at a specific subsystem.

## Spike 2 — Multi-channel USB audio

**Prerequisite:** Spike 1 is green.

**Goal:** validate that the vendored Teensy core can be modified to expose more than 2 channels of USB audio, and that the audio actually streams correctly on Windows.

### What it builds

A modified copy of `lib/teensy_cores/teensy4/` containing 4-channel USB audio support. The modifications are based on acumartini's PlatformIO patches if a public source is found, or on mcginty's `teensy4-studio-audio` fork if not. Files modified:

- `usb_desc.c` / `usb_desc.h` — change `bNrChannels` from 2 to 4, recalculate `wMaxPacketSize`, add additional `audio_block_t` slot declarations.
- `usb_audio.cpp` / `usb_audio.h` — expand RX/TX buffer sizes proportionally, modify the receive/transmit callbacks to allocate and manage 4 audio blocks per slot, update bitwise channel-interleaving logic.

Plus extensions to `lib/Audio/` (the vendored Audio library) so `AudioInputUSB` / `AudioOutputUSB` (and the F32 versions in OpenAudio) expose 4 input ports and 4 output ports instead of 2.

A new spike project `projects/spike_multichannel_usb/` that:

- Instantiates `AudioInputUSB_F32` with 4 channels.
- Routes each channel through an `AudioMixer4_F32` and back out via `AudioOutputUSB_F32`.
- Reports per-channel peak levels via SLIP-OSC.

### What it validates

- [ ] The modified core compiles cleanly.
- [ ] The cores overlay correctly applies the modified files.
- [ ] Windows enumerates the Teensy as a 4-channel sound card.
- [ ] DAW software (Reaper, Audacity) sees 4 input channels and 4 output channels.
- [ ] Audio plays back from the host to all 4 channels without crackling.
- [ ] Audio records from all 4 channels into the host without dropouts.
- [ ] Per-channel peak levels reported via OSC match the source signals.
- [ ] The transition from stereo (Spike 1) to 4-channel (Spike 2) is just core-file changes plus an Audio library extension — no application-level rework.

### Out of scope for Spike 2

- Going beyond 4 channels. mcginty's fork supports 8, but we validate 4 first as the minimum useful win. Going to 8 is a follow-on.
- 96 kHz / 24-bit. Stay at 48 kHz / 16-bit (UAC1 standard).
- USB Audio Class 2. Stay at UAC1.
- Mac / Linux compatibility. Validate on Windows first (the target host for the user's setup); other OSes are bonus.
- Integration with the small mixer model. The spike just proves the core changes work; integration into TDspMixer is part of the framework build.

### Success criteria

1. `pio run` produces a successful build for `projects/spike_multichannel_usb`.
2. Windows sees a 4-in / 4-out sound card on the Teensy's USB port.
3. Reaper or Audacity can record 4 simultaneous channels and play back 4 simultaneous channels for at least 5 minutes without dropouts.
4. Per-channel meter values reported via OSC match independently-measured signal levels.

## Spike 3 — USB Audio Class input feature unit

**Prerequisite:** Spike 2 is green.

**Goal:** validate that we can add a USB Audio Class feature unit on the *input* terminal so Windows shows a recording volume slider, and that the firmware can read its current value.

### Background

USB Audio Class 1 supports "feature units" — descriptor entries that declare available controls (volume, mute, bass, treble) on a terminal. The host (Windows / Mac / Linux) queries these and exposes them in the OS volume mixer.

The Teensy currently exposes a feature unit on the **output** (playback) terminal. That's why `usbIn.volume()` works — Windows sends a `SET_CUR` control request when the user moves the playback slider, and the Teensy stores the value for the firmware to read.

The Teensy does **not** currently expose a feature unit on the **input** (record) terminal. So Windows shows no slider for the recording volume on the Teensy's "microphone" device. For a pro mixer trying to behave like a real USB audio interface, this is a missing feature.

### What it builds

Modifications to `lib/teensy_cores/teensy4/`:

- `usb_desc.c` — add a feature unit descriptor on the input (microphone) terminal, declaring volume and mute controls.
- `usb_audio.cpp` — handle `SET_CUR` and `GET_CUR` control requests for the input feature unit, store the current value in a static variable accessible to the firmware.
- `usb_audio.h` — declare a public function like `AudioOutputUSB::volume()` (mirroring the existing `AudioInputUSB::volume()`) that returns the current input feature unit value.

A new spike project `projects/spike_input_feature_unit/` that:

- Sets up a multi-channel (4-channel from Spike 2) USB audio loopback.
- Polls the input feature unit value via the new accessor function.
- Reports the value over SLIP-OSC at `/test/input_volume f`.
- Prints the value on every change to plain text debug output.

### What it validates

- [ ] The modified `usb_desc.c` enumerates correctly on Windows.
- [ ] Windows Sound mixer shows a recording volume slider for the Teensy's microphone device.
- [ ] Moving the slider sends `SET_CUR` requests that the firmware receives.
- [ ] The firmware reads the current value via the new accessor and reports it correctly.
- [ ] The slider persists across reboots (or doesn't — depending on Windows behavior; just document what happens).
- [ ] No regression in playback volume monitoring (the existing `AudioInputUSB::volume()` still works).
- [ ] No regression in 4-channel audio streaming from Spike 2.

### Out of scope for Spike 3

- Mute control (just volume for v1; mute is straightforward to add later).
- Per-channel volume controls (just a single master volume on the input terminal).
- Notification of slider changes (firmware polls; pushed notifications are an optimization).
- Mac / Linux behavior. Validate on Windows.

### Success criteria

1. `pio run` produces a successful build for `projects/spike_input_feature_unit`.
2. Windows Sound > Recording Devices > Teensy microphone shows a working volume slider.
3. Moving the slider produces visible value changes in the firmware's OSC output and debug log.
4. The new functionality is in the vendored cores subtree as a clean modification, ready to PR back to upstream Teensy if Paul Stoffregen wants it.

## After the spikes

Once all three spikes are green, the foundation is fully validated. The framework build (TDspMixer library + small mixer example refactor) starts on top of a known-good base. Each future feature added to the framework can be tested in isolation without re-validating the foundation.

The spike projects themselves are kept in the repo as living examples — they document how to use each piece in isolation and serve as regression tests if the foundation needs to be re-verified after a major upstream library update.
