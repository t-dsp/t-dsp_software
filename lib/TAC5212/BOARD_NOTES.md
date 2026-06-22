# TAC5212 — board-specific notes (teensy41_digital_audio_board)

These are *board wiring* facts, not driver bugs. They bit us hard during
optical-monitor bring-up (2026-06-18); document here so the next person using
this driver on this board doesn't lose an afternoon.

## SD_IN / SD_OUT (DIN/DOUT) are swapped — pins shorted as a bodge → disable DOUT
On this board the codec's serial-data DIN and DOUT lines are **swapped**, and the
two pins are **shorted together** as the hardware workaround. With the codec's
DOUT driving, it fights the Teensy SAI TX (the DAC playback data) on that shared
node → **DAC is silent**.

**Required workaround in firmware:** after `setSerialFormat()` (which enables
DOUT, writing `INTF_CFG1 = 0x52` "PASI TX, active-low/weak-high"), force DOUT
off:

```cpp
codec.setSerialFormat(sf);                 // enables DOUT (0x52) + DIN
codec.writeRegister(0, /*INTF_CFG1*/0x10, 0x00);   // DOUT func disabled / Hi-Z
```

`INTF_CFG1` nibble convention: high nibble = DOUT function (0 = disabled), low
nibble = drive (0 = Hi-Z). So `0x00` releases the DOUT pin entirely while DIN
(`INTF_CFG2`, set by `setSerialFormat`) stays enabled so the DAC still receives.

**Consequence:** the codec **ADC / capture path is unusable** on this board — its
data can't leave the chip. Treat the TAC5212 here as **DAC / output-only**. (This
is the original reason optical OUT became the project's known-good output path.)

The driver intentionally does not expose a DOUT-disable setter (`setSerialFormat`
folds DOUT-enable in); use `writeRegister()` as above. If a board ever needs this
routinely, consider adding a `SerialFormat.doutEnable` flag to the driver.

## Same swap on the TAC5212 module + t-dsp adaptor (pro_audio_module)
The I2S/TDM serial-data lines are **also swapped** on the **TAC5212 module mated
to the t-dsp TAC5212 adaptor** — `SD_IN` and `SD_OUT` are **crossed on the
pro_audio_module TDM header**. Same root cause as the digital-audio-board bodge
above, but here the pins are *not* shorted: the cross is in the header wiring.

**Symptom:** the PLL **locks** (so `dumpStatus()` looks healthy — clock is fine),
but the **DAC stays silent** because playback data lands on the codec's DOUT pin
instead of DIN.

**Fix options:**
- **HW rework** the adaptor / module TDM header to un-cross `SD_IN`/`SD_OUT`, or
- **codec-side reroute** the data pins via register config so DIN/DOUT map to the
  physically-wired traces.

Until either is done, treat this board combo as **not passing audio** even though
PLL lock and I2C all report OK. See memory `project_tac5212_module_data_swap`.

## Other board facts
- TAC5212 is at **I2C 0x51 behind a TCA9544A mux (0x70), channel 3.** Select the
  mux channel before any codec transaction (`tdspMuxAutoSelectCodec(0x51)`).
- **SHDNZ is pin 35, shared with the on-board TLV320ADC6140.** Pulse low→high
  once at boot; after that drive the codec only via I2C SW_RESET — never
  re-toggle pin 35 (it also resets the 6140).
- DAC output mode `HpDriver` is the verified-working mode (headphone load on
  OUT1/OUT2). `SeLine` (true line driver) is not yet confirmed on this board.
- Codec wants **32-bit TDM slots** → drive it from the OpenAudio **F32** TDM
  output (`AudioOutputTDM_F32`), not the stock int16 `AudioOutputTDM`.
- `dumpStatus()` reports PLL lock / faults / OUT mode — use it first when audio
  is silent (PLL locking rules out a clock problem and points at the data path).

See `projects/t-dsp_spdif_monitor/NOTES.md` for the full optical-monitor bring-up
log and the F32 `update_responsibility` declaration-order gotcha.
