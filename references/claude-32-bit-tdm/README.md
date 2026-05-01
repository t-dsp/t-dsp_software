# AudioOutputTDM_F32

Float32 -> 24-bit-in-32-bit TDM output node for the OpenAudio_ArduinoLibrary
audio graph on Teensy 4.1. Designed against the TAC5212 but works with any
codec configured for 32-bit slot width / 24-bit MSB-first data.

## Files

| File | Role |
|---|---|
| `AudioOutputTDM_F32.h` | Class declaration, 8 F32 input ports |
| `AudioOutputTDM_F32.cpp` | Implementation (DMA setup + ISR + update) |
| `TDM_F32_stereo_sine.ino` | Example: two sine sources on slots 0 and 1 |

Drop the header and `.cpp` into your sketch folder, or alongside the rest of
the OpenAudio_ArduinoLibrary sources in `libraries/OpenAudio_ArduinoLibrary/`.

## What changed from the stock `AudioOutputTDM`

The SAI1 hardware was already running 32-bit slots in the stock driver — the
stock version just put a 16-bit sample in the upper half. So almost all of
`config_tdm()`, the DMA descriptor, and the ping-pong/half-buffer ISR pattern
are unchanged. The five edits are:

1. **Class hierarchy.** `class AudioOutputTDM : public AudioStream`
   becomes `class AudioOutputTDM_F32 : public AudioStream_F32`.

2. **Block type.** Every `audio_block_t *` becomes `audio_block_f32_t *`.
   The `inputQueueArray` is sized 8 either way.

3. **Receive call.** `receiveReadOnly(i)` becomes `receiveReadOnly_f32(i)`,
   `AudioStream::release(...)` becomes `AudioStream_F32::release(...)`,
   `AudioStream::update_all()` becomes `AudioStream_F32::update_all()`.

4. **ISR data path.** The stock interleave loop reads `int16_t *src1..src8`
   and stores `*dest++ = (*src1++) << 16`. The F32 ISR replaces that with:

   ```cpp
   int32_t q31_tmp[AUDIO_BLOCK_SAMPLES];
   for (int ch = 0; ch < 8; ch++) {
       uint32_t *p = dest + ch;
       if (block_input[ch]) {
           arm_float_to_q31(block_input[ch]->data, q31_tmp, AUDIO_BLOCK_SAMPLES);
           for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) p[i*8] = (uint32_t)q31_tmp[i];
       } else {
           for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) p[i*8] = 0;
       }
   }
   ```

   `arm_float_to_q31` saturates inputs outside `[-1.0, +1.0)` and produces a
   value whose **upper 24 bits are signed 24-bit audio**. The codec, in
   24-bit data mode, latches those upper 24 bits and discards the rest.

5. **Sample-rate plumbing.** Constructor accepts `AudioSettings_F32` so
   sample rate is configurable per-instance (not just `AUDIO_SAMPLE_RATE`).
   `audio_block_samples` still has to equal the compile-time
   `AUDIO_BLOCK_SAMPLES` because the DMA buffer is sized at compile time.

`config_tdm()` itself is byte-identical. Don't refactor it — keep it a
verbatim port so future PJRC fixes can be mirrored mechanically.

## Why Q31, not "x * 8388607.0f"

Same upper-24-bit result, free saturation, and CMSIS-DSP runs the loop with
NEON-style scheduling on the Cortex-M7. Manual:

```cpp
if (s > 1.0f) s = 1.0f; else if (s < -1.0f) s = -1.0f;
int32_t v = (int32_t)(s * 8388607.0f);
slot = (uint32_t)(v << 8);
```

The codec ignores anything below bit 8 of the slot, so "scale to 24 then
shift up" and "scale to 31 directly" give the codec the same audio. Q31
is faster and short.

## Slot / channel mapping

```
TDM frame (8 slots x 32 bits, fs * 256 BCLK):

  | slot 0 | slot 1 | slot 2 | slot 3 | slot 4 | slot 5 | slot 6 | slot 7 |
     |        |
   tdm,0    tdm,1                       <- AudioConnection_F32 input ports

Within a slot (32 BCLKs, MSB first):
  bit 31 ............... bit 8 | bit 7 .... bit 0
  [---- 24-bit signed audio ---] [- ignored by codec -]
```

