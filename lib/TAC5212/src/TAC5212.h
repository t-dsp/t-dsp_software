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
