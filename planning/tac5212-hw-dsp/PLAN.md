# TAC5212 hardware DSP — plan

Branch: `tac5212-hw-dsp`
Goal: surface the codec's onboard DSP (biquads, DRC, distortion limiter, IIR HPF,
interpolation filter mode, DAC digital volume) so we can shape tone + dynamics
**in the codec** instead of burning Teensy CPU on it. Move the TAC5212 controls
to a dedicated top-level tab and consolidate related tweaks there.

This is a working plan — not a contract. The phasing at the bottom is the order
of operations; everything above is the map.

---

## 1. Why

The Teensy is doing all the per-synth-sink DSP and the master path goes through
a Teensy-side shelf EQ + peak limiter (`processing-panel.ts`). The TAC5212 has
its own DSP chain that's currently unused:

- **3 biquads/channel × 4 DAC channels = 12 free hardware biquads** with
  32-bit coefficients (vs. Teensy float).
- A first-order programmable IIR / HPF on the DAC path.
- A DRC (dynamic range controller) per output channel — programmable
  threshold/ratio/attack/release/inflection.
- A signal distortion limiter with attack/release/threshold/inflection/slope.
- 4 interpolation filter modes (linear-phase / low-latency / ultra-low-latency
  / low-power) with audibly different transient/ringing character.
- Per-DAC digital volume control (-100..+27 dB, 0.5 dB steps) — currently
  only ADC DVOL is wrapped.
- Programmable channel mixer (DAC ASI mixer + side-chain mixer) — 32-bit
  coefficients per cross-point.

For the user's stated goal ("smoother, no ear fatigue") the most useful
levers are: high-shelf cut via DAC biquad, gentle DRC, and distortion limiter
as a soft-clip stage. Those become per-output controls in the codec — zero
Teensy cost, no zipper noise from float quantization at low levels.

---

## 2. Feature → register → handler → UI map

Page numbers below are TAC5212 register-page numbers (datasheet §8.2),
**not** doc page numbers. Coefficients are 32-bit two's complement, written
MSB-first as four sequential register bytes. Pages auto-increment from 0x7F
to next page's 0x08 to support burst writes.

### Per-DAC-channel DSP (4 channels × biquads + IIR + DRC + limiter)

| Feature                  | Page     | Reg range          | Lib placement          | OSC leaf                                    | UI control            |
|--------------------------|----------|--------------------|------------------------|---------------------------------------------|-----------------------|
| DAC biquad 1 coefficients| 15       | 0x08–0x1B (5 × 4B) | `Out::setBiquad(idx, BiquadCoeffs)` | `/codec/tac5212/dac/N/bq/I/coeffs ffffff`  | EQ widget per band    |
| DAC biquads 2..6         | 15       | 0x1C–0x7F          | same                   | same                                        | same                  |
| DAC biquads 7..12        | 16       | 0x08–0x7F          | same                   | same                                        | same                  |
| DAC biquad allocation    | Page 0   | 0x73 DSP_CFG1[3:2] | `setDacBiquadCount(0..3)` | `/codec/tac5212/dac/biquads i`           | Single enum (0/1/2/3 per ch) |
| DAC HPF mode (4 options) | Page 0   | 0x73 DSP_CFG1[5:4] | `setDacHpf(off|1|12|96|prog)` | `/codec/tac5212/dac/hpf s`              | Enum                  |
| DAC HPF custom IIR coefs | 17       | 0x78–0x7F + 18:0x08 | `setDacHpfCoefs(IirCoeffs)` | `/codec/tac5212/dac/hpf/coeffs fff`     | Optional "custom" subform |
| DAC interpolation filter | Page 0   | 0x73 DSP_CFG1[7:6] | `setDacInterpolationFilter()` | `/codec/tac5212/dac/interp s`           | Enum (linear/low-lat/ultra/low-power) |
| DAC DVOL channel 1A      | 18       | 0x0C–0x0F          | `Out::setDvol(dB)`     | `/codec/tac5212/dac/N/dvol f`               | Range slider          |
| DAC DVOL channel 1B/2A/2B| 18       | 0x10–0x1B          | same                   | same                                        | same                  |
| DAC DVOL gang flag       | Page 0   | 0x73 DSP_CFG1[0]   | `setDacDvolGang(bool)` | `/codec/tac5212/dac/dvol_gang i`            | Toggle                |
| DAC DVOL soft-step       | Page 0   | 0x73 DSP_CFG1[1]   | `setDacSoftStep(bool)` | `/codec/tac5212/dac/soft_step i`            | Toggle                |
| Distortion limiter coefs | 25       | 0x60–0x7F          | `setDacLimiter(LimiterCoeffs)` | `/codec/tac5212/dac/limiter/* f`        | Compound widget       |
| BOP / thermal foldback   | 26       | 0x14–0x5B          | `setBop(...)`/`setThermalFoldback(...)` | `/codec/tac5212/dac/bop/* f`        | (low priority — out of v1 scope) |
| DAC DRC (one global)     | 28       | 0x1C–0x3B          | `setDacDrc(DrcCoeffs)` | `/codec/tac5212/dac/drc/* f`                | Compound widget       |
| Limiter / DRC enable     | 1        | various            | `setDrcEnable()`/`setLimiterEnable()` | `/codec/tac5212/dac/{drc,limiter}/enable i` | Toggles |

### Per-ADC-channel DSP (already partially wrapped)

