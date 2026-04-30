// TAC5212 — register-level driver for the TI TAC5212 pro-audio codec.
//
// This library exposes chip-specific primitives only. Mixer-level concerns
// (fader, mute, EQ, metering, routing) belong to the TDspMixer framework.
// See planning/osc-mixer-foundation/02-osc-protocol.md for the canonical
// `/codec/tac5212/...` OSC subtree that this class mirrors 1:1.
//
// Design rule:
//   Per-channel leaves must not have chip-global side effects. If a knob
//   affects more than one channel, it lives on the TAC5212 class as a
//   chip-global method, not on a per-channel accessor.
//
// The architecture targets `-fno-rtti` and `-fno-exceptions` (Teensy default).
// `Result` is a plain value type, no exceptions, no polymorphism. All setters
// return `Result` so callers can distinguish Ok / Warning / Error outcomes.

#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <stdint.h>

namespace tac5212 {

// -----------------------------------------------------------------------------
// Result — library <-> caller contract
// -----------------------------------------------------------------------------
//
// Every setter returns Result. Invariants:
//   - status == Ok      : operation committed, no caveats
//   - status == Warning : operation committed, but there's something the
//                         caller should know (e.g. GPIO override shadows
//                         an I2C MICBIAS write)
//   - status == Error   : operation did NOT commit (invalid combo, I2C NACK,
//                         out-of-range argument)
//
// `message` is a static C string (no allocation, no ownership). Callers may
// pass it verbatim into an OSC reply bundle or print it to Serial.

enum class ResultStatus : uint8_t {
    Ok = 0,
    Warning = 1,
    Error = 2,
};

struct Result {
    ResultStatus status;
    const char  *message;

    bool isOk()      const { return status == ResultStatus::Ok; }
    bool isWarning() const { return status == ResultStatus::Warning; }
    bool isError()   const { return status == ResultStatus::Error; }

