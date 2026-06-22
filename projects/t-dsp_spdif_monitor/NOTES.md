# t-dsp_spdif_monitor — bring-up notes & "operational sound" checklist

Goal: optical S/PDIF **in** → play it out **both** the optical S/PDIF **out** and the
**TAC5212 DAC line/headphone out** (to an amp). Built as a stripped
`t-dsp_f32_audio_shield` (reuses the TAC5212 typed-driver bring-up + OpenAudio
F32 TDM/SPDIF path; drops synths/sampler/arp/USB-host/mixer).

## ✅ CONFIRMED OPERATIONAL-SOUND RECIPE (DAC tone audible 2026-06-18)
All FOUR must be true together or the DAC is silent:
1. **`AudioOutputTDM_F32 tdmOut` constructed FIRST** → it owns update_responsibility →
   `AudioStream_F32::update_all()` actually runs (graph live; `TXtone≈0.5`).
2. **SPDIF output initialized** (`AudioOutputSPDIF3_F32` + `begin()`) → sets up the
   audio clock (PLL4) → **codec PLL locks** (`dumpStatus` PLL locked: yes). Strip
   all SPDIF and the codec PLL won't lock → silent.
3. **DOUT disabled** after `setSerialFormat()`: `writeRegister(0,0x10,0x00)` → no
   contention with the Teensy TX on the swapped/shorted DIN/DOUT node.
4. **`OutMode::HpDriver` + `powerDac(true)` + unmute (`setDvol(0.0f)`)**.
DIRECT_TONE meets all four → 1 kHz audible on headphones, CPU ~1%.

Date: 2026-06-18. Board: `teensy41_digital_audio_board` with a **Teensy 4.0**
fitted, TAC5212 behind a TCA9544A I2C mux. Default env `teensy40_mux`.

---

## Things required to get operational sound (hard-won this session)

### 1. F32 update_responsibility — DECLARATION ORDER MATTERS  ⚠️ biggest gotcha
The first **hardware output** object constructed wins `update_setup()` and owns
`update_responsibility`; its DMA ISR is what calls `AudioStream_F32::update_all()`
for the **entire** F32 graph. `AudioOutputTDM_F32`'s constructor auto-calls
`begin()` and its SAI-TX ISR reliably drives the update.

- **`tdmOut` (AudioOutputTDM_F32) MUST be declared before the SPDIF objects.**
- If a SPDIF object (`AsyncAudioInputSPDIF3_F32` / `AudioOutputSPDIF3_F32`) is
  constructed first, it grabs responsibility but does **not** reliably fire
  `update_all()` → the whole F32 graph freezes: every node produces zero blocks
  (tone generator, spdifIn, peak meters all read "no data" / `-1`), DAC silent,
  even though the codec PLL is locked and optical OUT is "lit".
- Symptom that nails it: a peak meter on the *tone source itself* reads `-1`
  (no blocks) → the graph isn't updating, it's not a codec problem.
- The full `t-dsp_f32_audio_shield` declares `tdmOut` first (line ~140); copy
  that order.

### 2. Board hardware bodge — SD_IN/SD_OUT swapped + shorted → disable codec DOUT
On this board the codec data lines **DIN/DOUT (SD_IN/SD_OUT) are swapped**, and
the two pins have been **shorted together** as the workaround. With the codec's
DOUT active it drives that shared node and **fights the Teensy's SAI TX (the DAC
data)** → DAC silent.

- Fix: after `setSerialFormat()` (which enables DOUT = `INTF_CFG1` `0x52`),
  write **`INTF_CFG1 = 0x00`** (DOUT func disabled / Hi-Z). DIN stays enabled
  (`INTF_CFG2`) so the DAC still *receives*.
- Consequence: the **codec ADC is unusable** on this board (its data can't get
  out past the shorted/disabled DOUT). This firmware is **DAC/output-only**.
- This is why optical OUT became the historical "known-good" path.

### 3. Reboot loop in the full firmware = RAM/CPU pressure, not hardware
The full `t-dsp_f32_audio_shield` re-enumerates ~1×/sec (USB audio cuts in/out)
and dies before printing — a crash loop from the synth fleet's RAM + the heavy
S/PDIF-in resampler. The lean build here is rock-stable (`up=N` counts up).
Optical hardware is 100% good (proven via `spike_spdif_min` loopback, RX peak
0.5; both Cliff FCR6842032T/R parts + pin maps verified vs datasheet).

