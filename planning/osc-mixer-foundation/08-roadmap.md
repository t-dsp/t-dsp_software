# 08 — Roadmap

## Milestones in order

This is the sequence of work for the epic. No time estimates — just the order in which things must happen and what each milestone produces.

### M1: Planning complete (current state)

- All planning documents in this folder written.
- Architecture decisions captured in memory and in the docs.
- Vendoring strategy decided.
- Spike scope defined.

**Output:** This planning folder. **Status: ✅ done.**

### M2: Vendoring bootstrap

- Run subtree adds for OSCAudio, OpenAudio_ArduinoLibrary, PaulStoffregen/Audio, PaulStoffregen/cores.
- Create `vendored.json` with pinned commits captured.
- Write `tools/vendor.py` with `status`, `verify`, `update`, `add`, `freeze`, `contribute` subcommands.
- Write `tools/cores_overlay.py` PlatformIO extra_scripts hook.
- Write `tools/vendored.schema.json`.

**Output:** Four subtrees in `lib/`, manifest at the repo root, working vendor tool. The repo can now reproduce the vendored state from scratch via `vendor.py freeze` / `verify`.

### M3: Spike 1 — Foundation validation

See [07-spike-plan.md](07-spike-plan.md#spike-1-foundation) for full scope. Summary:

- New project `projects/spike_osc_foundation/`.
- F32 USB→USB loopback, SLIP-OSC transport, multiplexed text debug, dotted-path CLI shim.
- One mixer-domain handler, one OSCAudio passthrough handler.
- Python test harness `tools/spike_osc_test.py`.

**Output:** A working spike that proves the foundation. Green-light to start TDspMixer.

### M4: Upstream engagement (Jonathan, Chip, Paul)

Three parallel upstream relationships, each with at least one concrete contribution to lead with.

**Jonathan Oakley (h4yn0nnym0u5e/OSCAudio):**

- Open a GitHub issue on `h4yn0nnym0u5e/OSCAudio` introducing the T-DSP project.
- Link to this planning folder and the Spike 1 source code.
- Offer to contribute F32 wrapper extensions, the SLIP-USB-CDC + multiplexed-text example.
- Raise multi-channel USB merge status (he was named in the 2022 forum thread as a potential collaborator on the upstream merge).
- Establish a working relationship for ongoing collaboration.

**Chip Audette (chipaudette/OpenAudio_ArduinoLibrary):**

- PR the `setPeakingEq` method addition to `AudioFilterBiquad_F32.h`. T-DSP needs it for the parametric EQ band in the channel strip; the rest of the RBJ cookbook is already implemented in the file but peaking EQ was missing. Math verified against musicdsp.org's RBJ cookbook reference.
- Optionally, PR a top-level `LICENSE` file (the MIT terms are currently only in source headers, and GitHub's SPDX detector returns 404 on the repo as a result).
- Workflow: Jay opens the PR under his handle. Update `vendored.json` `OpenAudio_ArduinoLibrary.localPatches[0].upstreamPr` with the PR URL once filed. Once merged, bump `pinnedCommit` to a SHA that contains the merge and remove the patch from `localPatches`.

**Paul Stoffregen (PaulStoffregen/cores):**

- PR the `<type_traits>` include addition to `cores/teensy4/IntervalTimer.h`. Real upstream completeness bug — the file uses `std::is_arithmetic_v` etc. but doesn't include the header, so it only compiles when something else transitively pulls type_traits in. PlatformIO's standalone compile of `IntervalTimer.cpp` fails as a result. Trivial one-line fix.
- Possibly: raise the platform-teensy 5.1.0 toolchain mismatch (gcc 5.4.1 bundled with framework cores using C++17 features) for awareness. The right long-term fix is for `platform-teensy` to bundle a newer toolchain by default; T-DSP's `tools/cores_overlay.py` PATH-prepend trick should not be necessary.
- Workflow: Jay opens the PR under his handle. Update `vendored.json` `teensy_cores.localPatches[0].upstreamPr` with the PR URL once filed.

**Output:** Three live upstream conversations. None are blockers for downstream work, but all three move the project from "carries local patches indefinitely" to "vendored libraries reflect upstream + small targeted improvements that benefit everyone."

### M5: TDspMixer library skeleton

- Create `lib/TDspMixer/` with empty/skeleton headers for all the components: `MixerModel`, `SignalGraphBinding`, `OscDispatcher`, `SlipOscTransport`, `DottedCliShim`, `MeterEngine`, `SubscriptionMgr`, `SceneStore`, `CodecPanel`.
- All headers compile.
- Stub implementations return success / no-op so the library is buildable.
- A minimal example sketch that consumes the library and proves it links.

**Output:** A buildable but empty library with the right shape. Each subsequent milestone fills in one component.

### M6: MixerModel + SignalGraphBinding

- Implement `MixerModel` as plain data (channels, buses, faders, EQ params, sends, solo state).
- Implement `SignalGraphBinding` that maps a `MixerModel` to F32 audio objects (`AudioMixer4_F32`, `AudioFilterBiquad_F32`, etc.).
- Wire the small mixer's 5 channels and 2 buses into the model and binding.
- Verify by setting model values directly (no OSC yet) and confirming audio behavior changes.

**Output:** A working in-memory mixer that you can drive by calling C++ methods. No OSC yet.

### M7: OscDispatcher + the X32 mixer surface

- Implement `OscDispatcher` with the `/ch /bus /main /meters /-snap /info /sub` address tree from [02-osc-protocol.md](02-osc-protocol.md).
- Each handler mutates the `MixerModel` and triggers `SignalGraphBinding` to apply changes.
- Echo semantics: every write is echoed to subscribed clients.
- Verify by sending OSC messages over the SLIP transport and observing audio + echoes.

**Output:** A working X32-style mixer control surface. Engineers can now drive the small mixer over OSC.

### M8: MeterEngine + SubscriptionMgr

- Implement `MeterEngine` polling per-channel and per-bus meters at 30 Hz.
- Implement `SubscriptionMgr` (thin wrapper over `OSCSubscribe`) handling the `/sub addSub / renew / unsubscribe` lifecycle.
- Pack meters into OSC blobs at `/meters/input` and `/meters/output`.
- Verify by subscribing from a Python harness and observing meter blobs.

**Output:** Working meter streaming. Open Stage Control can now show real meters driven by the firmware.

### M9: DottedCliShim + CLI integration

- Implement the ~50-line dotted-path → OSC shim.
- Wire it into the transport so input bytes are routed by first-byte discriminator: `0xC0` → SLIP-OSC, ASCII → CLI.
- Verify by typing CLI commands in a serial monitor and observing same effect as OSC frames.

**Output:** The CLI escape hatch is live.

### M10: SceneStore

- Implement JSON serialization of `MixerModel` to/from SD card.
- Wire `/-snap/save` and `/-snap/load` handlers.
- Implement `/-snap/list`.
- Verify with a save/load round-trip via OSC.

**Output:** Scene save/load working end-to-end with one or two test scenes.

### M11: Extract TAC5212 codec library + Tac5212Panel

This milestone has two parts because the codec register library doesn't exist yet as a proper library — the current state is a single header at `projects/t-dsp_tac5212_audio_shield_adaptor/src/tac5212_regs.h` containing register definitions, and `main.cpp` does I²C reads/writes directly using those constants.

**Part A — Extract `lib/TAC5212/`:**

- Create `lib/TAC5212/` with `library.json`, `src/TAC5212.h`, `src/TAC5212.cpp`, `src/tac5212_regs.h` (moved from the project).
- Wrap the existing register pokes in a proper class API: `TAC5212::reset()`, `TAC5212::wake()`, `TAC5212::setAdcMode(channel, mode)`, `TAC5212::setAdcPga(channel, dB)`, `TAC5212::setOutputMode(out, mode)`, `TAC5212::regSet(reg, value)`, `TAC5212::regGet(reg)`, etc.
- Update the existing `projects/t-dsp_tac5212_audio_shield_adaptor/src/main.cpp` to use the library instead of inline register pokes (this is a no-functional-change refactor).
- Verify the existing project still builds and runs identically.

**Part B — Implement `Tac5212Panel`:**

- Implement `Tac5212Panel : public CodecPanel` in the project directory.
- Map `/codec/tac5212/...` addresses to `TAC5212` library method calls.
- Wire into the small mixer example's `main.cpp`.
- Verify by setting codec parameters via OSC.

**Output:** A proper `lib/TAC5212/` codec library exists, the codec panel is live. Combined with M10, the small mixer is feature-complete for v1.

### M12: Small mixer example refactor

- Rewrite `projects/t-dsp_tac5212_audio_shield_adaptor/src/main.cpp` as a thin wiring file:
  - Instantiate TDspMixer
  - Declare 5 channels and 2 buses
  - Configure binding to TAC5212 hardware I/O
  - Plug in `Tac5212Panel`
  - Run
- Delete the old terse-character text command interface (`m`, `l`, `u50`, etc.) — replaced by the OSC mixer surface and the dotted-path CLI.
- Update the project README to point at the new control mechanism.

**Output:** The existing small mixer example, now built on the framework. Aim for ~150 lines of `main.cpp`.

### M13: Open Stage Control preset

- Write an Open Stage Control JSON preset for the small mixer.
- Layout: 5 fader strips with mute/solo, 4-band EQ visualization (curve only), TAC5212 codec settings tab.
- Commit to `projects/t-dsp_tac5212_audio_shield_adaptor/tools/open_stage_control_small_mixer.json`.
- Verify by launching Open Stage Control with the preset and mixing audio.

**Output:** A working visual control surface for the small mixer. v1 of the epic is complete.

### Parallel track: web dev surface (Chromium-only WebSerial)

A second client surface, [`projects/t-dsp_tac5212_audio_shield_adaptor/tools/web_dev_surface/`](../../projects/t-dsp_tac5212_audio_shield_adaptor/tools/web_dev_surface/), runs alongside the Open Stage Control preset and serves a different user. It is a small Vite + vanilla TypeScript SPA that opens the Teensy's USB CDC port via the WebSerial API, runs SLIP framing and OSC encoding/decoding in the browser, and exposes channel strips, a main bus, the TAC5212 codec panel, a serial console pane, and a raw OSC input field. Chromium-only, intentionally non-portable, scoped specifically to this board — it is a development and validation tool, not a product, and not a replacement for M13. It becomes useful as soon as M7 (OscDispatcher) lights up, grows alongside M8 (meter subscriptions) and M11 (codec panel), and reaches feature-complete for its scope when M11 lands. It does not block any milestone in the linear M1–M16 sequence; treat it as a parallel track that earns its keep by shortening the firmware iteration loop. A bespoke *product* surface (Tauri or full-featured cross-browser client, portable across boards) is still parked as a downstream epic.

### M14: Spike 2 — Multi-channel USB

See [07-spike-plan.md](07-spike-plan.md#spike-2-multi-channel-usb-audio) for full scope.

This is parallelizable with M5–M13 in principle, but the team is one person. Sequenced after M13 in practice. Multi-channel USB extends the *foundation*, not the small mixer; the small mixer stays at stereo for v1.

**Output:** 4-channel USB audio working on Teensy 4.1 via the vendored cores. Ready to integrate into future examples.

### M15: Spike 3 — USB input feature unit

See [07-spike-plan.md](07-spike-plan.md#spike-3-usb-audio-class-input-feature-unit) for full scope.

**Output:** Windows shows a recording volume slider on the Teensy's microphone device, readable by firmware. Improves the small mixer's "behaves like a real USB sound card" credibility.

### M16: Epic complete; ready for next epics

The OSC mixer foundation is shipped. Future epics build on top:

- **8×8 mixer example** — second example, exercises the framework at higher channel count.
- **16×16 mixer example** — third example.
- **Tauri host-side control surface** — replaces Open Stage Control with a bespoke Rust + WebView client.
- **Dynamics processing** — wire `AudioEffectCompressor_F32`, `AudioEffectNoiseGate_F32` into the channel signal chain. Expose `/ch/NN/dyn/...` addresses.
- **EQ FFT spectrum overlay** — stream FFT bin values via blob subscription so the client can overlay a real-time spectrum on the EQ curve.
- **MIDI control surface integration** — Mackie Control Universal protocol over USB MIDI for hardware control surfaces (X-Touch, etc.).
- **OSCQuery + Ethernet UDP** — second-generation transport with self-describing endpoints, for LAN-wide multi-client control.

## Open questions

These are unresolved at the start of the epic and need answers before the milestone they affect.

### Open question 1: Does the audio shield adaptor PCB have an Ethernet magjack?

**Affects:** M16+ (future Ethernet/UDP transport). Not blocking the epic.

**Resolution path:** Inspect the PCB or schematic. If yes, plan UDP transport for the next epic. If no, the small mixer stays USB-only and a future hardware revision adds it.

### Open question 2: Does CNMAT/OSC ship with PlatformIO's Teensy framework?

**Affects:** M2 (vendoring bootstrap). May need to add a fifth subtree.

**Resolution path:** Test in Spike 1. If `<OSCMessage.h>` is found by the build, no action. Otherwise, vendor `lib/CNMAT_OSC/`.

### Open question 3: Does `lib_ignore = Audio` actually work for replacing the framework's Audio library?

**Affects:** M2, M3 (vendoring + Spike 1).

**Resolution path:** Validated in Spike 1. If it doesn't work, fall back to dropping a `library.json` in `lib/Audio/` to assert priority, or escalate to vendoring `framework-arduinoteensy` entirely.

### Open question 4: Is acumartini's multi-channel USB patchset publicly available?

**Affects:** M14 (Spike 2).

**Resolution path:** Search the PJRC forum thread for direct links. PM acumartini if no public link. Alternative: port mcginty's `teensy4-studio-audio` Makefile-based code into the vendored cores manually.

### Open question 5: Has Jonathan made progress on canonicalizing multi-channel USB since the 2022 forum thread?

**Affects:** M4 (engagement) and M14 (Spike 2).

**Resolution path:** Ask him directly when opening the M4 GitHub issue.

### Open question 6: How many F32 OSCAudio wrappers should we contribute back?

**Affects:** M4 (engagement). Not blocking.

**Resolution path:** After M6–M8 we'll know exactly which OpenAudio classes the small mixer uses. Generate or hand-write wrappers for those, PR to OSCAudio.

### Open question 7: Stereo-link semantics for the mixer model

**Affects:** M6 (MixerModel implementation). Already mostly resolved but worth confirming during implementation.

**Specifically:** when the user writes to the linked-pair "slave" channel directly (e.g. `/ch/02/mix/fader` when ch/01+02 are linked), do we:
- (a) Mirror the write to ch/01 and the binding handles both, or
- (b) Reject the write with an error reply, or
- (c) Treat ch/02 as "the same channel" — accept the write and propagate to ch/01?

X32 does (c). We should match.

### Open question 8: Codec panel parameter validation

**Affects:** M11.

When a client sends `/codec/tac5212/adc/1/pga f 100.0`, and the TAC5212's actual gain range is -12 dB to +40 dB, do we:
- (a) Clamp the value silently and apply.
- (b) Reject with an error reply and don't apply.
- (c) Apply the request literally and let the codec library clamp internally.

Recommendation: (b) for codec settings (engineers should know when their input is out of range), but (a) for mixer faders (users expect "fader to maximum" to work without error). Confirm during implementation.

## Future work parked outside this epic

These are explicitly not in this epic and will be addressed in follow-on work:

- **Dynamics processing.** `/ch/NN/dyn/...` slots reserved in the address tree but no implementation in v1.
- **EQ FFT visualization.** Curve-only in v1; spectrum overlay deferred.
- **AFL / PFL solo modes.** SIP only in v1.
- **DCAs / VCAs (control groups).** Not in v1.
- **Talkback / oscillator / signal generator.** Not in v1.
- **MIDI control surface integration.** Future epic.
- **USB Audio Class 2.** Future work after Spike 3.
- **96 kHz / 24-bit operation.** Future work.
- **Mac / Linux compatibility for USB audio.** Validate Windows first; other OSes are bonus tracks.
- **Multi-Teensy clustering.** Out of scope.
- **Networked AES67 / Dante / AVB.** Out of scope.
- **Tauri host-side GUI.** Future epic; Open Stage Control is the validation client for v1.

## Reading the roadmap

- Milestones M1–M3 are foundation work; nothing user-visible until M3 when the spike runs.
- M4 (engagement) is parallel — start the conversation with Jonathan early so it's underway by the time we have something to show.
- M5–M11 build the framework piece by piece. Each one is verifiable in isolation before moving on.
- M12–M13 are the deliverable: a refactored small mixer example and a working Open Stage Control client.
- M14–M15 extend the foundation for future examples and pro-audio credibility on Windows.
- M16 closes the epic and opens the next.