| Feature                  | Page     | Reg range          | Lib placement          | OSC leaf                                    | UI control            |
|--------------------------|----------|--------------------|------------------------|---------------------------------------------|-----------------------|
| ADC biquad 1..6 coefs    | 8        | 0x08–0x7F          | `Adc::setBiquad(idx,...)` | `/codec/tac5212/adc/N/bq/I/coeffs`       | EQ widget per band    |
| ADC biquad 7..12 coefs   | 9        | 0x08–0x7F          | same                   | same                                        | same                  |
| ADC biquad allocation    | Page 0   | 0x72 DSP_CFG0[3:2] | `setAdcBiquadCount()`  | `/codec/tac5212/adc/biquads i`              | Enum                  |
| ADC HPF mode             | Page 0   | 0x72 DSP_CFG0[5:4] | (already there as bool, expand to enum) | `/codec/tac5212/adc/hpf s` (was `i`) | Enum (was toggle)     |
| ADC AGC                  | 27/28    | 0x5C–0x0F          | `Adc::setAgc(AgcCoeffs)` | `/codec/tac5212/adc/N/agc/* f`            | (low priority — out of v1) |

### Hardware register notation

A "biquad coefficient" is 5 × 32-bit values: N0, N1, N2, D1, D2 in 5.27
fixed-point (datasheet §7.3.9.1.6, eq. 2). Library helper:

```cpp
struct BiquadCoeffs {
    int32_t n0;  // 5.27 fixed-point
    int32_t n1;
    int32_t n2;
    int32_t d1;
    int32_t d2;
};

// Compute from textbook biquad form (a0 normalized to 1)
BiquadCoeffs bqHighShelf(float fs, float fc, float dB, float Q);
BiquadCoeffs bqPeak(float fs, float fc, float dB, float Q);
BiquadCoeffs bqLowShelf(float fs, float fc, float dB, float Q);
BiquadCoeffs bqLowpass(float fs, float fc, float Q);
BiquadCoeffs bqHighpass(float fs, float fc, float Q);
BiquadCoeffs bqBypass();  // {0x7FFFFFFF, 0, 0, 0, 0} = unity all-pass
```

Coefficient designers run **on the host**, not on Teensy. UI computes coefs
from (type, freq, gain, Q) and ships the five 32-bit values over OSC. This
keeps Teensy code small and lets us show a frequency-response curve in the
browser by computing it from the same coefs we're shipping.

---

## 3. Driver-side changes (lib/TAC5212)

### New register tables

`lib/TAC5212/src/TAC5212_Registers.h` extensions:

```cpp
// Biquad pages — page 8 (ADC 1..6), 9 (ADC 7..12), 15 (DAC 1..6), 16 (DAC 7..12)
namespace adc_biquad {
    constexpr uint8_t PAGE_LO = 8;   // BQ 1..6
    constexpr uint8_t PAGE_HI = 9;   // BQ 7..12
    constexpr uint8_t BQ_STRIDE = 0x14; // 20 bytes per biquad (5 × 4)
    constexpr uint8_t BQ_BASE   = 0x08;
    inline constexpr uint8_t pageFor(uint8_t bq /*1..12*/) {
        return bq <= 6 ? PAGE_LO : PAGE_HI;
    }
    inline constexpr uint8_t baseFor(uint8_t bq /*1..12*/) {
        return BQ_BASE + ((bq - 1) % 6) * BQ_STRIDE;
    }
}
namespace dac_biquad {
    constexpr uint8_t PAGE_LO = 15;
    constexpr uint8_t PAGE_HI = 16;
    constexpr uint8_t BQ_STRIDE = 0x14;
    constexpr uint8_t BQ_BASE   = 0x08;
    inline constexpr uint8_t pageFor(uint8_t bq) { return bq <= 6 ? PAGE_LO : PAGE_HI; }
    inline constexpr uint8_t baseFor(uint8_t bq) { return BQ_BASE + ((bq - 1) % 6) * BQ_STRIDE; }
}

// DAC HPF + interpolation filter + biquad allocation share DSP_CFG1 (0x73)
constexpr uint8_t DSP_CFG1 = 0x73;
namespace dsp_cfg1 {
    constexpr uint8_t SHIFT_INTX_FILT = 6;
    constexpr uint8_t MASK_INTX_FILT  = 0xC0;
    constexpr uint8_t SHIFT_HPF_SEL   = 4;
    constexpr uint8_t MASK_HPF_SEL    = 0x30;
    constexpr uint8_t SHIFT_BQ_CFG    = 2;
    constexpr uint8_t MASK_BQ_CFG     = 0x0C;
    constexpr uint8_t MASK_DISABLE_SOFT_STEP = 0x02;
    constexpr uint8_t MASK_DVOL_GANG  = 0x01;

    constexpr uint8_t INTX_LINEAR_PHASE     = 0x00;
    constexpr uint8_t INTX_LOW_LATENCY      = 0x40;
    constexpr uint8_t INTX_ULTRA_LOW_LATENCY = 0x80;
    constexpr uint8_t INTX_LOW_POWER        = 0xC0;  // (note datasheet bit semantics)

    constexpr uint8_t HPF_PROGRAMMABLE = 0x00;
    constexpr uint8_t HPF_1HZ          = 0x10;
    constexpr uint8_t HPF_12HZ         = 0x20;
    constexpr uint8_t HPF_96HZ         = 0x30;

    constexpr uint8_t BQ_NONE = 0x00;
    constexpr uint8_t BQ_1    = 0x04;
    constexpr uint8_t BQ_2    = 0x08;
    constexpr uint8_t BQ_3    = 0x0C;
}

// Per-DAC-channel DVOL — page 18, 0x0C/0x10/0x14/0x18 for CH1A/CH1B/CH2A/CH2B
namespace dac_dvol {
    constexpr uint8_t PAGE = 18;
    constexpr uint8_t CH1A_BASE = 0x0C;
    constexpr uint8_t CH1B_BASE = 0x10;
    constexpr uint8_t CH2A_BASE = 0x14;
    constexpr uint8_t CH2B_BASE = 0x18;
    constexpr uint8_t STRIDE = 4;  // 32-bit volume coefficient
}

// Limiter — page 25, threshold/attack/release/inflection/slope
namespace dac_limiter {
    constexpr uint8_t PAGE = 25;
    constexpr uint8_t ATTACK_COEFF   = 0x60;  // 4 bytes
    constexpr uint8_t RELEASE_COEFF  = 0x64;
    constexpr uint8_t ENV_DECAY      = 0x68;
    constexpr uint8_t THRESHOLD_MAX  = 0x6C;
    constexpr uint8_t THRESHOLD_MIN  = 0x70;
    constexpr uint8_t INFLECTION_PT  = 0x74;
    constexpr uint8_t SLOPE          = 0x78;
    constexpr uint8_t RESET_COUNTER  = 0x7C;
}

// DAC DRC — page 28, 0x1C–0x3B
namespace dac_drc {
    constexpr uint8_t PAGE = 28;
    constexpr uint8_t MAX_GAIN       = 0x1C;
    constexpr uint8_t MIN_GAIN       = 0x20;
    constexpr uint8_t ATTACK_TC      = 0x24;
    constexpr uint8_t RELEASE_TC     = 0x28;
    constexpr uint8_t RELEASE_HOLD   = 0x2C;
    constexpr uint8_t RELEASE_HYST   = 0x30;
    constexpr uint8_t INV_RATIO      = 0x34;
    constexpr uint8_t INFLECTION_PT  = 0x38;
}
```