    static Result ok()                         { return {ResultStatus::Ok, nullptr}; }
    static Result warning(const char *msg)     { return {ResultStatus::Warning, msg}; }
    static Result error(const char *msg)       { return {ResultStatus::Error, msg}; }
};

// -----------------------------------------------------------------------------
// Enumerations (1:1 with OSC leaf enum strings)
// -----------------------------------------------------------------------------

enum class AdcMode : uint8_t {
    Differential,          // INxP/INxM as differential pair (INSRC=00)
    SingleEndedInp,        // INxP only, INxM ignored (INSRC=10). The sensible
                           // default for SE — safe regardless of what INxM is
                           // wired to on the board.
    SingleEndedInpInmGnd,  // INxP as signal, INxM as ground reference (INSRC=01).
                           // ONLY correct if the board AC-couples INxM to ground;
                           // otherwise INxM's signal subtracts from INxP.
    SingleEndedInm,        // INxM only, INxP ignored (INSRC=11)
    // NOTE: "pdm_input" appears in the planning doc OSC tree but has no
    // matching hardware field in ADC_CH*_CFG0. PDM is handled entirely via
    // the TAC5212::pdm() subsystem. This enum deliberately omits a Pdm value;
    // the planning doc should be updated to drop it from /adc/N/mode.
};

enum class AdcImpedance : uint8_t {
    K5,   // 5 kOhm — lowest noise, most current drawn from source
    K10,  // 10 kOhm
    K40,  // 40 kOhm — highest impedance, required for 96 kHz wide-BW mode
};

enum class AdcFullscale : uint8_t {
    V2rms,  // 2 Vrms differential (1 Vrms SE). Default. Works in all modes.
    V4rms,  // 4 Vrms differential (2 Vrms SE). Only valid with VREF 2.75 V,
            // high-CMRR coupling, and 24 kHz audio bandwidth.
};

enum class AdcCoupling : uint8_t {
    Ac,              // AC-coupled, 50 mVpp SE / 100 mVpp diff CM tolerance
    DcLow,           // AC/DC-coupled, 500 mVpp SE / 1 Vpp diff CM tolerance
    DcRailToRail,    // AC/DC-coupled, rail-to-rail CM tolerance (high CMRR)
};

enum class AdcBw : uint8_t {
    Audio24k,  // 24 kHz audio bandwidth. Default. Supported at all impedances.
    Wide96k,   // 96 kHz wide bandwidth. Requires 40 kOhm impedance.
};

enum class OutMode : uint8_t {
    DiffLine,     // Differential routing, line driver (300 Ohm min load)
    SeLine,       // Mono-SE at OUTxP, line driver
    HpDriver,     // Mono-SE at OUTxP, headphone driver (16 Ohm min load)
    FdReceiver,   // FD receiver driver (high DR/SNR with receiver loads)
};

enum class VrefFscale : uint8_t {
    V2p75,   // 2.75 V VREF — 2 Vrms diff / 1 Vrms SE full-scale. Default.
    V2p5,    // 2.5 V VREF
    V1p375,  // 1.375 V VREF
};

enum class MicbiasLevel : uint8_t {
    // The achievable voltages depend on the VREF_FSCALE setting per
    // datasheet Table 7-16. The library validates the combination on every
    // write and returns Result::error for reserved combos.
    SameAsVref,  // Output equals current VREF voltage
    HalfVref,    // Output equals 0.5 * current VREF voltage
    Avdd,        // Output bypassed directly to AVDD
};

enum class PdmSource : uint8_t {
    Gpi1,
    Gpi2,
};

enum class PdmClkPin : uint8_t {
    Gpio1,
    // Gpio2 reserved — not yet verified which TAC5212 GPIO variants carry
    // the PDM clock function. Add after datasheet check.
};

// DAC interpolation filter response (chip-global, all outputs).
// Linear-phase = cleanest frequency response but ~1 ms latency and pre-ringing.
// Lower-latency modes ring less but allow more aliasing.
enum class InterpFilter : uint8_t {
    LinearPhase,
    LowLatency,
    UltraLowLatency,
    LowPower,
};

// DAC high-pass filter (chip-global). Programmable mode loads the POR
// default all-pass coefs (HPF off); the three fixed cutoffs are
// hardware-set -3 dB points.
enum class DacHpf : uint8_t {
    Programmable,
    Cut1Hz,
    Cut12Hz,
    Cut96Hz,
};

// Biquad coefficients in TAC5212's 5.27 fixed-point format. The chip
// pre-doubles N1 and D1 in the transfer function (datasheet §7.3.9.1.6),
// so coefs stored here are textbook N1/D1 divided by two. Designers in
// TAC5212_Biquad.h handle the halving; callers feeding raw coefs must
// do it themselves.
struct BiquadCoeffs {
    int32_t n0;
    int32_t n1;
    int32_t n2;
    int32_t d1;
    int32_t d2;

    // POR all-pass: y[n] = x[n].
    static constexpr BiquadCoeffs bypass() {
        return BiquadCoeffs{0x7FFFFFFF, 0, 0, 0, 0};
    }
};

// -----------------------------------------------------------------------------
// DeviceInfo / Status — read-only structs returned by info() / readStatus()
// -----------------------------------------------------------------------------

struct DeviceInfo {
    const char *model;         // always "TAC5212"
    uint8_t     i2cAddr;       // e.g. 0x51
    uint8_t     pageInUse;     // current selected register page (normally 0)
};

struct Status {
    uint8_t  devSts0;               // DEV_STS0 raw (channel power bits)
    uint8_t  devSts1;               // DEV_STS1 raw (mode/PLL/fault bits)
    bool     pllLocked;             // derived: DEV_STS1 bit 4
    bool     micBiasActive;         // derived: DEV_STS1 bit 3
    bool     faultActive;           // derived: DEV_STS1 bit 1
    bool     micBiasGpioOverride;   // derived: EN_MBIAS_GPIO != 0
};

// -----------------------------------------------------------------------------
// DAC dynamics: distortion limiter and DRC coefficients
// -----------------------------------------------------------------------------
//
// Both the limiter (page 25) and DRC (page 28) are 8 × 32-bit programmable
// coefficient blocks. The chip's POR defaults are pre-tuned and immediately
// usable — see `LimiterCoeffs::chipDefault()` and `DrcCoeffs::chipDefault()`.
//
// For ear-fatigue treatment, scope is intentionally limited: the driver lets
// callers ship raw 32-bit coefficients (so a host or PPC3-derived tuning
// can be loaded verbatim) and exposes a small set of presets. Closed-form
// time-constant / threshold derivation is NOT in v1 because the datasheet
// does not document the precise encoding formulas, and incorrect dynamics
// coefficients can produce loud transients. Add a designer header later
// once a known-good encoding is verified by ear.

struct DrcCoeffs {
    int32_t maxGain;        // P28 R0x1C — max post-DRC gain (dB-encoded, chip-specific)
    int32_t minGain;        // P28 R0x20 — min post-DRC gain (dB-encoded)
    int32_t attackTc;       // P28 R0x24 — attack time constant
    int32_t releaseTc;      // P28 R0x28 — release time constant
    int32_t releaseHoldCount; // P28 R0x2C — samples to hold before releasing
    int32_t releaseHyst;    // P28 R0x30 — release hysteresis
    int32_t invRatio;       // P28 R0x34 — inverse ratio (1/N)
    int32_t inflectionPt;   // P28 R0x38 — knee inflection point

