# Plan: upstream the async S/PDIF F32 input fixes to OpenAudio_ArduinoLibrary

**Audience:** an engineer/agent who will prepare clean pull requests to
`chipaudette/OpenAudio_ArduinoLibrary` (and possibly coordinate with
`alex6679` and `PaulStoffregen/Audio`). Self-contained — no prior context
needed beyond this doc and the linked repos.

**Do NOT** modify the `t-dsp_software` repo for this work (its `lib/Audio` was
already updated to alex6679's current code locally — that's our reference for
"what works"). The upstream work happens in FORKS of the repos below.

---

## 1. Background — what we discovered

While bringing up optical S/PDIF input → a TAC5212 codec DAC on a Teensy 4.0,
the firmware **hung at init** whenever the S/PDIF *receiver* was brought up
alongside the SAI (I2S/TDM) output. After a long investigation we proved:

- It is **not** a hardware/silicon limit. S/PDIF-RX + SAI coexist fine.
- The cause was **stale library code**: the async S/PDIF input + `Resampler`
  we were using was an OLD snapshot of `alex6679`'s work. His **current** code
  fixes the exact init/timing bug. Dropping his current files in made it work
  (verified by ear: tone → S/PDIF OUT → optical loopback → S/PDIF IN → I2S →
  TAC5212 DAC → amp).

The relevant improvement in alex's current code: `configure()` now returns a
`newConfiguration` flag that `monitorResampleBuffer()` uses to avoid trusting
the buffer-latency math immediately after a resampler (re)configuration, plus a
switch from `micros()` timing to the `ARM_DWT_CYCCNT` cycle counter, and a new
`Resampler` step-control API (`updateIncrement`/`setPos`). Without these, the
resampler computes a garbage step right after configuring and the pipeline
stalls/hangs.

## 2. The dependency chain (this is the crux)

```
alex6679/teensy-4-spdifIn        <-- CURRENT (has the fix)
        |  (never upstreamed)
        v
PaulStoffregen/Audio  (mainline) <-- OLD  (async_input_spdif3 + Resampler +
        |                                   Quantizer + biquad, micros() timing,
        |                                   Resampler::addToSampleDiff)
        v
chipaudette/OpenAudio_ArduinoLibrary
   async_input_spdif3_F32.{h,cpp} <-- OLD F32 port (by Alexander Walch),
   input_spdif3_f32.{h,cpp}           depends on PJRC/Audio's Resampler/Quantizer/
   output_spdif3_f32.{h,cpp}          biquad (it has NO resampler of its own).
```

Verified 2026-06-19:
- `PaulStoffregen/Audio` `Resampler.h` has `bool addToSampleDiff(double)` and
  `void configure(float,float)` — the OLD API. `async_input_spdif3.cpp` uses
  `microsLast=micros()`. So **PJRC mainline is old too.**
- `chipaudette/OpenAudio_ArduinoLibrary` `async_input_spdif3_F32.cpp` has
  `microsLast`, `configure()` (void), `_resampler.addToSampleDiff(diff)` — the
  same OLD logic, F32-output flavored.
- OpenAudio has **no** `Resampler.*`/`Quantizer.*` of its own; it `#include`s
  `Resampler.h`/`Quantizer.h`/`biquad.h` from the Teensy Audio library.

**Implication:** you cannot update OpenAudio's F32 async to the new `Resampler`
API unless the Teensy Audio library it compiles against ALSO has alex's current
`Resampler`. So there are really two layers to fix.

## 3. The two contributions (split them)

### Contribution A — `sample_rate_Hz` linker bug (small, clean, do first)

`OpenAudio_ArduinoLibrary/input_spdif3_f32.{h,cpp}` is the **synchronous** F32
S/PDIF input (`AudioInputSPDIF3_F32`, by Frank Bösing). The header declares
`static float sample_rate_Hz;` and `begin()` uses it
(`AudioOutputSPDIF3_F32::config_spdif3(sample_rate_Hz)`), but **the `.cpp`
never defines it** → undefined-reference linker error. This class has never
been buildable. Confirmed present in upstream `chipaudette` master.