### New library API surface

`lib/TAC5212/src/TAC5212.h` — new types and methods:

```cpp
struct BiquadCoeffs {
    int32_t n0;
    int32_t n1;
    int32_t n2;
    int32_t d1;
    int32_t d2;
};

struct DrcCoeffs {
    float thresholdDb;     // -100 .. 0
    float ratio;           // 1..20 (1 = bypass)
    float attackMs;        // 1..500
    float releaseMs;       // 50..3000
    float maxGainDb;       // 0..30
    float kneeDb;          // 0..12 (soft-knee width)
};

struct LimiterCoeffs {
    float thresholdDb;     // -10 .. 0
    float attackMs;
    float releaseMs;
    float inflectionDb;
    float slope;
    float kneeDb;
};

enum class InterpFilter : uint8_t {
    LinearPhase, LowLatency, UltraLowLatency, LowPower
};

enum class DacHpf : uint8_t {
    Programmable,  // = "off" (default all-pass coefs)
    Cut1Hz,
    Cut12Hz,
    Cut96Hz,
};

class TAC5212 {
public:
    // ... existing API ...

    // Chip-global DAC DSP
    Result setDacInterpolationFilter(InterpFilter);
    Result setDacHpf(DacHpf);
    Result setDacBiquadsPerChannel(uint8_t n /*0..3*/);
    Result setDacDvolGang(bool);
    Result setDacSoftStep(bool enabled);

    // ADC biquad allocation (chip-global; per-channel coefs live on Adc)
    Result setAdcBiquadsPerChannel(uint8_t n /*0..3*/);
};

class TAC5212::Out {
public:
    // ... existing setMode/getMode ...

    // Per-output DSP (3 biquads max, idx 1..3)
    Result setBiquad(uint8_t idx, const BiquadCoeffs&);
    Result clearBiquad(uint8_t idx);  // writes bypass coefs

    // Per-output DAC digital volume
    Result setDvol(float dB);
    Result getDvol(float &dB);

    // Per-output dynamics
    Result setDrc(const DrcCoeffs&);
    Result setDrcEnable(bool);
    Result setLimiter(const LimiterCoeffs&);
    Result setLimiterEnable(bool);
};

class TAC5212::Adc {
public:
    // ... existing setMode/setImpedance/etc ...
    Result setBiquad(uint8_t idx, const BiquadCoeffs&);
    Result clearBiquad(uint8_t idx);
};
```

### Coefficient designer helpers

A separate header `lib/TAC5212/src/TAC5212_Biquad.h` (host-friendly, but
also Teensy-safe) provides closed-form designers using single-precision
float math. These are pure functions — no I²C — so the same code can also
run in TypeScript on the dev surface for the EQ-curve preview (port the
math). Testing strategy: a small `test_biquad_design.cpp` that designs
known-shape filters and checks coefficient values against precomputed
references (within tolerance).

### Page-cache invalidation