    // Datasheet POR defaults (§8.2.13). Verified safe.
    static constexpr DrcCoeffs chipDefault() {
        return DrcCoeffs{
            (int32_t)0x00006000,  // maxGain
            (int32_t)0xFFFF8200,  // minGain
            (int32_t)0x67ED87BB,  // attackTc
            (int32_t)0x7EAC7034,  // releaseTc
            (int32_t)0x000004B0,  // releaseHoldCount
            (int32_t)0x00000C00,  // releaseHyst
            (int32_t)0xF8000000,  // invRatio
            (int32_t)0xFFFFA000,  // inflectionPt
        };
    }
};

struct LimiterCoeffs {
    int32_t attackCoeff;    // P25 R0x60
    int32_t releaseCoeff;   // P25 R0x64
    int32_t envDecay;       // P25 R0x68
    int32_t thresholdMax;   // P25 R0x6C
    int32_t thresholdMin;   // P25 R0x70
    int32_t inflectionPt;   // P25 R0x74
    int32_t slope;          // P25 R0x78
    int32_t resetCounter;   // P25 R0x7C

    // Datasheet POR defaults (§8.2.10). Verified safe.
    static constexpr LimiterCoeffs chipDefault() {
        return LimiterCoeffs{
            (int32_t)0x78D6FC9F,  // attackCoeff
            (int32_t)0x40BDB7C0,  // releaseCoeff
            (int32_t)0x7FFC3A48,  // envDecay
            (int32_t)0x01699C10,  // thresholdMax
            (int32_t)0x007259DB,  // thresholdMin
            (int32_t)0x0000199A,  // inflectionPt
            (int32_t)0x10000000,  // slope
            (int32_t)0x00000960,  // resetCounter
        };
    }
};

enum class LimiterInputSel : uint8_t {
    Max,   // max(ch0, ch1) — most defensive, default
    Avg,   // avg(ch0, ch1) — gentler, follows averaged level
};

// -----------------------------------------------------------------------------
// TAC5212 — main class
// -----------------------------------------------------------------------------

class TAC5212 {
public:
    // Forward declarations for per-subsystem accessors
    class Adc;
    class Out;
    class Pdm;

    explicit TAC5212(TwoWire &wire = Wire);

    // --- Lifecycle ------------------------------------------------------------

    // Probe the device at i2cAddr (default 0x51), perform a software reset,
    // and wake the chip. Returns Ok on successful probe + reset, Error if the
    // I2C transaction NACKs. Does NOT apply any board-specific configuration
    // — the caller is responsible for subsequent setSerialFormat / slot map
    // / channel enable / power setters.
    Result   begin(uint8_t i2cAddr = 0x51);

    // Software reset + wake. Leaves the chip in POR-default state (all
    // channels off, outputs in differential line mode, ADCs in differential
    // input mode, VREF at 2.75 V, MICBIAS off). Any caller that wants a
    // working audio path must re-apply its own configuration afterward.
    Result   reset();

    // Sleep / wake via DEV_MISC_CFG[0] SLEEP_ENZ.
    Result   wake(bool awake);

    uint8_t  address()     const { return _addr;   }
    uint16_t errorCount()  const { return _errors; }

    // --- Info / Status --------------------------------------------------------

    DeviceInfo info() const;
    Status     readStatus();

    // Write a human-readable status dump to `out`. Useful for debug / serial
    // monitor. Reads all relevant status registers and decodes them.
    void       dumpStatus(Print &out);

