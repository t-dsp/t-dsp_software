# WS2812B Status LED Test

Verifies the onboard status **WS2812B** (SparkFun COM-16347) on the Teensy 4.1
digital audio board.

## Hardware path

From the `teensy41_digital_audio_board` schematic (STATUS LED block):

```
Teensy pin 31 (symbol pin "31_CTX3")
  -> net TEENSY_LED
  -> 74HCT2G17 dual buffer (3.3V -> 5V level shift, non-inverting)
  -> WS2812B DIN
```

The ESP32 can drive the same LED via net `ESP32_LED`. Make sure the ESP32
firmware isn't also toggling the status LED while running this test.

## What it does

1. **Startup channel check** — RED, GREEN, BLUE, WHITE (1s each) to confirm
   every channel of the RGB die.
2. **Rainbow loop** — continuous smooth HSV sweep to confirm timing is stable.

The onboard Teensy LED blinks as a heartbeat, independent of the WS2812 path,
so you can tell "firmware dead" apart from "LED wiring dead".

Serial (115200) prints the current phase.

## Build / upload

```
pio run -t upload
```

> Close any open serial monitor before uploading, or the COM port stays busy —
> if the upload fails on a busy port, press the **PROGRAM** button on the Teensy.