Lib `_selectPage()` is already idempotent. Coefficient writes that touch
pages 8/9/15/16/17/18/25/28 should burst write contiguous registers using
the auto-increment behavior (after 0x7F, page rolls to next page's 0x08).
Burst-write helper:

```cpp
Result _writeBurst(uint8_t page, uint8_t startReg, const uint8_t *bytes, size_t n);
```

This needs to handle the page boundary case (write 1..127, switch page, write
remainder) since one biquad spans 20 bytes which can cross a page boundary
only if biquad 6 ends at 0x7B + 4 = 0x7F → biquad 7 starts at 0x08 of next
page. Verify by testing biquad 6 + 7 writes round-trip correctly.

---

## 4. Tac5212Panel firmware handler additions

Extend `projects/.../src/Tac5212Panel.{h,cpp}` with one handler per new leaf.
The OSC tree grows like this:

```
/codec/tac5212/
  ├── (existing) /adc/N/{mode,impedance,fullscale,coupling,bw,dvol}
  ├── (existing) /adc/hpf  → CHANGE from `i` toggle to `s` enum
  ├── (existing) /vref/fscale
  ├── (existing) /micbias/{enable,level}
  ├── (existing) /out/N/mode
  ├── (existing) /pdm/{enable,source}
  ├── (existing) /reset, /wake, /info, /status, /reg/{set,get}
  │
  ├── (NEW) /adc/biquads i                    — ADC biquads per channel (0..3)
  ├── (NEW) /adc/N/bq/I/coeffs iiiii          — five int32 coefs (n0,n1,n2,d1,d2)
  ├── (NEW) /adc/N/bq/I/design fffff          — convenience: type, freq, dB, Q (host designs, but firmware accepts to allow re-snapshot)
  │
  ├── (NEW) /dac/N/dvol f                     — per-DAC digital volume
  ├── (NEW) /dac/N/bq/I/coeffs iiiii          — DAC biquad coefs
  ├── (NEW) /dac/biquads i                    — DAC biquads per channel
  ├── (NEW) /dac/dvol_gang i
  ├── (NEW) /dac/soft_step i
  ├── (NEW) /dac/hpf s                        — programmable | 1hz | 12hz | 96hz
  ├── (NEW) /dac/interp s                     — linear | low_latency | ultra_low_latency | low_power
  │
  ├── (NEW) /dac/N/drc/enable i
  ├── (NEW) /dac/N/drc/threshold f            — dB
  ├── (NEW) /dac/N/drc/ratio f
  ├── (NEW) /dac/N/drc/attack f               — ms
  ├── (NEW) /dac/N/drc/release f              — ms
  ├── (NEW) /dac/N/drc/max_gain f             — dB
  ├── (NEW) /dac/N/drc/knee f                 — dB
  │
  ├── (NEW) /dac/N/limiter/enable i
  ├── (NEW) /dac/N/limiter/threshold f
  ├── (NEW) /dac/N/limiter/attack f
  ├── (NEW) /dac/N/limiter/release f
  └── (NEW) /dac/N/limiter/knee f
```

Snapshot extension: `Tac5212Panel::snapshot()` must read back DVOL, biquad
coefs, DRC state, limiter state for each output channel and emit echoes,
so the dev surface tab populates correctly on connect.

**Design rule preserved:** per-channel leaves stay per-channel; chip-global
settings (HPF, interp filter, biquad count, gang) live at the chip-global
path level. No `dac/N/biquads` — that field is shared across all DAC
channels by hardware.

---

## 5. Web UI — hierarchical organization (the UX rule)

**The Codec tab uses 3 levels of hierarchy with clear section headers and
indented sub-settings.** Every screen reads top-down: a section header tells
you what part of the codec you're touching, then sub-settings underneath are
the knobs for that section.

```
LEVEL 1: Top-level tab            "Codec" (next to Mixer)
LEVEL 2: Sub-tab strip            "Signal Chain" | "DAC EQ" | "Dynamics" | "ADC" | "Routing" | "System"
LEVEL 3: Section panel            Bordered card with bold header + collapsible body
LEVEL 4: Sub-setting              Labeled control inside a section (slider / enum / toggle)
```

Every section header gets:
- Bold uppercase title (e.g. **OUTPUT 1 — DAC FILTER**)
- One-line plain-English subtitle (e.g. "How the codec converts digital to
  analog. Affects character of transients.")
- Optional "Reset to defaults" mini-button on the right edge of the header
- Collapse/expand chevron on the left edge

Sub-settings within a section get:
- Left-aligned label
- Control widget (slider/select/toggle) on the right
- Plain-English help line under the label when the section is expanded
- Visually consistent height per row so eye can scan a column of values

### Codec tab — full information architecture

```
[ Codec ] (top-level tab)
│
├── Sub-tab: SIGNAL CHAIN  (default landing — visualizes the whole flow)
│   ┌─────────────────────────────────────────────────────────────┐
│   │ AUDIO PATH OVERVIEW                                         │
│   │ Click any block to jump to its sub-tab.                     │
│   │                                                             │
│   │  [ ASI in ] → [ DAC ] → [ EQ ] → [ DRC ] → [ Limiter ]      │
│   │                       ↓ ↓ ↓                                 │
│   │                       OUT 1   OUT 2                         │
│   └─────────────────────────────────────────────────────────────┘
│
├── Sub-tab: DAC EQ
│   ┌─────────────────────────────────────────────────────────────┐
│   │ OUTPUT 1 — TONE SHAPING                                     │
│   │ 3 hardware EQ bands. Cut harshness, add warmth.             │
│   │                                                             │
│   │   [ live frequency-response curve ]                         │
│   │                                                             │
│   │   Band 1   Type [Low Shelf ▾]   100 Hz   +1.0 dB   Q 0.7    │
│   │   Band 2   Type [Peak ▾]        2.5 kHz  -1.5 dB   Q 1.2    │
│   │   Band 3   Type [High Shelf ▾]  8 kHz    -2.0 dB   Q 0.7    │
│   │                                                             │
│   │   Presets: [ Flat ] [ Smooth ] [ Bright ] [ Dark ]          │
│   └─────────────────────────────────────────────────────────────┘
│   ┌─────────────────────────────────────────────────────────────┐
│   │ OUTPUT 2 — TONE SHAPING                                     │
│   │ ... same layout ...                                         │
│   └─────────────────────────────────────────────────────────────┘
│   ┌─────────────────────────────────────────────────────────────┐
│   │ EQ ALLOCATION (chip-wide)                                   │
│   │ How many EQ bands per output. Trades flexibility for CPU.   │
│   │                                                             │
│   │   Bands per channel  [ 0 ] [ 1 ] [ 2 ] [●3]                 │
│   └─────────────────────────────────────────────────────────────┘
│
├── Sub-tab: DYNAMICS
│   ┌─────────────────────────────────────────────────────────────┐
│   │ OUTPUT 1 — DRC (compressor)                                 │
│   │ Tames loud transients smoothly. The "ear-fatigue fix".      │
│   │                                                             │
│   │   Enable        [ on ]                                      │
│   │   Threshold     ────●────  -20 dB                           │
│   │   Ratio         ──●──────   2.0:1                           │
│   │   Knee          ──●──────   6 dB                            │
│   │   Attack        ────●────   10 ms                           │
│   │   Release       ──────●──   200 ms                          │
│   │   Max gain      ────●────   12 dB                           │
│   │                                                             │
│   │   [ live transfer-curve plot ]    [ live GR meter ]         │
│   └─────────────────────────────────────────────────────────────┘
│   ┌─────────────────────────────────────────────────────────────┐
│   │ OUTPUT 1 — LIMITER (peak protection)                        │
│   │ Hard ceiling that prevents clipping at any cost.            │
│   │                                                             │
│   │   Enable        [ on ]                                      │
│   │   Threshold     ──────●──   -1 dBFS                         │
│   │   Attack        ●────────   0.1 ms                          │
│   │   Release       ────●────   50 ms                           │
│   │   Knee          ──●──────   2 dB                            │
│   └─────────────────────────────────────────────────────────────┘
│   ┌─────────────────────────────────────────────────────────────┐
│   │ OUTPUT 2 — DRC + LIMITER                                    │
│   │ ... same layout ...                                         │
│   └─────────────────────────────────────────────────────────────┘
│
├── Sub-tab: DAC FILTER & VOLUME
│   ┌─────────────────────────────────────────────────────────────┐
│   │ DIGITAL VOLUME                                              │
│   │ Per-output digital volume. -100 dB to +27 dB, 0.5 dB steps. │
│   │                                                             │
│   │   Output 1     ──────●──   0.0 dB                           │
│   │   Output 2     ──────●──   0.0 dB                           │
│   │   Gang outputs       [ off ]                                │
│   │   Soft-step on changes [ on ]                               │
│   └─────────────────────────────────────────────────────────────┘
│   ┌─────────────────────────────────────────────────────────────┐
│   │ INTERPOLATION FILTER (chip-wide)                            │
│   │ How the DAC reconstructs samples. Affects transient feel.   │
│   │                                                             │
│   │   Mode  [●Linear phase] [ Low latency ] [ Ultra ] [ Power ] │
│   │   Help: Linear phase = best frequency response, ~1 ms       │
│   │         Low latency  = less pre-ringing, ~0.5 ms            │
│   │         Ultra        = lowest latency, more aliasing        │
│   │         Power        = lowest current draw, audible roll-off│
│   └─────────────────────────────────────────────────────────────┘
│   ┌─────────────────────────────────────────────────────────────┐
│   │ DAC HIGH-PASS FILTER (chip-wide)                            │
│   │ Removes DC and infrasonic content from playback.            │
│   │                                                             │
│   │   Cutoff  [ Off ] [ 1 Hz ] [●12 Hz ] [ 96 Hz ]              │
│   └─────────────────────────────────────────────────────────────┘
│
├── Sub-tab: ADC  (existing — minor reorg into clearer sections)
│   ┌─────────────────────────────────────────────────────────────┐
│   │ LINE INPUT                                                  │
│   │   Mode  [●Stereo ] [ Mono ]                                 │
│   └─────────────────────────────────────────────────────────────┘
│   ┌─────────────────────────────────────────────────────────────┐
│   │ CHANNEL 1                                                   │
│   │   Digital gain   ────●────   0.0 dB                         │
│   │   Mode           [ Differential ▾ ]                         │
│   │   Impedance      [ 5k ] [●10k ] [ 40k ]                     │
│   │   Full scale     [●2 Vrms ] [ 4 Vrms ]                      │
│   │   Coupling       [ AC ▾ ]                                   │
│   │   Bandwidth      [●24 kHz ] [ 96 kHz ]                      │
│   └─────────────────────────────────────────────────────────────┘
│   ┌─────────────────────────────────────────────────────────────┐
│   │ CHANNEL 2                                                   │
│   │ ... same layout ...                                         │
│   └─────────────────────────────────────────────────────────────┘
│   ┌─────────────────────────────────────────────────────────────┐
│   │ ADC HIGH-PASS FILTER (chip-wide)                            │
│   │   Cutoff  [ Off ] [ 1 Hz ] [●12 Hz ] [ 96 Hz ]              │
│   └─────────────────────────────────────────────────────────────┘
│
├── Sub-tab: ROUTING & REFERENCE
│   ┌─────────────────────────────────────────────────────────────┐
│   │ OUTPUT DRIVERS                                              │
│   │   Output 1 mode  [ Diff line ▾ ]                            │
│   │   Output 2 mode  [ Diff line ▾ ]                            │
│   └─────────────────────────────────────────────────────────────┘
│   ┌─────────────────────────────────────────────────────────────┐
│   │ VOLTAGE REFERENCE                                           │
│   │   VREF full scale  [ 2.75 V ▾ ]                             │
│   └─────────────────────────────────────────────────────────────┘
│   ┌─────────────────────────────────────────────────────────────┐
│   │ MIC BIAS                                                    │
│   │   Enable [ off ]                                            │
│   │   Level  [ 2.75 V ▾ ]                                       │
│   └─────────────────────────────────────────────────────────────┘
│   ┌─────────────────────────────────────────────────────────────┐
│   │ PDM MIC                                                     │
│   │   Enable  [ off ]                                           │
│   │   Source  [ GPI1 ▾ ]                                        │
│   └─────────────────────────────────────────────────────────────┘
│
└── Sub-tab: SYSTEM  (existing — same controls)
    ┌─────────────────────────────────────────────────────────────┐
    │ DEVICE                                                      │
    │   [ Reset ]  [ Wake: on ]  [ Info ]  [ Status ]             │
    └─────────────────────────────────────────────────────────────┘
    ┌─────────────────────────────────────────────────────────────┐
    │ DIAGNOSTICS                                                 │
    │   Raw register write/read  (existing /reg/set, /reg/get)    │
    └─────────────────────────────────────────────────────────────┘
```

### Section header convention (CSS)

Add three new CSS classes to the codec panel stylesheet:

```css
.codec-section-header {
  font-weight: 700;
  text-transform: uppercase;
  letter-spacing: 0.05em;
  font-size: 0.95rem;
  border-bottom: 1px solid var(--border);
  padding: 0.5rem 0.75rem;
  display: flex;
  align-items: baseline;
  gap: 0.75rem;
  cursor: pointer; /* collapse/expand */
}
.codec-section-subtitle {
  font-weight: 400;
  font-size: 0.85rem;
  color: var(--muted);
  text-transform: none;
  letter-spacing: normal;
}
.codec-section-body {
  padding: 0.5rem 0.75rem 0.75rem;
  display: grid;
  grid-template-columns: minmax(140px, 1fr) 2fr;
  row-gap: 0.5rem;
  column-gap: 1rem;
  align-items: center;
}
```

The single 2-column grid in `.codec-section-body` is what gives the
"label : control" alignment that scans cleanly down the page.

### Help text source of truth

Each control gets a `help?: string` field on the `Control` type. When
present, the dispatcher renders it under the label. Helps are short
("Tames loud transients smoothly"), not engineering paragraphs.

```ts
export type Control =
  | { kind: 'enum'; label: string; help?: string; address: string; options: string[]; ... }
  | { kind: 'toggle'; label: string; help?: string; address: string }
  | ...
```

### Existing codec-panel-config.ts structure stays — extended with sections

The current types `Tab → Group → Control` map cleanly to `sub-tab → section
→ sub-setting`. We rename `Group` to `Section` and add `subtitle?: string`
+ `collapsible?: boolean` + `defaultCollapsed?: boolean` + `presets?:
PresetButton[]`. Migration is a sed; existing controls keep working.

---

## 6. Web UI — codec-panel-config additions

`tools/web_dev_surface/src/codec-panel-config.ts` gains new tabs. The
existing declarative `Control` types (`enum | toggle | range | action`) cover
most needs but biquad EQ + dynamics need new control kinds:

```ts
export type Control =
  | { kind: 'enum'; ... }
  | { kind: 'toggle'; ... }
  | { kind: 'action'; ... }
  | { kind: 'range'; ... }
  // NEW
  | { kind: 'biquad'; label: string; address: string; defaultType: BiquadType; defaultFreq: number; defaultDb: number; defaultQ: number }
  | { kind: 'drc'; label: string; addressBase: string }       // expands to 7 address leaves
  | { kind: 'limiter'; label: string; addressBase: string };  // expands to 5 address leaves
```

Three new top-level codec-panel tabs:

- **DAC EQ** — per-output channel column with 3 biquad bands. Each band has:
  type select (peak / low-shelf / high-shelf / low-pass / high-pass / off),
  frequency knob (20 Hz–20 kHz log), gain knob (-12..+12 dB), Q knob
  (0.5..10). A live frequency-response plot above the bands shows the
  composite curve. Slider drag throttles to 30 Hz (existing `sendThrottled`).
- **DAC Dynamics** — per-output DRC + limiter compound widgets. DRC controls:
  threshold, ratio, attack, release, max-gain, knee (six sliders + on
  toggle). Optional transfer-curve plot. Limiter controls: threshold,
  attack, release, knee (four sliders + on toggle).
- **DAC Filter & DVOL** — interpolation filter mode enum, HPF mode enum,
  per-channel DVOL slider, gang toggle, soft-step toggle, biquads-perchannel enum.

Existing tabs (ADC, VREF/MICBIAS, Output mode, PDM, System) stay where they
are. ADC EQ becomes an additional tab if + when we expose ADC biquads
(low priority since the mic side isn't where ear fatigue lives).

### Biquad widget rendering plan

`ui/codec-panel.ts`'s `renderControl()` adds a `biquad` branch that
constructs a small DOM block: type select + 3 sliders + a
`<canvas>` for the response curve. The same TS-side biquad math used to
encode coefficients drives the curve preview. Coefficient encoding:

```ts
// 5.27 fixed-point: int32 = round(coef * 2^27); clamp to [-2^31, 2^31-1].
function toQ27(coef: number): number {
  return Math.max(-0x80000000, Math.min(0x7FFFFFFF, Math.round(coef * (1 << 27))));
}
```

OSC types `iiiii` for biquad coefficients (5 × int32). Five separate args
keep wire format simple and avoid a custom blob type.

---

## 7. Tab restructure in main.ts

Today the codec panel renders inline at the bottom of the **Mixer** tab
(see `main.ts:297-299`). Move it to a dedicated **Codec** top-level tab.

### Before

```
[ Mixer | Spectrum | Synth | FX | Processing | Loop | Beats | Clock | Arp ]
└── Mixer view: channel strips + codec panel (inline) + raw OSC
```

### After

```
[ Mixer | Codec | Spectrum | Synth | FX | Processing | Loop | Beats | Clock | Arp ]
                ^^^^^
                NEW
```

Place **Codec** as second tab (right after Mixer) since it's a configuration
view, not a performance view.

### What moves into the new Codec tab

- **Stays in Codec tab (originally codec-panel):** existing ADC, VREF/MICBIAS,
  Output mode, PDM, System sub-tabs.
- **Adds in Codec tab (this branch):** DAC EQ, DAC Dynamics, DAC Filter &
  DVOL sub-tabs.
- **Migrates from Processing tab:** the Teensy-side shelf-EQ + limiter UI
  in `processing-panel.ts` becomes redundant once DAC EQ + Dynamics ship.
  Plan: keep the Processing tab in v1 for fallback, label it "Software
  processing" vs. the new "Codec processing"; deprecate in a follow-up
  branch once parity is verified by ear.
- **Stays in Mixer tab:** the raw-OSC pane stays where it is. The codec
  inline section gets removed.

### What leaves the Mixer tab

`mixerView.append(mixerRow, codecSection, rawSection)` →
`mixerView.append(mixerRow, rawSection)`. The `codecSection` element gets
moved into a new `codecView` section that mirrors the existing pattern of
`spectrumSection` / `synthSection` / etc.

### Sub-tabs inside the Codec view

The codec-panel already has its own internal tab bar (rendered by
`renderControl()`). We piggyback on that — adding new entries to
`tac5212Panel: Tab[]` automatically grows the sub-tab strip. No new
tab-switching machinery in main.ts needed for sub-tabs; just for the
new top-level Codec tab.

---

## 8. Tradeoffs & open questions

1. **Coefficient designer location.** Three options:
   - (a) JS-only — UI computes coefficients, ships int32 quintuples. Snapshot
     can't reproduce sliders — only the coefficient array round-trips, and the
     UI has no way to know "these came from a peak filter at 1 kHz +3 dB".
   - (b) Both — UI computes for sliders → int32 → ships, but ALSO ships
     the source params (`/dac/N/bq/I/design fffff` with type/freq/dB/Q).
     Firmware stores the design params alongside the coefs so snapshot
     reproduces sliders. **Recommended.** Costs 8 bytes per band of RAM.
   - (c) Firmware computes — moves math onto Teensy. Doable but no point
     if (b) is cheap.

2. **Biquad allocation count is chip-global, not per-channel.** Datasheet
   §7.3.9.2.4: `DAC_DSP_BQ_CFG[1:0]` is a single 2-bit field in
   DSP_CFG1 governing **all four** DAC channels. That means the UI
   biquad count is ONE control, not four. We can still allow each
   channel's bands to be independently configured — just that the count
   ceiling is shared. Consequence: choose the count first ("3 bands per
   channel"), then design per-channel.

3. **Migration of processing-panel.ts.** Initially keep both paths. The
   Teensy shelf+limiter still has a use case if the user wants to bypass
   the codec processing entirely (e.g. measuring raw codec output). A
   follow-up branch can simplify when parity is confirmed.

4. **DAC HPF "programmable" mode is the all-pass default.** Setting
   `/dac/hpf` to `programmable` does NOT load custom coefficients — it
   loads the chip's POR-default coefs which are an all-pass filter,
   effectively HPF off. This matches the existing ADC HPF semantics.
   Custom IIR coefficients are a v2 feature.

5. **DAC interpolation filter affects all channels.** Like biquad count,
   this is a single chip-global enum.

6. **Snapshot bandwidth.** A complete snapshot post-this-branch reads
   back: 4 channels × 12 biquads × 5 coefficients × 4 bytes + DRC + limiter
   + ASI snapshot. That's ~1 KB per snapshot if everything is wired,
   with current 30 Hz throttle that's fine. If the dev surface gets
   slow on connect, batch the readbacks behind a "fetch coefficients"
   button instead of in `snapshot()`.

7. **Soft-step disabled by default in this work.** The chip's soft-step
   only applies to DVOL changes, not biquad coef changes. Coef changes
   click if applied during audio. Solutions:
   - Apply on idle (when DAC is muted)
   - Crossfade by ramping DVOL down → write coefs → ramp DVOL up
   - Accept the click for studio-grade parameter-tweaking workflow
   v1 takes the third option; v2 can add the crossfade if the click is
   audible.

8. **/snapshot reply size.** A full codec snapshot grows from ~30 fields
   to ~150+ fields. Frame size and CDC throughput need to be confirmed.
   Existing throttle is per-message, so the bundle splits naturally.

9. **OSC leaf naming for DAC channels.** TAC5212 DAC has channels 1A/1B/2A/2B
   (4 single-ended) but typical use is 2 differential outputs (1, 2). UI
   should expose differential pairs (channels 1 and 2) and let firmware
   decide whether to gang the A/B pair. Non-differential modes (stereo
   single-ended, pseudo-diff) can come in a v2 if needed.

---

## 9. Phasing

Goal: each phase ends with something working that passes ear+meter checks.

### Phase 1 — DAC DVOL + interpolation filter (low risk, high value)

- Lib: `Out::setDvol`, `Out::getDvol`, `setDacInterpolationFilter`,
  `setDacSoftStep`, `setDacDvolGang`.
- Panel: handlers + snapshot.
- UI: new Codec tab in main.ts; new DAC Filter & DVOL sub-tab with these
  controls.
- Move existing codec sections from Mixer to Codec tab.
- Acceptance: change DVOL from UI, hear gain change. Switch interpolation
  filter, listen for transient/character difference. Codec tab populated
  on connect.

### Phase 2 — DAC biquad EQ (the headline ear-fatigue fix)

- Lib: `Out::setBiquad`, `Out::clearBiquad`, `setDacBiquadsPerChannel`,
  `BiquadCoeffs` + designer helpers in `TAC5212_Biquad.h`.
- Panel: 3 biquad coef handlers per output × 2 outputs = 6 new leaves.
  Plus the chip-global `dac/biquads` count.
- UI: DAC EQ sub-tab with 6 bands (3 per output × 2). Real-time response
  curve. Source-param round-trip (option (b) above).
- Acceptance: dial in a -2 dB shelf at 8 kHz on output 1, hear it; spectrum
  plot shows the cut. Round-trip on disconnect/reconnect preserves slider
  positions.

### Phase 3 — DAC DRC + distortion limiter

- Lib: `Out::setDrc`, `Out::setLimiter`, enable toggles, coefficient encoders
  (attack/release time → exponential coefficient, threshold-dB → linear gain
  threshold per datasheet pages 220–222).
- Panel: 12 new leaves (6 DRC + 5 limiter + enables × 2 outputs).
- UI: DAC Dynamics sub-tab with two compound widgets per output, plus
  optional gain-reduction meter (subscribe to `/codec/tac5212/dac/N/gr` if
  the chip exposes it; otherwise infer from input vs. output level).
- Acceptance: send a hot signal, watch limiter bring it down without audible
  pumping. Set DRC ratio 4:1 with -20 dBFS threshold, hear soft-knee
  compression on transients.

### Phase 4 — Polishing + Processing tab consolidation

- Audit the Processing tab: which controls duplicate the new Codec tab?
- Add a labeling pass to disambiguate Software vs Codec processing.
- Add factory presets to DAC EQ (e.g. "Smooth" = -2 dB high-shelf @ 8 kHz,
  "Flat", "Bright", "Dark") via the existing action button kind.
- Document the canonical OSC tree extensions in
  `planning/osc-mixer-foundation/02-osc-protocol.md`.

### Out of scope for this branch

- ADC biquads (low priority since input side isn't the ear-fatigue lever).
- ADC AGC (separate large feature).
- Brownout protection / thermal foldback (datasheet pages 220–222 — useful
  but no ear-fatigue impact).
- Mixer cross-channel coefficients (page 17 ASI DIN mixer) — deserves its
  own design pass.
- Voice activity detection (VAD/UAD) — separate feature scope.

---

## 10. Files touched (when implementation starts)

```
lib/TAC5212/src/TAC5212.h                   +30 lines (types, methods)
lib/TAC5212/src/TAC5212.cpp                 +200 lines (impls)
lib/TAC5212/src/TAC5212_Registers.h         +60 lines (page tables)
lib/TAC5212/src/TAC5212_Biquad.h            new (~120 lines)
lib/TAC5212/src/TAC5212_Biquad.cpp          new (~150 lines)

projects/.../src/Tac5212Panel.h             +20 lines (handler decls)
projects/.../src/Tac5212Panel.cpp           +400 lines (handler impls + snapshot)
projects/.../src/main.cpp                   no change (wiring already in place)

projects/.../tools/web_dev_surface/src/codec-panel-config.ts   +250 lines
projects/.../tools/web_dev_surface/src/ui/codec-panel.ts       +150 lines (new control kinds)
projects/.../tools/web_dev_surface/src/ui/biquad-widget.ts     new (~200 lines)
projects/.../tools/web_dev_surface/src/ui/dynamics-widget.ts   new (~150 lines)
projects/.../tools/web_dev_surface/src/biquad-design.ts        new (~120 lines, mirrors firmware math)
projects/.../tools/web_dev_surface/src/dispatcher.ts           +60 lines (codec listener fanout for biquad/drc/limiter compound writes)
projects/.../tools/web_dev_surface/src/main.ts                 +40 lines (Codec view section + tab wiring)
```

Total: ~2000 LOC across firmware + UI, in 3 phases. Phase 1 alone is ~300 LOC
and ships the tab restructure + DVOL + interpolation filter — small, useful,
verifiable.

---

## 11. Quick reference

- Datasheet: TI TAC5212 SLASF23A (Dec 2023, rev Jan 2025)
- Library README: `lib/TAC5212/README.md`
- Existing OSC protocol notes: `planning/osc-mixer-foundation/02-osc-protocol.md`
- Existing codec panel UI: `tools/web_dev_surface/src/ui/codec-panel.ts`
- Existing codec panel descriptor: `tools/web_dev_surface/src/codec-panel-config.ts`
- Existing firmware handler: `projects/.../src/Tac5212Panel.{h,cpp}`
- This worktree: `.claude/worktrees/tac5212-hw-dsp/`