    // --- Chip-global MICBIAS / VREF (share register 0x4D) --------------------
    //
    // Rule B: these are chip-global, not per-channel. They live directly on
    // TAC5212, not on the Adc accessor.
    //
    // VREF_FSCALE, MICBIAS_VAL, and EN_MBIAS_GPIO all share VREF_MICBIAS_CFG
    // (0x4D). Any setter in this group reads the register, updates the target
    // field, validates the resulting combination against datasheet Table 7-16,
    // and only writes if the combo is legal.
    //
    // setMicbiasEnable additionally reads EN_MBIAS_GPIO on return and surfaces
    // a Warning if GPIO override is active, because in that case the I2C
    // MICBIAS_PDZ bit is silently shadowed by the pin state.

    Result setMicbiasEnable(bool on);
    Result setMicbiasLevel(MicbiasLevel);
    Result setVrefFscale(VrefFscale);

    // --- Chip-global ADC DSP -------------------------------------------------
    //
    // ADC high-pass filter is a chip-global setting in the TAC5212's DSP_CFG0
    // register (bits[5:4]), applied to BOTH ADC channels simultaneously. It
    // lives here on the TAC5212 class rather than on AdcChannel, per Rule B
    // (no cross-channel side effects in a per-channel leaf).
    //
    //   setAdcHpf(false) -> programmable all-pass mode, HPF effectively off
    //   setAdcHpf(true)  -> 12 Hz cutoff, the typical audio rumble-removal
    //                       setting
    //
    // If finer control is needed later (1 Hz / 12 Hz / 96 Hz / programmable
    // IIR), add a second method setAdcHpfCutoff(AdcHpfCutoff) and a matching
    // enum — but v1 keeps it boolean to match the OSC leaf `/codec/tac5212/
    // adc/hpf i 0|1`.
    Result setAdcHpf(bool on);

    // --- Chip-global DAC DSP -------------------------------------------------
    //
    // DSP_CFG1 (page 0, 0x73) packs interpolation filter, HPF mode, biquad
    // count, soft-step disable, and DVOL gang in one byte. All read-modifywrite to preserve unrelated fields.
    //
    //   biquadsPerChannel: 0..3. Hardware allocates the same count to every
    //   active DAC channel. Setting > what was set before requires the new
    //   bands' coefficients to already be programmed (or they get the POR
    //   bypass coefs by default, which is fine).

    Result setDacInterpolationFilter(InterpFilter);
    Result getDacInterpolationFilter(InterpFilter &out);
    Result setDacHpf(DacHpf);
    Result getDacHpf(DacHpf &out);
    Result setDacBiquadsPerChannel(uint8_t n);
    Result getDacBiquadsPerChannel(uint8_t &n);
    Result setDacDvolGang(bool ganged);
    Result setDacSoftStep(bool enabled);  // true = enabled (default)

    // Page-1 MISC_CFG0 bit 1 (DSP_AVDD_SEL) — required HIGH before any
    // DSP-resident block (limiter, BOP, DRC) is enabled. Datasheet POR
    // leaves the bit at "Reserved" (0); enabling a DSP block in that
    // state produces a high-pitched squeal until the bit is set. Call
    // once at boot, before setDacLimiterEnable / setDacDrcEnable.
    Result setDspAvddSelect(bool on);

    // --- DAC dynamics: distortion limiter + DRC (chip-global) ----------------
    //
    // Limiter and DRC are pre-DAC dynamics processors. The chip ships with
    // sensible POR coefficients; load them with `LimiterCoeffs::chipDefault()`
    // / `DrcCoeffs::chipDefault()` and just toggle the enable bits.
    //
    // DRC is per-channel (4 enable bits in P1.AGC_DRC_CFG). For ear-fatigue
    // applications, treat it as an all-channels-or-nothing toggle — the
    // coefficient block is shared across channels regardless.

    Result setDacLimiterCoeffs(const LimiterCoeffs&);
    Result setDacLimiterEnable(bool on);
    Result setDacLimiterInputSel(LimiterInputSel sel);

    Result setDacDrcCoeffs(const DrcCoeffs&);
    // Enable DRC on all 4 DAC sub-channels at once (CH1A/1B/2A/2B).
    Result setDacDrcEnable(bool on);

