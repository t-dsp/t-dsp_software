# 01 — Architecture

## The four-layer stack

The firmware is organized into four layers with strict separation of concerns. The discipline of keeping these layers distinct is the thing that makes the framework scale to bigger examples and more features without rewriting.

```
┌─────────────────────────────────────────────────────────────┐
│ 4. Transport layer                                          │
│    SLIPEncodedUSBSerial   (v1)                              │
│    + EthernetUDP          (v2 if magjack)                   │
│    + OSCQuery / mDNS      (v3 — discoverability)            │
├─────────────────────────────────────────────────────────────┤
│ 3. OSC dispatch layer                                       │
│    Address tree, pattern matching, argument parsing,        │
│    subscription management, echo/reply, rate limiting,      │
│    CLI shim (dotted-path → OSCMessage).                     │
├─────────────────────────────────────────────────────────────┤
│ 2. Mixer model                                              │
│    Channels, strips, buses, sends, scenes. Pure data.       │
│    Knows nothing about Teensy Audio Library or OSC.         │
├─────────────────────────────────────────────────────────────┤
│ 1. Signal graph                                             │
│    Teensy Audio Library (F32) objects: mixers, biquads,     │
│    analyzers. Bound to the model by setters — the model     │
│    is the source of truth, this layer is the implementation.│
└─────────────────────────────────────────────────────────────┘
```

### Why these layers in this order

The critical discipline: **the mixer model is the source of truth**, not the Teensy Audio Library objects.

When an OSC message arrives at the dispatcher, it mutates the model. A binding step then writes through to the audio graph. Why this matters:

- **Scene recall** is just "deserialize a model and re-apply the binding."
- **Multi-client sync** is just "echo model changes back to all subscribers."
- **Undo** is just "restore the previous model snapshot."
- **Adding a new client transport** is just "another consumer of the dispatch layer; no audio code changes."

If the OSC handlers wrote directly to the audio objects (skipping the model), every one of those features becomes a special case. With the model as the source of truth, they're all variations of "modify the model, run the binding."

## Library/example split

```
lib/
├── TAC5212/                       (NEW — to be extracted from projects/.../src/tac5212_regs.h)
├── OSCAudio/                      (vendored — Jonathan Oakley)
├── OpenAudio_ArduinoLibrary/      (vendored — Chip Audette)
├── Audio/                         (vendored — PaulStoffregen)
├── teensy_cores/                  (vendored — PaulStoffregen)
└── TDspMixer/                     (NEW — written by us)
    └── src/
        ├── MixerModel.h/cpp           channels, buses, faders, EQ params, sends
        ├── SignalGraphBinding.h/cpp   binds MixerModel → F32 audio objects
        ├── OscDispatcher.h/cpp        address-tree dispatch, pattern matching
        ├── SlipOscTransport.h/cpp     SLIP-over-USB-CDC framing + I/O
        ├── DottedCliShim.h/cpp        50-line text → OSCMessage shim
        ├── MeterEngine.h/cpp          AudioAnalyzePeak/RMS → blob streams
        ├── SubscriptionMgr.h/cpp      thin wrapper over OSCSubscribe
        ├── SceneStore.h/cpp           model ⇌ JSON on SD
        └── CodecPanel.h               interface for codec-specific OSC subtrees

projects/
├── t-dsp_tac5212_audio_shield_adaptor/   ← small mixer example (this epic)
│   └── src/
│       ├── main.cpp                       ~150 lines — wiring only
│       └── Tac5212Panel.h/cpp             codec-specific OSC subtree
├── t-dsp_8x8_example/                    (future, same lib, bigger config)
└── t-dsp_16x16_example/                  (future)
```

**The discipline:** anything that's not chip-specific lives in `TDspMixer`. Each example's `main.cpp` should declare how many channels, instantiate the codec driver, configure the per-channel binding to the codec's I/O slots, plug in the `CodecPanel` subclass, and call `framework.run()`. If interesting logic appears in `main.cpp`, it probably belongs in the library.

## The load-bearing principle: mixer surface vs Audio Library config surface

This is the most important architectural distinction in the entire epic. It is easy to lose sight of because both layers speak OSC, both run on the same dispatcher, and both can modify audio behavior. **They are not the same thing and they must not be conflated.**

### TDspMixer is the mixer surface

- **Audience:** live audio engineer running a show, or any client UI that targets pro audio engineers (Open Stage Control, TouchOSC, future Tauri client).
- **Address shape:** X32-flavored, mixer-domain. `/ch/01/mix/fader`, `/bus/01/mix/on`, `/main/st/mix/fader`, `/codec/tac5212/adc/1/pga`.
- **What it exposes:** mixer concepts. Channels, buses, faders, mutes, sends, EQ, scenes, meters.
- **Stability:** this is the contract. Changes are versioned, documented, and migrate clients.

### OSCAudio's `/teensy*/audio/` is the Audio Library config surface

