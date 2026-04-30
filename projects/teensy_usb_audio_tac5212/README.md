# teensy_usb_audio_tac5212

The most basic possible USB-host -> TAC5212 sketch. Plug the Teensy 4.1 into a
USB host, set the host's audio output to "Teensy Audio" (16-bit/44.1k stereo),
and the audio comes out OUT1/OUT2 on the TAC5212 board.

The build is layered on top of the **teensy-4-usbAudio** reference under
`references/teensy-4-usbAudio/`. The project's `overlay_teensy_usb_audio.py`
extra_script copies that reference's `changedCorefiles/` over PlatformIO's
framework cache before compile -- the same step the reference's README tells
you to do manually for the Arduino IDE.

## Audio graph

```
USB host -- 16-bit/44.1k --> AudioInputUSB --> AudioOutputTDM --> SAI1_TX
                                                                     |
                                                                     v
                                                       TAC5212 RX_CH1/CH2
                                                              |
                                                              v
                                                          DAC -> OUT1/OUT2
```

`AudioOutputTDM` packs two int16 channels into each 32-bit slot. The TAC5212
in 32-bit TDM mode reads each slot as one sample, so we feed `tdmOut`
channels **0 and 2** -- those land in the upper 16 bits of slot 0 and slot 1
respectively. (Same trick the production project uses --
`projects/t-dsp_tac5212_audio_shield_adaptor/src/main.cpp:914-915`.)

## Build / upload

```
pio run -e teensy41 -d projects/teensy_usb_audio_tac5212
pio run -e teensy41 -d projects/teensy_usb_audio_tac5212 -t upload
pio device monitor -e teensy41 -d projects/teensy_usb_audio_tac5212
```

LED blinks at 1 Hz when the firmware is alive.

> **First build after switching from another project in this repo:** the
> overlay adds `usb_audio_interface.cpp` to the framework cores, but PIO
> caches the framework's source-file list. If you see linker errors about
> `USBAudioInInterface::*` undefined, run `pio run -t clean` once and
> rebuild -- PIO rescans the cores directory and picks up the new file.

## What's intentionally NOT here

- No F32. Stock 16-bit `AudioInputUSB` and `AudioOutputTDM`.
- No DSP, mixers, EQ, limiter -- straight passthrough.
- No ADC capture. The 6140 ADC on this board shares SHDNZ with the TAC5212 so
  it powers up alongside; this sketch SW-resets it back to sleep so it stops
  fighting on the TDM bus (per `project_6140_buffer_contention`).
- No Serial UI or web surface. SEREMU is available for `Serial.println` but
  this sketch doesn't print anything.
