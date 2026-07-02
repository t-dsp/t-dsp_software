# t-dsp_esp32_bt_receiver

Bluetooth receiver firmware for the **ESP32-DevKitC** on the
`teensy41_digital_audio_board`. The ESP32 is a Bluetooth Classic **A2DP sink**:
a phone connects over Bluetooth, the ESP32 decodes the audio and streams it to
the Teensy 4.1 over I2S, and the Teensy plays it through the TAC5212 DAC.

This is the only **ESP32** project in the repo — it builds with
`platform = espressif32` instead of `platform = teensy`.

## Audio path

```
phone --A2DP/Bluetooth--> ESP32 (this firmware, I2S MASTER, 44.1k/16-bit)
                                |
                     I2S out (BCK/WS/DOUT)
                                v
             #ESP32_I2S1 5-pin header on the board
                                v
        Teensy 4.1 SAI2 slave input (AudioInputI2S2slave_F32, pin 5)
                                v
        44.1k -> 48k async resampler (lib/Audio, alex6679)
                                v
                        TAC5212 DAC --> OUT jack
```

## ESP32 <-> Teensy I2S wiring (fixed by the board)

The `#ESP32_I2S1` header ties the ESP32's I2S to the Teensy's **SAI2**. The
ESP32 is I2S **master** (generates BCLK/LRCLK); the Teensy is the slave.

| Signal | ESP32 GPIO | Teensy 4.1 pin (SAI2) | Direction |
|--------|-----------|-----------------------|-----------|
| BCK (bit clock)   | GPIO26 | 4  (BCLK2)  | ESP32 → Teensy |
| WS (LR clock)     | GPIO16 | 3  (LRCLK2) | ESP32 → Teensy |
| DOUT (audio data) | GPIO25 | 5  (IN2)    | **ESP32 → Teensy** (the A2DP audio) |
| DIN               | GPIO33 | 2  (OUT2)   | Teensy → ESP32 (unused; future BT transmit) |
| MCLK              | GPIO0  | 33 (MCLK2)  | unused (Teensy slave needs no MCLK) |

> **Note:** ESP32 `GPIO0` doubles as the boot-strapping pin used when the Teensy
> flashes the ESP32 over serial (a later phase). It is left unconfigured here.

## Build & flash (USB, for now)

```bash
cd projects/t-dsp_esp32_bt_receiver
python -m platformio run                    # build
python -m platformio run --target upload    # flash over the DevKitC USB port
python -m platformio device monitor         # 115200 baud
```

Then pair your phone with the Bluetooth device **"T-DSP"** and play audio. The
device name is set by `BT_DEVICE_NAME` in [src/main.cpp](src/main.cpp).

## Status

- [x] A2DP sink → I2S master scaffold (mirrors the proven
      [esp32_T4_bt_music_receiver](https://github.com/JayShoe/esp32_T4_bt_music_receiver))
- [ ] Bench bring-up: pair, confirm BCLK/LRCLK on the scope, verify audio
      reaches the Teensy on pin 5 (IN2)
- [ ] Teensy-side `AudioInputI2S2slave_F32` node + 44.1→48k resampler into the
      TAC5212 graph
- [ ] Phase 2: Teensy programs this firmware over UART2 + EN + IO0
      (esptool protocol; ref [collin80/GEVCU7](https://github.com/collin80/GEVCU7/tree/main/src/devices/esp32))