- **Audience:** firmware developer at the bench, debugging the signal graph.
- **Address shape:** object-and-method. `/teensy1/audio/mainMixL/g f 0.5` directly invokes `mainMixL.gain(0, 0.5)`.
- **What it exposes:** Teensy Audio Library object methods. Whatever's instantiated and wrapped in `OSC<Class>`.
- **Stability:** unstable, undocumented, not for end-user clients. Compiled in only when `OSCAUDIO_DEBUG_SURFACE` is defined.

### Why the distinction is load-bearing

A naive design would say "we have OSC, so let's just route fader changes through `/teensy1/audio/mainMixL/g`." That breaks down immediately because:

- A fader change isn't *just* one `gain()` call. It might affect left and right (stereo channel), apply pan, update solo state, dirty the meter, and echo to subscribed clients. The mixer surface owns that orchestration.
- The audio object addresses leak implementation. Tomorrow we might rewire `ch/01` through different mixer objects, or move EQ from one biquad chain to another. The OSC address an engineer types should not change because we refactored the audio graph.
- An engineer thinks "channel 1 fader," not "AudioMixer4 instance number 7, slot 0." The engineer's mental model is the contract.
- Generic Audio Library exposure is great for *poking at the graph during development*. It's terrible as the thing a paid engineer types into during a show.

**The rule:** when adding a feature, ask "is this something a mixing engineer does during a show, or something a firmware developer does at the bench?" That decides which surface owns it. Mixer features go in TDspMixer's handlers, which call audio object methods directly. Debug-only features come for free via OSCAudio's `/audio` namespace.

The two surfaces coexist on the same wire. They are routed by the dispatcher based on address prefix. They never overlap.

## The CodecPanel pattern

Each example board has codec-specific settings (preamp gain, input mode, phantom power, output driver type, raw register pokes). These don't belong in TDspMixer — every example would need different ones. Instead:

- TDspMixer defines an interface `CodecPanel` with a single virtual method: `void route(OSCMessage& msg, int addrOffset, OSCBundle& reply)`.
- Each example's project directory implements its own subclass: `Tac5212Panel`, `AK4558Panel`, `WM8731Panel`, etc.
- The example's `main.cpp` instantiates the panel and registers it with the dispatcher under `/codec/<model>/`.
- The library never knows what codec is in use. The codec panel never knows about the mixer model.

This is a clean dependency boundary: TDspMixer depends on no specific codec; codec panels depend on no specific mixer model.

The codec panel can call methods on the codec library directly (e.g. `tac5212.adc(1).setPga(6.0f)`). It doesn't have to go through any abstraction. The only constraint is that its OSC subtree is namespaced under `/codec/<model>/` so clients can render it as a separate panel in their UI.

## Constraints from Teensy

Some Teensy-specific constraints shape the architecture:

- **`-fno-rtti`.** Teensy is built without RTTI, so `dynamic_cast` does not work. Every layer must use compile-time type knowledge. The `SignalGraphBinding` declares its concrete F32 audio object types as members of channel classes; no runtime type-check fallbacks.
- **No exceptions.** `-fno-exceptions` is also typical. Error handling is return-code based or via `OSCBundle` reply messages.
- **Single-threaded loop.** Audio runs in DMA + ISR; everything else runs in `loop()`. SLIP/OSC reads, dispatcher runs, model mutates, binding writes through, all on the main loop. No mutexes needed because there's no contention. **However:** OSC writes and `Serial.println` calls must stay on the main loop to avoid byte interleaving mid-frame. Nothing OSC-related from ISRs.

## Where each piece lives — quick reference

| Concern | Layer | Owner |
|---|---|---|
| `/ch/01/mix/fader` handler | OSC dispatch | `lib/TDspMixer/OscDispatcher` |
| The actual fader value | Mixer model | `lib/TDspMixer/MixerModel` |
| Updating `AudioMixer4_F32::gain()` | Signal graph binding | `lib/TDspMixer/SignalGraphBinding` |
| `/codec/tac5212/adc/1/pga` handler | Codec panel | `projects/<board>/Tac5212Panel` |
| Calling `TAC5212::setPga()` | Codec driver | `lib/TAC5212/` (to be extracted from current `projects/.../src/tac5212_regs.h`) |
| SLIP frame parsing | Transport | `lib/TDspMixer/SlipOscTransport` |
| Multiplexed text input parsing (CLI shim) | OSC dispatch | `lib/TDspMixer/DottedCliShim` |
| `/teensy1/audio/...` debug routes | Debug surface | `lib/OSCAudio` (vendored) |
| Subscription / metering streams | OSC dispatch | `lib/TDspMixer/MeterEngine` + `lib/OSCAudio/OSCSubscribe` |
| Scene save/load | Mixer model | `lib/TDspMixer/SceneStore` |
| USB descriptors, multi-channel | Teensy core | `lib/teensy_cores/teensy4/` (vendored, modified) |