    // --- Per-channel accessors ------------------------------------------------
    //
    // `adc(1)` / `adc(2)` return a lightweight value-type handle that holds a
    // pointer back to the codec plus the channel index. Method calls on the
    // handle delegate to the parent TAC5212 with the channel baked in. This
    // mirrors the OSC path shape `/codec/tac5212/adc/N/...`.

    Adc adc(uint8_t n);
    Out out(uint8_t n);
    Pdm pdm();

    // --- Raw register access (escape hatch) ----------------------------------
    //
    // `/codec/tac5212/reg/set` and `/codec/tac5212/reg/get` map directly to
    // these. The library handles page selection transparently.

    Result  writeRegister(uint8_t page, uint8_t reg, uint8_t value);
    uint8_t readRegister(uint8_t page, uint8_t reg);

    // Burst write — writes `n` consecutive bytes starting at (page, startReg).
    // Handles the auto-increment-across-page boundary case (after reg 0x7F
    // the next byte lands at 0x08 of the next page). Used internally by the
    // biquad coefficient writer to ship 20 bytes per band in one transaction.
    Result writeBurst(uint8_t page, uint8_t startReg, const uint8_t *bytes, size_t n);

    // --- Boot-time bulk setup (NOT on OSC tree) ------------------------------
    //
    // These methods configure things that are set once at boot and are not
    // hot-swappable: audio serial interface format, slot map, channel enable
    // masks, ADC/DAC power-up. The project's setupCodec() (or a future
    // Tac5212Panel boot handler) calls these after `begin()`.

    enum class Format   : uint8_t { Tdm, I2s, LeftJustified };
    enum class WordLen  : uint8_t { Bits16, Bits20, Bits24, Bits32 };
    enum class Polarity : uint8_t { Normal, Inverted };

    struct SerialFormat {
        Format   format         = Format::Tdm;
        WordLen  wordLen        = WordLen::Bits32;
        Polarity fsyncPol       = Polarity::Normal;
        Polarity bclkPol        = Polarity::Inverted;
        bool     busErrRecover  = true;
    };

    // Configure the audio serial interface end-to-end:
    //   * PASI_CFG0 from the SerialFormat fields (format / word length /
    //     FSYNC + BCLK polarity / bus-error recovery)
    //   * INTF_CFG1 to route the DOUT pin from PASI TX (active-low /
    //     weak-high drive)
    //   * INTF_CFG2 to enable the PASI DIN receiver
    //
    // The latter two are POR-disabled and would silently kill audio if
    // left untouched, so they're folded into this single call rather
    // than exposed as separate setters. Slot map, slot offsets, and
    // channel enable / power-up are still the caller's responsibility.
    Result setSerialFormat(const SerialFormat&);
    Result setRxChannelSlot(uint8_t rxCh, uint8_t slot, bool enable = true);
    Result setTxChannelSlot(uint8_t txCh, uint8_t slot, bool enable = true);
    Result setRxSlotOffset(uint8_t bclks);
    Result setTxSlotOffset(uint8_t bclks);

    Result setChannelEnable(uint8_t inMask, uint8_t outMask);
    Result powerAdc(bool on);
    Result powerDac(bool on);

private:
    TwoWire *_wire;
    uint8_t  _addr    = 0x51;
    uint8_t  _curPage = 0;
    uint16_t _errors  = 0;

    // Page management + I2C primitives. `_write` / `_read` accept a page
    // argument and call `_selectPage` transparently (which is a no-op when
    // already on the target page). `_rmw` reads the register, masks out the
    // target field, ORs in the new value, and writes it back.
    Result   _selectPage(uint8_t page);
    Result   _write(uint8_t page, uint8_t reg, uint8_t value);
    Result   _rmw(uint8_t page, uint8_t reg, uint8_t mask, uint8_t value);
    uint8_t  _read(uint8_t page, uint8_t reg);

    // Shared-register helpers for VREF_MICBIAS_CFG (0x4D).
    Result   _readVrefMicbiasCfg(uint8_t *out);
    Result   _writeVrefMicbiasValidated(uint8_t newValue);
    static Result _validateVrefMicbiasCombo(uint8_t regValue);

