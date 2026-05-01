# T-DSP F32 Audio Shield

Fully F32 / 24-bit / 48 kHz port of `t-dsp_tac5212_audio_shield_adaptor`. No
int16 audio anywhere on the host audio path — USB samples land in F32, run
through the TAC5212 over true-32-bit-per-slot TDM, and come back out as F32
to the host.

## Current capabilities (built and validated)

```
USB host playback
       │
       ▼
[USB FU 0x31]  ─poll─▶ pollHostVolume()
       │                     │
       ▼                     ▼
AudioInputUSB_F32 ──▶ mixL/R[0] ──▶ mainAmp ──▶ hostvolAmp ──▶ procShelf ──▶ tdmOut ──▶ TAC5212 DAC ──▶ jack
                          ▲                                                                                │
                          │                                                                                │
              AudioConvert_I16toF32 ◀── AudioSynthDexed                                              loopback cable
                          │  (mono, dual-mono into mixL[1]/mixR[1])                                        │
                          │                                                                                ▼
USB MIDI host             │                                                                       TAC5212 ADC
keyboard ──▶ MidiRouter ──┴──▶ ArpFilter ──▶ DexedSink                                                    │
              ▲                                                                                            ▼
              ▼                                                                                       tdmIn ──▶ AudioOutputUSB_F32
        ClockSink                                                                                                       │
              ▲                                                                                                        ▼
        tdsp::Clock                                                                                            USB host capture
              ▼
        24 PPQN ticks                                                                              [USB FU 0x30] ─poll─▶ pollCaptureHostVolume()
```

| Feature | Status |
|---|---|
| F32 USB↔TAC5212 stereo passthrough at 24-bit / 48 kHz | ✅ -101 dB THD, -144 dB noise floor, 138 dB SNR |
| TAC5212 codec init via typed `lib/TAC5212` driver | ✅ |
| Mixer: per-channel fader + mute (`/ch/01`, `/ch/02`) | ✅ |
| Main bus: stereo fader + mute (`/main/st/mix/...`) | ✅ |
| Main-bus high-shelf EQ (`/proc/shelf/...`, default passthrough) | ✅ |
| Windows playback volume slider tracking (FU 0x31) | ✅ device-side amp with single-block latency |
| Windows recording volume slider tracking (FU 0x30) | ✅ tracked + broadcast (no audio consumer yet) |
| USB MIDI keyboard input via USB host port | ✅ |
| MIDI router with viz sink broadcasting `/midi/note` | ✅ |
| Arpeggiator filter (`/arp/on i`, default off = pass-through) | ✅ |
| Dexed FM synth (8 voices, 10 banks × 32 voices) | ✅ playable from keyboard, dual-mono into mixL[1]/mixR[1] |
| Dev surface OSC + WebSocket via SLIP-OSC on USB CDC | ✅ |
| Tac5212Panel control surface (`/codec/tac5212/...`) | ✅ |

## Not yet built (next session)

- **Solo** per channel — only 2 channels currently (USB L/R), so solo isn't urgent
- **TLV320ADC6140 + 4-channel XLR mic preamp** — adds Line / Mic / XLR 1-4 channels
- **PDM mic** — onboard stereo PDM mic on TAC5212 GPIO1 + GPI1
- **Listenback monitor amps** — connect FU 0x30 (capture hostvol) to actual audio attenuation
- **Additional synth engines** from the production project — MPE, Neuro, Acid, Supersaw, Chip
- **Beat sequencer** (TDspBeats — needs SD card)
- **Looper** (TDspLooper)
- **Channel HPF + per-channel EQ**
- **Limiter** in the F32 graph (TAC5212 chip-side limiter is exposed via the codec panel)
- **Meters + spectrum analyzer**

## Key implementation notes

### Why not `#include <usb_audio.h>` in `main.cpp`

An earlier attempt at host-volume tracking included `<usb_audio.h>` directly
in `main.cpp` to read `AudioInputUSB::features.{volume,mute}`. That
correlated with a USB enumeration failure (Teensy invisible to Windows).
Whether the include was the actual cause or coincidence (port-bound Windows
driver state was a confound), the safer pattern is:

- `lib/USB_Audio_F32_24/src/AudioInputUSB_F32.cpp` includes `<usb_audio.h>`
  and exposes static `volume()` / `mute()` accessors.
- `lib/USB_Audio_F32_24/src/AudioOutputUSB_F32.cpp` does the same for FU 0x30.
- `main.cpp` calls `AudioInputUSB_F32::volume()` and never sees the int16
  AudioStream-related types.

This keeps the int16 USB Audio class definition out of the link unless
explicitly opted into.

### Audio block pool sizing

Phase 1 (USB↔TDM passthrough only) ran on `AudioMemory_F32(40)`. Adding the
mixer, hostvol, biquad, Dexed bridge, and dual-mono fan-out pushed the F32
pool to **128**, and the int16 pool (only Dexed + bridge consume it) to
**256**. Block starvation surfaces as cumulative noise floor + small
frequency drift, not as outright silence — easy to misdiagnose if you've
never seen the symptom.

### TAC5212 lib changes used by Phase 1+

- `setSerialFormat()` was extended to also write `INTF_CFG1` (DOUT routes
  PASI TX) and `INTF_CFG2` (DIN enable). POR leaves both pins inactive — a
  TDM/I2S/LJ format with neither set produces silence regardless of slot
  map.
- `setTxSlotOffset()` was writing to `PASI_TX_CFG2` (0x1D, channel select
  bits) — now correctly writes to `PASI_TX_CFG1` (0x1C, BCLK offset). The
  bug killed slot 0 (silent L channel end-to-end). Caught by the loopback
  test.
- New `setDspAvddSelect(bool)` — page-1 `MISC_CFG0` bit 1 fix that arms the
  DSP-resident limiter / BOP / DRC blocks. Required HIGH before enabling
  any of them; POR default produces a high-pitched squeal otherwise.

## OSC tree (current)

```
/snapshot                      — full state dump (panel + mixer + synth)
/info                          — device identity

/ch/01/mix/fader f             — USB L fader, 0..1 linear
/ch/02/mix/fader f             — USB R fader
/ch/01/mix/on i                — USB L mute
/ch/02/mix/on i                — USB R mute

/main/st/mix/faderL f          — main bus L fader
/main/st/mix/faderR f          — main bus R fader
/main/st/mix/on i              — main mute
/main/st/hostvol/enable i      — engage Windows playback slider
/main/st/hostvol/value f       — read-only, echoed when Windows pushes SET_CUR

/usb/cap/hostvol/value f       — read-only, Windows recording slider tracking
/usb/cap/hostvol/mute i        — read-only

/proc/shelf/enable i           — high-shelf engaged?
/proc/shelf/freq f             — corner Hz
/proc/shelf/gain f             — boost / cut dB

/midi/viz/enable i             — broadcast /midi/note frames?
/midi/note i i i               — emitted on note events (note, velocity, channel)
/midi/note/in i i i             — UI-originated note (note, velocity, channel)

/arp/on i                      — arpeggiator engaged? (default off = pass-through)

/synth/dexed/volume f          — 0..1 linear
/synth/dexed/on i              — engage Dexed
/synth/dexed/voice i i [s]     — bank, voice index; firmware echoes name
/synth/dexed/midi/ch i         — listen channel (0 = omni)

/codec/tac5212/...             — Tac5212Panel subtree (full register access)
```

## Build / upload

```
~/AppData/Roaming/Python/Python313/Scripts/platformio.exe run -d projects/t-dsp_f32_audio_shield -t upload
```

The Teensy auto-reboots via USB CDC on a clean upload. If it reports
"No Teensy boards were found", press the PROGRAM button on the Teensy 4.1
to drop into HalfKay bootloader mode.

## Sanity check

```
python projects/teensy_usb_audio_tac5212/loopback_test.py
```

Plug a 3.5 mm TRS cable from OUT1/2 to INxP. Expected: 1 kHz sine at -6 dBFS
with THD better than -90 dB and SNR > 130 dB on the signal channel.
