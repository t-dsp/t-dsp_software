# T-DSP TAC5212 Audio Shield Adaptor

Firmware for the T-DSP TAC5212 Audio Shield Adaptor — a Teensy 4.1 paired with the TAC5212 Pro Audio Module via the audio shield adaptor PCB.

## What it does

- **USB Audio Class device** — Teensy appears as a stereo USB soundcard ("Teensy MIDI/Audio") on the host computer
- **USB playback → DAC** — audio from the computer streams through I2S/TDM to the TAC5212 DAC
- **Host volume tracking** — the Windows/macOS playback volume slider directly controls the TAC5212 DAC digital volume
- **PDM microphone** — onboard PDM mic captured via the codec's PDM channels, mixable into the DAC output for monitoring
- **Line input** — analog line input through the TAC5212 ADC, sent to USB so the host can record it
- **Local monitoring** — line input and PDM mic can be routed to the headphone output for direct monitoring

## Hardware

- Teensy 4.1
- TAC5212 Pro Audio Module on the audio shield adaptor PCB
- I2C bus 0 (`Wire`, pins 18/19) at address `0x51`
- Pin 35 (TX8) drives `EN_HELD_HIGH` to power the TAC5212 LDO

## Build & Upload

From the project directory:

```bash
cd projects/t-dsp_tac5212_audio_shield_adaptor
python -m platformio run                    # build
python -m platformio run --target upload    # upload to Teensy
```

> **Note:** If you have the serial monitor open, you must close it before uploading. The Teensy Loader cannot reboot the board into program mode if the COM port is held by another process. Alternatively, press the physical program button on the Teensy.

## Serial Monitor

```bash
python -m platformio device monitor
```

Or in VS Code: `Ctrl+Shift+P` → **PlatformIO: Serial Monitor**

The monitor runs at **115200 baud**.

## Keyboard Commands

Once the serial monitor is open, send these single-character commands.

### Toggles & status

| Command | Description |
|---------|-------------|
| `m` | Toggle PDM mic monitoring on/off |
| `l` | Toggle line input monitoring on/off |
| `s` | Print status: buffer usage, CPU, current volumes, codec status |

### Volume control

The volume system has three independent channels: **USB** (host playback), **PDM mic**, and **LINE** input. You select an "active channel" and then adjust it with `+`/`-` or arrow keys.

| Command | Description |
|---------|-------------|
| `u` / `p` / `i` | Select **USB** / **PDM mic** / **LINE** as the active channel |
| `u50`, `p75`, `i80` | Set the channel directly to a value (0–100) |
| `+` or `=` | Increase active channel by 5% |
| `-` or `_` | Decrease active channel by 5% |
| Up arrow ↑ | Same as `+` |
| Down arrow ↓ | Same as `-` |

You can hold `+` or `-` for terminal auto-repeat, mash them, or use the arrow keys for the same effect.

**Examples:**

```
p           → mic is now the active channel
++++++      → ramp mic up 30%
↑↑↑↑        → another 20%
i75         → set line input directly to 75% (and switch active channel to LINE)
↓↓          → drop line input by 10%
m           → toggle mic monitor off
s           → show status
```

> The host's USB audio volume slider also controls the TAC5212 DAC digital volume directly — you don't need `u` for normal playback. The `u` channel here is a Teensy-side mixer gain *on top of* the host volume.

## Audio Routing

```
                       ┌─────────────────────────────────┐
                       │            Mixers                │
USB In  (host)   ──L──▶│ mixL[0]                          │
                  ─R──▶│ mixR[0]                          │
                       │                                  │
PDM Mic (codec)  ────▶ │ mixL[1] / mixR[1]    ──▶ TDM Out │ ──▶ DAC ──▶ Headphones
                       │                                  │
Line In (codec)  ────▶ │ mixL[2] / mixR[2]                │
                       └─────────────────────────────────┘

Line In (codec) ─────▶ USB Out (host capture / recording)
```

- **USB In** is at gain `1.0` by default (host volume controls DAC level)
- **PDM mic** and **line in** start at gain `0.0` — toggle/set them with `m`, `l`, `p`, `i`
- **Line In** also flows to **USB Out** unconditionally so the host can record it

## Codec Configuration

- **Format:** TDM, 32-bit word length
- **BCLK polarity:** inverted
- **DAC slot map:** RX CH1 → slot 0 (left), RX CH2 → slot 1 (right)
- **ADC slot map:** TX CH1 → slot 0, TX CH2 → slot 1
- **PDM slot map:** TX CH3 → slot 2 (PDM ch3), TX CH4 → slot 3 (PDM ch4)
- **Output mode:** differential, DAC source
- **ADC input:** single-ended on `INxP` only (`IN1-` is tied to `IN2+` for balanced mic mode — must be ignored for line input)

## Files

- [src/main.cpp](src/main.cpp) — main firmware
- [lib/TAC5112/](lib/TAC5112/) — Houston's TAC5112 control library (register-compatible with TAC5212)
- [platformio.ini](platformio.ini) — build config (uses `USB_MIDI_AUDIO_SERIAL` USB type)
