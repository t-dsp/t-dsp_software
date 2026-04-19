# Build out the TAC5212 driver + web dev tool

## Where to do the work

**Branch:** `tac5212-driver-extraction` (already exists, not pushed, 5 commits ahead of master, worktree at `.claude/worktrees/tac5212-driver-extraction/`).

The handoff doc on that branch ([HANDOFF_path_b_codec_extraction.md](.claude/worktrees/tac5212-driver-extraction/planning/HANDOFF_path_b_codec_extraction.md)) already names four "remaining raw writes" in `setupCodec()` that want typed lib setters — PDM bringup, ADC SE_MUX_INP mode, INTF_CFG1/2 DOUT/DIN, status read. Start there because those are the smallest, most concrete asks and they retire hand-rolled `writeRegister()` calls one at a time. They don't need hardware experimentation — the byte values are already known-good.

Once those are clean, grow the lib into actual DSP surface in roughly this order. Each phase is a standalone merge target.

## Phase 0 — close handoff gaps (pure refactor, low risk)

1. **PDM stubs** → implement `pdm().setEnable / setSource / setClkPin` per datasheet §7.3.7. Registers: `GPIO1_CFG` 0x0A, `GPI1_CFG` 0x0D, `INTF_CFG4` 0x13. Retires three raw writes.
2. **AdcMode::SingleEndedMuxInp** → add 4th enum value, add `adcModeToInsrc()` case, add matching OSC enum string `"se_mux_inp"` in `Tac5212Panel::handleAdcMode()`, add the option in `codec-panel-config.ts` adc mode enum. Retires two raw writes.
3. **DOUT/DIN config** → extend `SerialFormat` struct with `doutEnable, doutDrive, dinEnable` fields OR add sibling `setSerialIO(...)`. Retires two raw writes.
4. Delete `projects/.../src/tac5212_regs.h` entirely once nothing in `main.cpp` references it.

No OSC/UI additions in this phase — pure lib/bringup hygiene.

## Phase 1 — ADC fine gain + phase calibration (per-channel, trivial)

Lib: add `adc(N).setFineGain(float dB)` (±0.7 dB, 0.1 dB steps, `ADC_CHx_CFG3` bits [7:4]) and `adc(N).setPhaseCal(uint8_t cycles)` (0..63 mod-clock cycles, `ADC_CHx_CFG4` bits [7:2]).

OSC: `/codec/tac5212/adc/N/fgain f` and `/codec/tac5212/adc/N/pcal i`.

Web: two more sliders inside the existing `adcChannel(n)` group in `codec-panel-config.ts`. Slider range, unit, echo format — the pattern is already there from `dvol`.

Low risk, no cross-channel interaction, no register-page juggling (page 0 only). Useful right away: phase-align a stereo mic pair without pulling a scope.

## Phase 2 — decimation/interpolation filter selection (chip-global)

Lib: `setAdcDecimationFilter(DecimationFilter)` and `setDacInterpolationFilter(InterpolationFilter)` with enum values `LinearPhase / LowLatency / UltraLowLatency / LowPower`. Writes `DSP_CFG0[7:6]` + `ADC_LOW_PWR_FILT` (0x4E bit 2) and the DAC side in `DSP_CFG1[7:6]` + 0x4F.

OSC: `/codec/tac5212/adc/filter s` and `/codec/tac5212/dac/filter s`.

Web: enum dropdown in a new "DSP" tab section. This is where the web UI stops being analog-front-end-only and starts showing real DSP.

## Phase 3 — biquad filters (the big one)

This is the feature users will care about most: per-channel EQ on the codec side. Design decisions up front:

- **Where in the software stack?** The codec driver exposes register primitives. EQ as a mixer concept (bands, Q, frequency, gain) belongs in `TDspMixer`, not `lib/TAC5212`. So: lib gets `adc(N).setBiquad(index, const BiquadCoeffs&)` and a `DSP_CFG0[3:2]` count selector. TDspMixer grows an `EqBand` abstraction that targets *either* codec biquads or software biquads depending on where the channel sources from. Phase 3a is just the codec setters; Phase 3b is the mixer integration. Keep them separate.
- **On-the-fly vs boot-only?** The chip supports on-the-fly biquad updates in 2-channel mode (`EN_BQ_OTF_CHG`, B0_P1_R3 bit 0). Enable it by default in `begin()`.
- **Coefficient format?** 32-bit 2's-complement, 5 registers per biquad (N0, N1, N2, D1, D2). Compute in floating point on the host, serialize on the firmware side. Don't expose raw hex on OSC — expose `{type: "peaking|lowshelf|...", freq, q, gain_db}` and convert.

OSC: `/codec/tac5212/adc/N/eq/K/set fffi` (freq, q, gain, type_index). Web: a proper EQ curve visualization, not just sliders. This is where the branch starts feeling like a real mixer.

## Phase 4 — mixer matrix (channel routing)

Lib: `setAdcMixer(srcCh, dstCh, float gain01)` and `setDinMixer(...)` covering the 4×4 ADC mixer (page 10) and the ASI-DIN→DAC mixer (page 17). Each coefficient is one I2C burst write. Matrix-style UI in the web tool — a grid of faders.

This is board-specific to surface on the small-mixer UI (a fully routable mixer doesn't match the physical controls). So: keep the matrix in the `/codec/tac5212/` subtree as an engineer-facing debug surface, don't promote it into the `/ch` mixer tree.

## Phase 5 — protection + activity detection

AGC, DRC, distortion limiter, brownout protection, thermal foldback, VAD/UAD. These are large coefficient blocks (page 25-28) and most applications don't need them. Implement on demand — probably leave the library stubs in place until someone has a concrete use case.

## How each phase flows through the three layers

The pattern is set by the existing `adc(N).setDvol()` → `/codec/tac5212/adc/N/dvol` → web slider chain:

1. **`lib/TAC5212/src/TAC5212.{h,cpp}`** — add the register constants to `TAC5212_Registers.h`, add the typed method (enum arg or validated float arg), RMW the register, return `Result`.
2. **`projects/.../src/Tac5212Panel.cpp`** — add a `handleXxx()` method, route to it from `route()` by string-matching the OSC path, call the lib setter, echo back on success.
3. **`projects/.../tools/web_dev_surface/src/codec-panel-config.ts`** — add a `Control` entry in the right group/tab. Match enum strings exactly with what `Tac5212Panel` echoes.
4. **`planning/osc-mixer-foundation/02-osc-protocol.md`** — document the new leaf in the `/codec/tac5212/` subtree section.
5. **Memory** — update `tac5212_osc_subtree.md` so future sessions don't re-invent dropped leaves.

Every phase should end in a clean `pio run` + a bench flash + a smoke test on the web UI. The existing branch's 5-commit cadence (small lib change / panel change / UI change / doc update) is a good template.

## Risk notes

- Biquad page switching (page 0 → page 10/11/15-17) is the first time `_selectPage()` gets exercised heavily. It's in the lib already but hasn't been stressed. Add a cheap page-hygiene assert (or just always return to page 0 after a biquad burst) before Phase 3.
- On-the-fly biquad updates + soft-step DVOL can double-ramp and sound weird. If that shows up, disable DVOL soft-step during biquad writes — the chip has `ADC_DSP_DISABLE_SOFT_STEP` exactly for this.
- The web UI is currently laid out for 2 ADC + 2 DAC channels. Adding EQ per channel will blow up vertical real estate — plan for collapsible panels before Phase 3, not after.
