# 06 — Small Mixer Model v1

## Scope

The "small mixer" is the first example built on TDspMixer. It runs on the existing T-DSP TAC5212 Audio Shield Adaptor (Teensy 4.1 + TAC5212 codec module). Its job is to exercise enough of the framework that we know all the building blocks work, while staying small enough that the example's `main.cpp` is genuinely a wiring file.

This document defines the v1 mixer model concretely: what channels exist, what buses exist, what the signal chain looks like per channel, what's in scope and what isn't. The point is to nail this down before writing code so there's no ambiguity later.

## Channels (5 logical inputs)

| # | Address | Source | Type | Notes |
|---|---|---|---|---|
| 01 | `/ch/01` | USB playback L | Stereo-link L | Linked to ch/02 |
| 02 | `/ch/02` | USB playback R | Stereo-link R | Linked to ch/01 |
| 03 | `/ch/03` | TAC5212 ADC IN1 | Stereo-link L | Linked to ch/04 — line in L |
| 04 | `/ch/04` | TAC5212 ADC IN2 | Stereo-link R | Linked to ch/03 — line in R |
| 05 | `/ch/05` | TAC5212 PDM mic | Mono | Onboard PDM microphone |

Stereo-linked pairs share fader, mute, solo, EQ, send levels; pan is independent (defaulted to hard L/R for the link). Either channel of a pair can be addressed independently for clients that aren't link-aware — writes are mirrored to the linked channel automatically.

The PDM mic is a single mono channel.

## Buses (2)

| # | Address | Purpose | Notes |
|---|---|---|---|
| Main | `/main/st` | Headphone master (stereo) | What the engineer hears in the headphones |
| Aux 01 | `/bus/01` | USB capture (stereo) | What gets recorded by the host computer |

Channel sends route audio to buses. Each channel has a send level to each bus (`/ch/NN/mix/MM/level`). The main bus also has the global fader fed by all channels' main send.

Why two buses for the small mixer:
- **Main** is mandatory — the engineer needs to hear something.
- **Aux 01 (capture)** is what makes this useful as a "USB sound card mixer." The host can record any combination of inputs.

A real pro mixer has many more buses (multiple aux sends, subgroups, matrix mixes, mains LRC). The framework supports this; the small mixer simply doesn't use them. Future examples (8×8, 16×16) will declare more buses.

## Per-channel signal chain

In order from input to output:

```
Source → HPF → 4-band parametric EQ → Fader → Mute → Pan → Sends to buses
```

### Source

Whatever drives the channel. For USB channels, `AudioInputUSB_F32`. For line in channels, the TAC5212 ADC TDM slots routed through `AudioInputI2S_F32` (or the TDM equivalent). For the PDM channel, the codec's PDM channels routed through TDM. All sources land in F32 by the time they enter the channel strip.

### HPF (high-pass filter)

A separate biquad stage *before* the EQ chain. Independent on/off and frequency control:

- `/ch/NN/preamp/hpf/on i 0|1`
- `/ch/NN/preamp/hpf/f f Hz`

Default frequency 80 Hz, default off. This matches X32 convention — HPF is a pre-EQ stage, not band 1 of the EQ. Implementation: one `AudioFilterBiquad_F32` instance per channel (or per linked pair) configured as a 2nd-order high-pass.

### 4-band parametric EQ

Four biquad stages cascaded. Default band assignment:

| Band | Default type | Default freq | Default Q |
|---|---|---|---|
| 1 | Low shelf | 100 Hz | 0.7 |
| 2 | Parametric | 500 Hz | 1.0 |
| 3 | Parametric | 2000 Hz | 1.0 |
| 4 | High shelf | 8000 Hz | 0.7 |

All bands default to 0 dB gain (no effect). The user can change any band's type (`lcut`, `lshelf`, `peq`, `hshelf`, `hcut`) freely; the binding reconfigures the underlying biquad coefficients.

Implementation: one `AudioFilterBiquad_F32` per channel with 4 stages. The `SignalGraphBinding` translates `/ch/NN/eq/B/{type,f,g,q}` parameter changes into `setLowShelf`, `setHighShelf`, `setBandpass`, etc. coefficient calls.

EQ on/off (`/ch/NN/eq/on`) is a global bypass for all 4 bands. When off, the binding configures all stages as flat (or routes around them — implementation detail).

### Fader

A normalized 0..1 value that scales the channel's contribution to its sends. The fader value is multiplied into the send levels at the binding step (i.e. `effective_send = fader * pan_factor * send_level`). This is the standard "fader is post-EQ, pre-send-level" topology.

### Mute

`/ch/NN/mix/on` (X32 idiom: 1 = unmuted, 0 = muted). When muted, the channel contributes zero to all sends. Implementation: bypass via gain=0 at the send mixer.

### Pan

`/ch/NN/mix/pan f 0..1` (0.5 = center). For mono channels, pan affects the left/right send factors. For stereo-linked channels, pan affects the L/R balance independently for each side.

### Sends

For each bus, the channel has a send level: `/ch/NN/mix/MM/level f 0..1`. This is multiplied with the fader and pan factors to determine the channel's actual contribution to that bus.

For the small mixer with two buses (main + aux 01), each channel has two send levels. Defaults:

- Send to main: 1.0 (channels are heard in the main bus by default)
- Send to aux 01 (capture): 1.0 (channels are recorded by default)

## Solo behavior (SIP — solo-in-place)