    // ADC combo validation for (fullscale, bw, vref/fscale) regime.
    Result   _validateAdcCfg0(uint8_t ch, uint8_t newCfg0);
};

// -----------------------------------------------------------------------------
// TAC5212::Adc — per-ADC-channel accessor (N = 1..2)
// -----------------------------------------------------------------------------

class TAC5212::Adc {
public:
    Result setMode(AdcMode);
    Result setImpedance(AdcImpedance);
    Result setFullscale(AdcFullscale);
    Result setCoupling(AdcCoupling);
    Result setBw(AdcBw);

    // ADC digital volume (DVOL). Per-channel, 0.5 dB steps.
    //   dB range: -100.0 to +27.0  (values below -100 are treated as mute)
    //   POR default: 0.0 dB (unity)
    Result setDvol(float dB);
    Result getDvol(float &dB);

private:
    friend class TAC5212;
    TAC5212 *_codec = nullptr;
    uint8_t  _n     = 0;
    Adc(TAC5212 *c, uint8_t n) : _codec(c), _n(n) {}
};

// -----------------------------------------------------------------------------
// TAC5212::Out — per-output accessor (N = 1..2)
// -----------------------------------------------------------------------------

class TAC5212::Out {
public:
    Result setMode(OutMode);
    // Read-back: returns the OutMode currently programmed in the chip's
    // CFG0 (SRC|ROUTE) and CFG1 (DRIVE) registers. If the chip has been
    // poked into a register combination this driver doesn't recognize
    // (raw /reg/set, partial config, etc.) the result is Error and the
    // OutMode out-param is left at DiffLine. Otherwise Ok and the
    // out-param holds the decoded value. Used by /snapshot to populate
    // a freshly-connected client's codec panel without a write round-trip.
    Result getMode(OutMode &out);

    // Per-DAC digital volume. Range -100.0 .. +27.0 dB in 0.5 dB steps
    // (POR default 0.0 dB). Values below -100 dB are interpreted as mute.
    //
    // Differential output mode (DiffLine, FdReceiver) writes the same
    // volume to both sub-channels (1A+1B for output 1, 2A+2B for output 2)
    // so that DC offset on the output remains zero. Mono / pseudo-diff
    // modes write only the active sub-channel.
    Result setDvol(float dB);
    Result getDvol(float &dB);

    // Per-output biquad coefficient programming. `idx` is 1..3 (the chip
    // exposes up to 3 biquads per channel via DSP_CFG1[3:2]). Coefficients
    // are written MSB-first as 5 × 4 bytes burst, taking advantage of the
    // codec's auto-increment register addressing.
    //
    // The (channel, idx) pair maps to a global biquad slot 1..12 per the
    // hardware allocation in datasheet Table 7-48:
    //   With BQ_CFG = 1: BQ_n = ch_n
    //   With BQ_CFG = 2: BQ_n = ch_n,            BQ_(n+4) = ch_n
    //   With BQ_CFG = 3: BQ_n = ch_n, BQ_(n+4) = ch_n, BQ_(n+8) = ch_n
    //
    // Caller is responsible for ensuring DSP_CFG1[3:2] (set via
    // `setDacBiquadsPerChannel`) is large enough for the requested idx.
    // Setting a band that's beyond the current allocation succeeds at the
    // I²C level (writes the coefficients) but the chip ignores them.
    Result setBiquad(uint8_t idx, const BiquadCoeffs &coeffs);
    Result clearBiquad(uint8_t idx);   // writes BiquadCoeffs::bypass()
    Result getBiquad(uint8_t idx, BiquadCoeffs &out);

private:
    friend class TAC5212;
    TAC5212 *_codec = nullptr;
    uint8_t  _n     = 0;
    Out(TAC5212 *c, uint8_t n) : _codec(c), _n(n) {}
};

// -----------------------------------------------------------------------------
// TAC5212::Pdm — PDM microphone subsystem (chip-scoped, not per-channel)
// -----------------------------------------------------------------------------

class TAC5212::Pdm {
public:
    Result setEnable(bool on);
    Result setSource(PdmSource);
    Result setClkPin(PdmClkPin);

private:
    friend class TAC5212;
    TAC5212 *_codec = nullptr;
    explicit Pdm(TAC5212 *c) : _codec(c) {}
};

}  // namespace tac5212