**Fix** (add next to the other static member definitions in
`input_spdif3_f32.cpp`, ~line 50):
```cpp
float AudioInputSPDIF3_F32::sample_rate_Hz = AUDIO_SAMPLE_RATE_EXACT;
```
- No dependency on anything else. Compiles + links immediately.
- Standalone PR to `chipaudette/OpenAudio_ArduinoLibrary`.
- (Optional, same PR: the header has a typo `getTargetLantency()` →
  `getTargetLatency()` in `async_input_spdif3_F32.h`; leave it unless you also
  do Contribution B, to avoid an API break.)

### Contribution B — update async F32 input to alex6679's current logic (bigger)

This is the real fix for the hang, but it has the Resampler dependency above.

**What changed (old OpenAudio F32 async → alex's current int16 async).** Diff
`chipaudette/.../async_input_spdif3_F32.cpp` against
`alex6679/teensy-4-spdifIn/async_input_spdif3.cpp`:
1. **Timing:** `micros()`/`microsLast` → `ARM_DWT_CYCCNT` cycle counter with
   `cyclesLast`, `isrDiffCycles`, `updateLast`. Higher resolution; fixes the
   latency-tracking drift.
2. **`configure()` returns `bool newConfiguration`** (was `void`). Also computes
   `_targetLatencyS`/`_maxLatency` from `noSamplerPerIsr` and the input freq.
3. **`monitorResampleBuffer(bool newConfiguration)`** (was no-arg). Rewritten
   around the cycle timing; uses `_resampler.updateIncrement(diff)` (new) and
   `_resampler.setPos(...)` (new); when `newConfiguration` is true it does NOT
   trust the computed `diff` (the bug that caused the stall/hang).
4. **`update()`**: `bool nc = configure(); monitorResampleBuffer(nc);`.
5. **Members:** add `cyclesLast`/`isrDiffCycles`/`updateLast`/`noSamplerPerIsr`/
   `_targetLatencyS`; remove `microsLast`.
6. **Resampler API** (in the Teensy Audio lib): `updateIncrement`, `setPos`,
   `configure(double,double)`, `periodeLength`, float-based Kaiser, dynamically
   allocated `kaiserWindowXsq`. This is the dependency.

**F32-specific parts to PRESERVE** (do not copy alex's int16 output verbatim):
- Output blocks are `audio_block_f32_t`, not `audio_block_t`.
- The int16 `Quantizer` (dither/noise-shaping to 16-bit) is replaced by the
  internal `Scaler_F32` in `async_input_spdif3_F32.cpp` — currently a near
  float→float passthrough (`// TODO: degenerated to a copy`). Keep F32 output;
  no 16-bit quantization. (Opportunity: resample directly into the F32 output
  block and drop the copy.)
- Uses `AudioSettings_F32` / `AUDIO_SAMPLE_RATE_EXACT` and `AudioStream_F32`.

**The Resampler dependency — three ways to resolve, in order of preference:**

- **B1 (correct, slower): upstream alex's Resampler to PJRC/Audio first.**
  Alex's current `Resampler.{h,cpp}`, `Quantizer.{h,cpp}`, `biquad.h`,
  `async_input_spdif3.{h,cpp}` → PR to `PaulStoffregen/Audio` (he originally
  contributed this code there, so it's the natural home). Then OpenAudio's F32
  async re-port "just works" against updated PJRC/Audio. Best for the whole
  ecosystem; depends on PJRC's review timeline. Ideally coordinate with
  `alex6679` — it's his code and he may prefer to submit it himself.
- **B2 (self-contained): OpenAudio bundles its own current Resampler.** Copy
  alex's current `Resampler`/`Quantizer`/`biquad` INTO OpenAudio under distinct
  names (e.g. `Resampler_F32`, or an `oa_spdif::` namespace) to avoid a linker
  collision with the Teensy Audio library's same-named classes. Then the F32
  async uses OpenAudio's own copy. Decouples OpenAudio from PJRC's version but
  duplicates ~1200 lines and diverges over time. Only if B1 is blocked.
- **B3 (interim): file an issue, don't PR the async yet.** Open a detailed
  issue on `chipaudette/OpenAudio_ArduinoLibrary` (link this doc + the diffs),
  and cc `alex6679`, so the maintainer chooses B1 vs B2. Pair it with
  Contribution A (which is unblocked).

## 4. Recommended sequence

1. **Now:** PR Contribution A (`sample_rate_Hz`) to OpenAudio. Trivial, correct,
   unblocked, immediately useful.
2. **Coordinate:** open an OpenAudio issue for Contribution B referencing this
   analysis, and reach out to `alex6679` about upstreaming his current
   `teensy-4-spdifIn` resampler to `PaulStoffregen/Audio` (path B1). His code is
   the source of truth; getting it into PJRC/Audio fixes the root for everyone.
3. **When the Resampler is available upstream (B1) or bundled (B2):** re-port
   `async_input_spdif3_F32.{h,cpp}` per §3. Keep authorship/credit
   ("by Alexander Walch" / Frank Bösing) and the MIT headers intact.

## 5. Testing (no special hardware needed)

A Teensy 4.0/4.1 with an optical TOSLINK OUT→IN loopback cable (or any external
S/PDIF source) is enough. Minimal repro that exercises S/PDIF-RX + SAI together:
```cpp
#include <Audio.h>
AsyncAudioInputSPDIF3 spdifIn(false,false,100,20,80); // or the _F32 variant
AudioOutputSPDIF3     spdifOut;   // update driver + S/PDIF clock
AudioOutputI2S        i2sOut;     // brings up the SAI — the coexistence test
AudioConnection a(spdifIn,0,spdifOut,0), b(spdifIn,1,spdifOut,1);
AudioConnection c(spdifIn,0,i2sOut,0),   d(spdifIn,1,i2sOut,1);
void setup(){ pinMode(LED_BUILTIN,OUTPUT); AudioMemory(40); }
void loop(){ digitalToggle(LED_BUILTIN); delay(500); } // blinks = alive; dark = hung
```
- OLD code → LED dark (hang). alex's CURRENT code → LED blinks. That's the pass/fail.
- Then feed a real 44.1k and 48k source and confirm `isLocked()` and clean audio
  (no clicks/drift over minutes — the timing rewrite is about long-term stability).
- Reference working sketches in the t-dsp repo: `projects/spike_spdif_alex/`
  (coexistence) and `projects/spike_spdif_alex_dac/` (full DAC loop). The DAC
  bodges there (DIN/DOUT disable, TAC5212 mux/SHDNZ) are **board-specific and
  must NOT go upstream.**

## 6. Do-not-include upstream (t-dsp-local only)
- TAC5212 codec init, the DIN/DOUT-swap bodge (`INTF_CFG1=0x00`), TCA9544A mux,
  SHDNZ handling — all board-specific.
- Any `TDSP_*` build flags, DMAMEM/DTCM filter-placement tweaks
  (`TDSP_RESAMPLER_FILTER_FAST_RAM`). Note: alex's current Resampler keeps the
  163 KB `filter` as a per-instance member (DTCM) — fast and cache-friendly;
  the DMAMEM tweak was only to reclaim RAM1 for our big synth firmware and is
  not needed upstream.

## 7. Key files & links
- OpenAudio (target): https://github.com/chipaudette/OpenAudio_ArduinoLibrary
  - `input_spdif3_f32.{h,cpp}`  (Contribution A)
  - `async_input_spdif3_F32.{h,cpp}`, `output_spdif3_f32.{h,cpp}` (Contribution B)
- alex6679 (source of truth): https://github.com/alex6679/teensy-4-spdifIn
  - `async_input_spdif3.{h,cpp}`, `Resampler.{h,cpp}`, `Quantizer.{h,cpp}`, `biquad.h`
- PJRC Audio (Resampler home): https://github.com/PaulStoffregen/Audio
- Our working reference (this repo): `lib/Audio/*` (already = alex's current),
  `projects/spike_spdif_alex_dac/NOTES.md`, memory `project_spdif_optical_ram_fit`.