Solo-in-place is the v1 solo mode. It's hardcoded; `/config/solo/mode` is reserved as a future endpoint for AFL/PFL alternatives.

### How SIP works

When any channel has `mix/solo = 1`:
- The main bus mixes only soloed channels (non-soloed channels are muted to main).
- The capture bus is unaffected — it always sums all channels regardless of solo.

When all channels have `mix/solo = 0`:
- The main bus behaves normally (all unmuted channels heard).

This is the simplest solo behavior and matches what most engineers expect from a "solo" button. Implementation: a global `anySolo` flag in the mixer model, recomputed when any channel's solo changes; the binding consults it when applying main bus send levels.

## Master (headphone output)

The main bus has its own fader and mute:

- `/main/st/mix/fader f 0..1` — headphone master volume
- `/main/st/mix/on i 0|1` — headphone mute

The TAC5212's DAC digital volume is **also** controlled via the host's USB audio playback volume slider (the existing `usbIn.volume()` mechanism). The framework should respect both: the OSC master fader is a post-mixer scaling factor, and the host slider sets the codec's hardware DAC volume independently. Both compose multiplicatively.

## Metering

Per-channel input meters and per-bus output meters. Implementation:

- `AudioAnalyzePeak_F32` and `AudioAnalyzeRMS_F32` instances tapped into each channel's post-fader signal.
- `AudioAnalyzePeak_F32` and `AudioAnalyzeRMS_F32` on each bus output.
- `MeterEngine` polls all of them at 30 Hz, packs into blobs, and emits via `OSCSubscribe` to subscribed clients.

Blob format (per blob address):

- `/meters/input` — 5 channels × 2 floats (peak, RMS) = 40 bytes
- `/meters/output` — 2 buses × 2 floats × 2 channels (L, R) = 32 bytes

Both blobs combined per update: ~80 bytes including framing. At 30 Hz: ~2.4 KB/s. Comfortable bandwidth.

Subscription: a client sends `/sub addSub i i s` with interval=33 (ms), lifetime=10000 (ms), address=`/meters/input` (or `/meters/output`). Server streams. Client renews every ~5 seconds. When the client disconnects (or doesn't renew), streaming stops.

## Scenes

The mixer model is plain data (POD-like structs holding channel state, bus state, EQ params). `SceneStore` serializes the model to JSON and writes it to the SD card under `/scenes/NN.json`.

OSC endpoints:
- `/-snap/save i` — store current model state to scene N
- `/-snap/load i` — recall scene N: deserialize, replace model, run binding to apply
- `/-snap/list` — return scene names blob

The implementation works end-to-end in v1, but the scene library is just one or two test scenes. Curated content is out of scope.

## What is explicitly NOT in scope for the small mixer v1

To keep the example legible and the framework focused:

- **Dynamics processing.** The mixer model has slots for `/ch/NN/dyn/...` reserved, but no compressor, gate, or limiter is wired into the signal chain. Future work.
- **Multiple buses beyond main + capture.** The framework supports N buses; the small mixer just declares 2.
- **Per-band EQ visualization (FFT spectrum overlay).** EQ visualization in v1 is curve-only — the client computes the curve from the band parameters. FFT overlay streaming is future work.
- **Solo modes other than SIP.** AFL/PFL is reserved.
- **Talkback, oscillator, signal generator.** Not relevant for a small mixer.
- **Multiple solo, mute, or scene "groups" (DCAs / VCAs).** Future X32-style feature.
- **MIDI control surface integration.** Future epic.
- **Per-channel processing chains beyond HPF + EQ.** No de-esser, no FX sends, no insert points.

The reserved address slots (e.g. `/ch/NN/dyn/...`) are documented in [02-osc-protocol.md](02-osc-protocol.md) so future expansion has a clear place to land without breaking the existing tree.

## Audio object instantiation budget

For the small mixer, the F32 audio graph instantiates roughly:

- 5× `AudioFilterBiquad_F32` for HPFs
- 5× `AudioFilterBiquad_F32` for 4-band EQs
- A few `AudioMixer4_F32` instances for bus summing (probably 2-3 mixers per bus due to the 4-input limit)
- 1× `AudioInputUSB_F32`, 1× `AudioOutputUSB_F32`
- TDM in/out for the TAC5212 (using stock Audio Library `AudioInputTDM` / `AudioOutputTDM` plus F32 conversion blocks, since OpenAudio's TDM support varies)
- ~10× `AudioAnalyzePeak_F32` + `AudioAnalyzeRMS_F32` for meters
- Conversion blocks at the I16/F32 boundaries

Total memory footprint: well within Teensy 4.1's RAM. CPU load: very low, since F32 biquads on a Teensy 4.1 are cheap and there are only ~30 of them.

## How the framework scales to 8×8 and 16×16

The same `MixerModel` declaration with different parameters:

- `Channel channels[NUM_CHANNELS]` — 8 or 16 instead of 5
- `Bus buses[NUM_BUSES]` — more aux sends, subgroups
- `SignalGraphBinding` declares more biquad/mixer instances

The OSC tree shape doesn't change. The client UI presets are different (more strips), but the protocol contract is identical. A client written for the small mixer can talk to the 16×16 — it'll just see more channels. Whether the UI shows them all or only the first 5 is a client concern.

This is the payoff for the architectural discipline. The first example does the work of validating the framework; subsequent examples reuse it.