### 4. AsyncAudioInputSPDIF3_F32 is NOT realtime-viable here  ⛔ the blocker
The OpenAudio async F32 S/PDIF input overruns realtime on the Teensy 4.0 when it
actually runs, **starving `loop()` (no LED heartbeat) — the firmware hangs.**
- CPU readings while the graph was *frozen* (pre update-order fix): 173% at
  `attenuation=100,half=20..80`; 92% at `60,16..32` (those were the resampler
  ISR, not the real per-block work — the graph wasn't updating).
- After the update-order fix the graph runs, the resampler processes every
  block, and the board **hangs regardless of filter size** — even a 17-tap
  filter (`40,4..8`) hangs. So the cost is a **fixed per-block component**
  (constant filter *re-design* — double-precision Kaiser/Bessel — chasing the
  jittery recovered clock), not the convolution. Can't tune it away from the
  sketch.
- Fits the project history: optical-in was shipped **gated OFF**
  (`TDSP_HAS_SPDIF_IN=0`) — it likely never ran in realtime.
- **Important distinction for the "SPDIF + I2S at once?" question:** SPDIF
  *output* + TDM/I2S run together fine (DIRECT_TONE proves it). It's the async
  *input*'s resampler CPU that kills it, NOT a hardware peripheral conflict.

**UPDATE 2026-06-19 — deeper investigation (overnight + morning):**
- Forum (resampler author, Alexander Walch): the resampler is only **~7-8% CPU**
  at 48 kHz, max ~15% — so it is NOT inherently heavy. Our 92-173% was a wrong
  reading (graph state) / OCRAM cache thrash.
- **Filter table cache fix DID NOT fix the hang.** Moved `Resampler::filter`
  (163 KB) from OCRAM/DMAMEM to DTCM via `TDSP_RESAMPLER_FILTER_FAST_RAM`
  (verified at 0x20005df4 = DTCM in the ELF). The async input **still hangs**.
  So the hang is NOT the cache. Kept the flag anyway (correct for perf).
- **Synchronous input (`AudioInputSPDIF3_F32`) ALSO hangs** with TDM. (Had to
  fix a lib bug first: `AudioInputSPDIF3_F32::sample_rate_Hz` was declared but
  never defined → linker error; the sync F32 input had never been buildable.)
- **`SPDIF_IN_ONLY` (SPDIF in + TDM, NO SPDIF out) ALSO hangs.** So it is not the
  in+out combo — it is the **SPDIF receiver coexisting with the SAI/TDM**.

**DEFINITIVE FINDING:** On this Teensy 4 + Audio-library stack, the **S/PDIF
receiver and the SAI/TDM (codec) clock-conflict.** Evidence:
- SPDIF out + TDM (DIRECT_TONE)  → WORKS (DAC audio confirmed).
- SPDIF in  + SPDIF out, NO TDM  → WORKS (spike_spdif_min loopback).
- SPDIF in  + TDM (any input kind, ± SPDIF out) → HANGS (LED dark, USB wedged).
The SAI is hard-wired as clock master (PLL4); the **synchronous** SPDIF input
needs to slave the system clock to the incoming stream → direct conflict, hang.

**The architecturally-correct fix is the ASYNC (resampling) input** — it is
DESIGNED to decouple the SPDIF-in clock from the SAI clock (resample between
them), which is exactly this scenario. It SHOULD coexist with TDM. It currently
hangs, but that is a software/integration bug to debug (where: begin-time clock
config vs the first update()), NOT a fundamental limit. The sync input is a dead
end here (can't decouple clocks). Next steps for the async path:
1. Localize the async hang: instrument before/after spdifIn.begin() and the
   first update() (LED-state breadcrumbs survive a USB wedge; serial may not).
2. Check whether config_spdif3()'s `while(SPDIF_SCR & SOFT_RESET)` or
   set_audioClock() conflicts with the already-running SAI1/PLL4 (init order).
3. Likely need PJRC-forum input — confirm whether anyone runs the async S/PDIF
   input + an I2S/TDM codec output simultaneously on a T4, and how they clock it.
Alternative if the async path stays blocked: a **hardware S/PDIF→I2S decoder**
chip feeding the Teensy SAI as a normal synced I2S input (bypasses the Teensy
SPDIF RX entirely), or take the input over **USB audio** instead of optical.

### 5. Codec bring-up essentials (reused from full firmware)
- `hardResetCodecPower()` — pulse SHDNZ (pin 35) low→high **once**, before
  `Wire.begin()`. Shared with the on-board 6140; never re-toggle.
- `tdspMuxAutoSelectCodec(0x51)` — TCA9544A at 0x70; codec found on **channel 3**.
- `setSerialFormat()` defaults (TDM/32/FSYNC normal/BCLK inverted) match the
  OpenAudio TDM master; then RX slot routing CH1←slot0, CH2←slot1, offset 1.
- DAC OutMode: `HpDriver` is the known-good mode (drives headphones on OUT1/2).
  `SeLine` (true line level) is **untested** — earlier "silence" with SeLine was
  actually the update-freeze (#1), so SeLine needs a clean retest now.
- `setDspAvddSelect(true)`, `powerDac(true)`, unmute via `out(n).setDvol(0.0f)`.
- `g_codec.dumpStatus(Serial)` confirms PLL lock / faults / OUT mode.

### 6. Misc
- `AudioOutputTDM_F32` ctor auto-calls `begin()`; `AudioOutputSPDIF3_F32` and
  `AsyncAudioInputSPDIF3_F32` need explicit `begin()`.
- Codec is 32-bit TDM → use the **F32** TDM output (32-bit), not stock int16 TDM.
- Close any serial monitor before upload; reboot-looping boards need the PROGRAM
  button. Repeated reflashes can briefly wedge the USB serial enumeration.

---

## Status checklist (optical in → optical out + DAC line out)
- [x] Lean firmware builds + runs stable on teensy40_mux (no reboot loop)
- [x] Codec PLL locks, mux auto-select, no faults
- [x] Optical OUT transmits / SPDIF RX locks (hardware proven)
- [x] Root-caused DAC silence: (a) DOUT bodge for shorted board, (b) F32
      update_responsibility ordering
- [x] Applied fix: `tdmOut` declared first + `INTF_CFG1=0x00`
- [ ] **VERIFY**: tone source meter `TXtone≈0.5` (F32 update running) — pending
      a clean serial read after the order fix
- [ ] **VERIFY**: DAC actually plays the tone (headphones, HpDriver)
- [ ] Async S/PDIF input delivers audio blocks (RXpeak > 0) so optical-in feeds
      the DAC — currently the open problem (#4)
- [ ] Retest `SeLine` for true line level into the amp
- [ ] Full path end-to-end: external optical source → DAC line out + optical out

## Build envs
- `teensy40_mux`        — monitor: optical in → optical out + DAC line out
- `teensy40_selftest`   — tone → optical out, DAC plays the looped-back input
- `teensy40_directtone` — tone straight to DAC + optical out (input bypassed; isolation)
- `teensy40_loopback`   — (in spike_spdif_min) stock-Audio SPDIF loopback test
