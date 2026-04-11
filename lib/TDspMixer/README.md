# lib/TDspMixer/

Shared mixer framework for the T-DSP project family. Implements the
mixer model, signal graph binding, OSC dispatcher, SLIP transport,
meter engine, and codec panel interface. Each example project
(currently only `projects/t-dsp_tac5212_audio_shield_adaptor/`) consumes
this library and provides a codec-specific `CodecPanel` subclass.

## MVP v1 scope

This library is shipping a **stripped MVP** of the full framework
defined in `planning/osc-mixer-foundation/08-roadmap.md`. See
`~/.claude/memory/decisions_mvp_v1_scope.md` for what's in and out.

**In MVP v1:**

- Per-channel fader, mute (`mix/on`), solo (solo-in-place)
- Per-channel stereo linking for 3 pairs (USB, Line, PDM mic)
- Per-channel high-pass filter (single biquad, configurable on/off + cutoff)
- Per-channel name (user-settable label)
- Main stereo output fader + mute
- Windows USB feature unit slider as a bypassable pre-main attenuator
  (`/main/st/hostvol/{enable,value}`)
- Input meters (peak + RMS, 30 Hz, blob format)
- Codec panel interface (`CodecPanel` base class, subclass lives in
  project)
- SLIP-OSC transport over USB CDC, multiplexed with plain text debug
  and dotted-path CLI input

**Explicitly NOT in MVP v1** (clean insertion points for later
additions):

- 4-band parametric EQ per channel
- Buses and sends (channels go straight to main)
- Pan
- Scenes / snapshots (no SceneStore)
- Dynamics (compression, gating)

## Architecture

Four layers, in order from audio hardware to client:

1. **Signal graph** — concrete Teensy Audio Library objects
   (`AudioMixer4`, `AudioAmplifier`, `AudioFilterBiquad`, etc.) created
   by the example's `main.cpp`. MVP uses stock I16 audio library; F32
   migration is a clean follow-on.
2. **Mixer model** (`MixerModel`) — plain data. Channels + main + some
   config. Source of truth. OSC handlers mutate this, binding reads it.
3. **Signal graph binding** (`SignalGraphBinding`) — holds pointers to
   the audio objects and pushes model values into them via
   `applyChannel(n)`, `applyMain()`, etc. The binding is
   type-specific (AudioMixer4 etc.), not type-parameterized.
4. **OSC dispatch** (`OscDispatcher` + `SlipOscTransport` +
   `MeterEngine`) — reads SLIP-framed OSC frames from USB CDC, routes
   to mixer handlers or the registered `CodecPanel`, echoes changes to
   subscribed clients, and streams meter blobs at 30 Hz.

Outbound path: client OSC message → `OscDispatcher::route` → mutate
model → call binding → append echo to reply bundle → transport flushes
bundle.

Inbound path: audio changes state → binding pushes into audio objects →
next audio frame applies the new gains.

## Usage

```cpp
#include <TDspMixer.h>
#include <Audio.h>

// Concrete audio objects (project-specific)
AudioInputUSB       usbIn;
AudioInputTDM       tdmIn;
AudioMixer4         mixMainL;
AudioMixer4         mixMainR;
AudioAmplifier      mainAmpL;
AudioAmplifier      mainAmpR;
AudioFilterBiquad   hpfCh1;
// ... etc.

// TDspMixer components
tdsp::MixerModel          model;
tdsp::SignalGraphBinding  binding;
tdsp::OscDispatcher       dispatcher;
tdsp::SlipOscTransport    transport;
tdsp::MeterEngine         meters;
// Project-specific codec panel subclass
Tac5212Panel              codecPanel(codec /* from lib/TAC5212/ */);

void setup() {
    // Wire the audio graph (AudioConnection declarations, codec init, etc.)
    // ...

    // Register audio objects with the binding
    binding.setModel(&model);
    binding.setChannel(1, &mixMainL, 0, &hpfCh1);
    // ... for each channel
    binding.setMain(&mainAmpL, &mainAmpR);
    binding.applyAll();

    // Wire the dispatcher
    dispatcher.setModel(&model);
    dispatcher.setBinding(&binding);
    dispatcher.registerCodecPanel(&codecPanel);

    // Transport callbacks
    transport.begin();
    transport.setOscMessageHandler(/* callback */, /* userData */);

    // Meters
    meters.setDispatcher(&dispatcher);
    meters.setChannel(1, &peakCh1, &rmsCh1);
    // ... for each channel
}

void loop() {
    transport.poll();

    // Poll meters and flush blob if ready
    OSCBundle reply;
    if (meters.tick(reply)) {
        transport.sendBundle(reply);
    }
}
```

## Files

- `src/TDspMixer.h` — umbrella include
- `src/MixerModel.h` / `.cpp` — channel + main state data
- `src/SignalGraphBinding.h` / `.cpp` — maps model to audio objects
- `src/SlipOscTransport.h` / `.cpp` — SLIP-OSC + plain-text CDC I/O
- `src/OscDispatcher.h` / `.cpp` — mixer-domain + codec-panel routing
- `src/MeterEngine.h` / `.cpp` — periodic peak/RMS sampling + blob emission
- `src/CodecPanel.h` — abstract base class for project-specific codec panels

## Design rules (from planning docs + memory)

- `/codec/<model>/...` addresses belong to `CodecPanel` subclasses,
  never to TDspMixer. Codec libraries handle hardware specifics.
- Gain / level / fader / mute / solo are TDspMixer concerns, never
  codec-library concerns.
- Per-channel OSC leaves must not have chip-global side effects.
- Faders are normalized 0..1 on the wire (X32 convention).
- `mix/on` is the X32 idiom: `on = 0` means muted.
