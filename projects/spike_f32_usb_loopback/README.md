# Spike: F32 USB Loopback

Validates the new 24-bit / 48 kHz / F32 audio path before porting the
production firmware. Signal chain:

```
USB host  --24-bit / 48 kHz / 2 ch-->  Teensy 4.1
                                       |
                                       v
                          AudioInputUSB_F32  (audio_block_f32_t*)
                                       |
                                       v
                          AudioMixer4_F32   (unity gain, room for DSP)
                                       |
                                       v
                          AudioOutputTDM_F32 (32-bit slots, 24-bit MSB-first)
                                       |
                                       v
                          TAC5212  (TDM slots 0 + 1, DAC out)
```

USB capture (Teensy -> host) follows the same path in reverse via
`AudioOutputUSB_F32`.

## Why this exists

The production project at
`projects/t-dsp_tac5212_audio_shield_adaptor/` is int16 / 44.1 kHz with
~5900 lines of audio graph code. Switching the whole thing to F32 in one
commit is too high-risk — too many things could be wrong (USB descriptor
edits, DMA buffer layout, codec sample-rate clocking, F32 graph wiring,
sample-rate-dependent code like the Dexed engine and looper buffer).

This spike isolates the new plumbing — the F32 USB transport and the F32
TDM driver — into a tiny, instrumentable program. Loopback verified
here, then the production project gets ported with confidence.

## Build flags

| Flag | Purpose |
|---|---|
| `AUDIO_SAMPLE_RATE_EXACT=48000.0f` | Override the 44.1k default in `AudioStream.h` |
| `AUDIO_BLOCK_SAMPLES=128` | Explicit (also the default) |
| `AUDIO_SUBSLOT_SIZE=3` | Activates the 24-bit / 48k USB descriptor + F32 hand-off (added at milestone 3) |
| `USB_SERIAL` then `USB_AUDIO` | Milestones 1-2 use SERIAL; milestone 3+ switches to AUDIO |

## Milestones

1. `lib/Audio_F32_TDM/` compiles standalone *(this build, USB_SERIAL only)*
2. `lib/teensy_cores/teensy4/usb_audio.c` renamed to `.cpp` with
   `extern "C"` shims; existing `t-dsp_tac5212_audio_shield_adaptor`
   project still builds and runs unchanged
3. `AUDIO_SUBSLOT_SIZE=3` USB descriptor changes compile, host enumerates
   the spike as a 24-bit / 48 kHz device (no audio path yet)
4. `AudioInputUSB_F32` / `AudioOutputUSB_F32` loopback verified
5. Full spike runs: USB -> F32 mixer -> TDM -> TAC5212 plays audio

## Next steps

Once this spike validates the plumbing, the production firmware port
will live at:

```
projects/t-dsp_tac5212_audio_shield_adaptor_f32/
```

That folder will reuse `lib/Audio_F32_TDM/` and `lib/USB_Audio_F32_24/`
verbatim — only the audio graph wiring changes, since the engines
(Dexed, MPE, Neuro), looper, and meters all need to be rebuilt against
F32 base classes and the new sample rate. **Do not create that folder
until the spike clears milestone 5.**
