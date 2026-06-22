# spike_spdif_alex_dac — optical S/PDIF in → TAC5212 DAC (WORKING)

**Status: WORKING, confirmed by ear (2026-06-19).** 1 kHz tone → S/PDIF OUT
(optical) → loopback cable → S/PDIF IN → async resampler → `AudioOutputI2S` →
SAI1 → TAC5212 DAC (I2S) → headphones/amp. The full optical-in → analog-out
chain runs on a Teensy 4.0.

## The bug (what cost us ~3 sessions)
The S/PDIF **receiver** + the SAI (codec) output hung the firmware at init —
every input build went "dark." We chased: the resampler CPU (red herring — it's
~7-8% per alex's forum numbers), the filter table in OCRAM/cache (moved to DTCM,
didn't help), TDM vs I2S, double-`begin()`, sync vs async input. All dead ends.

**Real cause: stale code.** Our vendored async S/PDIF input + `Resampler` (both
the int16 `lib/Audio` copy and the OpenAudio F32 port) were an OLD snapshot of
alex6679's `teensy-4-spdifIn`. His current code fixes exactly the init/timing
bug that was hanging us (configure() returns `newConfiguration`; Resampler uses
`updateIncrement`/`setPos`; ARM_DWT_CYCCNT-based timing). It was never the board,
the 4.0, or a real "S/PDIF-RX + SAI can't coexist" limit — those earlier
conclusions were wrong.

## The fix
- Replaced `lib/Audio/{async_input_spdif3.{h,cpp}, Resampler.{h,cpp},
  Quantizer.{h,cpp}, biquad.h}` with alex6679's **current** versions
  (https://github.com/alex6679/teensy-4-spdifIn). Backups: git history +
  `/c/tmp/audio_backup`.
- Isolation proof: `projects/spike_spdif_alex` = his code + `AudioOutputI2S`
  (no codec) → LED blinks, coexists with the SAI (the thing the old code hung on).
- This project adds the codec: `AsyncAudioInputSPDIF3` → `AudioOutputI2S` →
  TAC5212 in **I2S 2-ch** (a stereo monitor doesn't need the 8-slot TDM), with
  the DIN/DOUT swap+short handled (disable codec DOUT, `INTF_CFG1=0x00`),
  `HpDriver`, mux auto-select, single SHDNZ pulse.

## ⚠️ Caveat — this breaks the F32/TDM firmware build
Updating `lib/Audio/Resampler` to alex's new API breaks
`lib/OpenAudio_ArduinoLibrary/async_input_spdif3_F32.cpp` (it still calls the OLD
Resampler API: `addToSampleDiff`/`addToPos`/`fixStep`). The spikes `lib_ignore`
OpenAudio so they build. **`t-dsp_f32_audio_shield` and `t-dsp_spdif_monitor`
will NOT compile optical-in until the F32 async wrapper is ported.**

## Next steps (to fold optical-in into the real F32/TDM firmware)
1. Port `async_input_spdif3_F32.{h,cpp}` to alex's current logic — it's his
   int16 `async_input_spdif3.cpp` with float output (audio_block_f32_t) and NO
   int16 quantizer (the resampler is float-native), matching the new Resampler
   API. Then `AsyncAudioInputSPDIF3_F32 → AudioOutputTDM_F32` works in the real
   firmware (32-bit TDM, codec + ADC6140 on 8 slots).
2. OR bridge the int16 async via `AudioConvert_I16toF32 → AudioOutputTDM_F32`.
3. Re-verify with an external optical source (not just the self-loop) at 44.1k
   and 48k.

## Other facts
- See `lib/TAC5212/BOARD_NOTES.md` for the DIN/DOUT swap+short bodge, the
  TCA9544A mux (0x70 ch3), and SHDNZ on pin 35.
- The async input never has update-responsibility; an output (here `spdifOut`)
  drives `update_all`. `spdifOut` also provides the S/PDIF clock.