Stereo: connect to ports 0 and 1, leave 2-7 open. Scaling out is purely an
audio-graph change — nothing in the driver changes between 2 and 8 channels.

If you put more than one TAC5212 on the same TDM bus, give each codec a
non-overlapping slot range (codec-side register), then route audio graph
ports to those slots.

## TAC5212 configuration (codec-side)

The driver's contract with the codec:

| Parameter | Value |
|---|---|
| Mode | TDM |
| Slot width | 32 bits |
| Data length | 24 bits |
| Bit order | MSB first |
| Data position in slot | Left-justified (starts at MSB) |
| FSYNC | 1 BCLK pulse, asserted one BCLK *before* slot 0 (`I2S_TCR4_FSE`) |
| BCLK polarity | Data clocked out on falling edge (`I2S_TCR2_BCP`) |
| BCLK rate | fs * 256 (e.g. 12.288 MHz at 48 kHz) |
| MCLK | Provided on pin 23, target 256 * fs (verify TAC5212 PLL setup) |
| Master/slave | Teensy is BCLK + FSYNC master; codec is slave |

What you need to set on the TAC5212 (consult the current datasheet for
register addresses — the part has had revisions):

- Audio Serial Interface: TDM mode, 24-bit word, 32-bit slot.
- ASI offset / first-bit position: 0 BCLKs (data starts at slot boundary).
- BCLK + FSYNC as inputs (slave).
- Slot routing: DAC L from slot 0, DAC R from slot 1 (or wherever you map).
- Clock source: PLL from MCLK at 256 * fs, or BCLK if MCLK isn't wired.

If you hear MSB-aligned but bit-shifted audio (sounds like one channel's bit
is in another's slot), the offending knob is almost always the FSYNC offset
register on the TAC5212 — toggle between "FSYNC and data on same edge" vs.
"data delayed 1 BCLK" until it matches `I2S_TCR4_FSE` on the Teensy side.

## Pin map (Teensy 4.1 SAI1)

| Teensy pin | Signal | Direction |
|---|---|---|
| 7  | TX_DATA0 (SDOUT to codec) | OUT |
| 20 | FSYNC (frame sync)        | OUT |
| 21 | BCLK (bit clock)          | OUT |
| 23 | MCLK (master clock)       | OUT |
| 8  | RX_DATA0 (SDIN from codec) | IN  *if you also build AudioInputTDM_F32* |

Both TX and RX share BCLK + FSYNC (this is what the `tsync = 1, rsync = 0`
in `config_tdm` means: TX drives, RX syncs to TX). So if you later add an
input TDM class, it just attaches to the same SAI1 instance.

## Performance notes

- DMA buffer is `8 * AUDIO_BLOCK_SAMPLES * 2` uint32 in DMAMEM (OCRAM2).
  At `AUDIO_BLOCK_SAMPLES = 128`: 8 KB. Static, no malloc.
- DMAMEM is non-cacheable on Teensy 4.x, so no `arm_dcache_flush` calls
  are needed around the buffer. Don't add them — they slow the ISR.
- ISR worst case at 48 kHz / 128-sample blocks: 8 calls to
  `arm_float_to_q31(.., 128)` plus 1024 strided stores, every 2.67 ms.
  Measured well under 50 us on Teensy 4.1 @ 600 MHz; plenty of headroom.
- `arm_float_to_q31` saturates internally, so float inputs above +1.0
  clip cleanly to full-scale rather than wrapping. Worth knowing if a
  filter in front of this overshoots — you'll get hard digital clip,
  not a sign-flip glitch.

## Things this driver intentionally doesn't do

- **Variable `audio_block_samples` at runtime.** The DMA buffer is sized
  at compile time. If you need 64-sample blocks, recompile with a
  different `AUDIO_BLOCK_SAMPLES`.
- **Cache management.** Not needed for OCRAM2; would be needed if you
  ever moved the buffer to DTCM/ITCM (don't).
- **Codec init.** Configure the TAC5212 over I2C from your own driver —
  this class only owns the TDM master.
