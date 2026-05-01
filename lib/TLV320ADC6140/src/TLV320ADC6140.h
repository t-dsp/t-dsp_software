// TLV320ADC6140 — register-level driver for the TI TLV320ADC6140 quad-channel
// audio ADC. Mirrors the pattern of lib/TAC5212 (Result type, per-channel
// accessor, namespace, -fno-rtti/exceptions friendly).
//
// The library exposes chip-specific primitives only. Mixer-level concerns
// (fader, mute, metering, routing) belong to TDspMixer. The project's
// Adc6140Panel subclass of tdsp::CodecPanel translates the OSC subtree
// /codec/adc6140/... into calls here.
//
// Hardware context (this board):
//   - Slave mode. Teensy drives BCLK + FSYNC + SHDNZ.
//   - Shared I²C bus with TAC5212 (chip at 0x4C, TAC5212 at 0x51).
//   - Shared TDM data line — TAC5212 owns slots 0-3, ADC6140 owns slots
//     4-7. TX_FILL must be Hi-Z so we don't fight TAC5212 on its slots.
//   - Same ASI framing as TAC5212: TDM, 32-bit, BCLK inverted.

#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <stdint.h>

#include "TLV320ADC6140_Registers.h"  // for kDefaultI2cAddr used in begin() default

namespace tlv320adc6140 {

// Result — same shape as tac5212::Result. Every setter returns Result so
// callers can distinguish Ok / Warning / Error without exceptions.
enum class ResultStatus : uint8_t { Ok = 0, Warning = 1, Error = 2 };

struct Result {
    ResultStatus status;
    const char  *message;

    bool isOk()      const { return status == ResultStatus::Ok; }
    bool isWarning() const { return status == ResultStatus::Warning; }
    bool isError()   const { return status == ResultStatus::Error; }

    static Result ok()                     { return {ResultStatus::Ok, nullptr}; }
    static Result warning(const char *msg) { return {ResultStatus::Warning, msg}; }
    static Result error(const char *msg)   { return {ResultStatus::Error, msg}; }
};

// --- Enumerations (1:1 with the OSC leaf enum strings) ---------------------

enum class Format   : uint8_t { Tdm, I2s, LeftJustified };
enum class WordLen  : uint8_t { Bits16, Bits20, Bits24, Bits32 };
enum class Polarity : uint8_t { Normal, Inverted };

enum class InputType : uint8_t {
    Microphone,   // CH_INTYP=0 — default, biases analog front-end for mic levels
    Line,         // CH_INTYP=1 — line-in (higher headroom assumption)
};

enum class InputSource : uint8_t {
    Differential,   // INxP/INxM as balanced pair — correct for XLR dynamic mics
    SingleEnded,    // INxP only, INxM grounded on the board
    PdmDigital,     // digital PDM via GPIx + PDMCLK
};

enum class Coupling : uint8_t {
    Ac,   // external AC-coupling cap on INxP/INxM (typical for mic/line)
    Dc,   // DC-coupled input (skip AC cap). Some features disabled in DC mode.
};

enum class Impedance : uint8_t {
    K2_5,  // 2.5 kΩ — lowest noise. Marginal for dynamic mic loading.
    K10,   // 10 kΩ — standard for dynamic mic inputs (SM58, etc.).
    K20,   // 20 kΩ — high-Z for low-output sources.
};

enum class MicBias : uint8_t {
    Off,           // MICBIAS power-down. Correct for dynamic mics.
    Vref,          // MBIAS_VAL=000 — output = current VREF (default 2.75 V)
    VrefBoosted,   // MBIAS_VAL=001 — output = VREF × 1.096
    Avdd,          // MBIAS_VAL=110 — output = AVDD
};

enum class FullScale : uint8_t {
    V2Rms275,   // VREF=2.75 V, 2 Vrms diff FS. Default. Requires AVDD ≥ 3.0 V.
    V1Rms8250,  // VREF=2.5 V, 1.818 Vrms diff FS.
    V1Rms1375,  // VREF=1.375 V, 1 Vrms diff FS. For AVDD=1.8 V operation.
};

enum class DecimationFilter : uint8_t {
    LinearPhase,      // default — flat group delay, best phase
    LowLatency,       // ~7 samples group delay, slight phase dev
    UltraLowLatency,  // ~4 samples group delay, wider phase dev
};

enum class ChannelSumMode : uint8_t {
    Off,                   // default — each channel is independent
    Pairs,                 // (ch1+ch2)/2 → out1 & out2, etc. +3 dB SNR
    Quad,                  // (ch1+ch2+ch3+ch4)/4 → out1..4. +6 dB SNR
};

enum class HpfCutoff : uint8_t {
    Programmable,  // HPF_SEL=00, use first-order IIR coeffs in P4_R72..R83
    Cutoff12Hz,    // 0.00025 × fs (= 12 Hz at 48 kHz) — default, rumble removal
    Cutoff96Hz,    // 0.002  × fs (= 96 Hz at 48 kHz)
    Cutoff384Hz,   // 0.008  × fs (= 384 Hz at 48 kHz) — aggressive rumble cut
};

enum class DreAgcMode : uint8_t {
    Dre,  // DRE selected for channels that have CHx_DREEN=1
    Agc,  // AGC selected for same channels
};

// --- Status snapshot -------------------------------------------------------

struct DeviceInfo {
    const char *model;     // always "TLV320ADC6140"
    uint8_t     i2cAddr;   // e.g. 0x4C
};

struct Status {
    uint8_t devSts0;       // per-channel power status raw (bit 7 = ch1)
    uint8_t devSts1;       // mode status raw (bits[7:5])
    uint8_t asiSts;        // detected FSYNC/BCLK ratio (ASI_STS)
    bool    ch1Powered;
    bool    ch2Powered;
    bool    ch3Powered;
    bool    ch4Powered;
    bool    active;        // MODE_STS == 6 or 7
    bool    recording;     // MODE_STS == 7
    bool    sleepMode;     // MODE_STS == 4
    bool    asiClockValid; // ASI_STS fields both != 0xF
};

// --- Main class ------------------------------------------------------------

class TLV320ADC6140 {
public:
    class Channel;

    explicit TLV320ADC6140(TwoWire &wire = Wire);

    // --- Lifecycle --------------------------------------------------------

    // Probe + soft-reset + wake. Returns Ok if the chip ACKs at `addr`.
    // After begin() the chip is in POR-default state (all channels off,
    // MICBIAS off, VREF=2.75 V, TDM/32-bit, slave, slots 0..7 default,
    // TX_FILL=0 which is WRONG for shared buses — call setSerialFormat()
    // next).
    Result  begin(uint8_t addr = kDefaultI2cAddr);
    Result  reset();
    Result  wake(bool awake);

    uint8_t address()    const { return _addr; }
    uint16_t errorCount() const { return _errors; }

    // --- Info / status ---------------------------------------------------

    DeviceInfo info() const;
    Status     readStatus();
    void       dumpStatus(Print &out);

    // --- Audio Serial Interface ------------------------------------------

    struct SerialFormat {
        Format   format    = Format::Tdm;
        WordLen  wordLen   = WordLen::Bits32;
        Polarity fsyncPol  = Polarity::Normal;
        Polarity bclkPol   = Polarity::Inverted;
        bool     txFillHiZ = true;   // CRITICAL on shared TDM bus
        uint8_t  txOffset  = 0;      // BCLK cycles from FSYNC edge to slot 0
    };

    Result setSerialFormat(const SerialFormat&);

    // Assign a TDM slot to a channel. `ch` is 1..4 (analog). The chip
    // supports 1..8 but only 1..4 are wired to real analog front-ends
    // on this board.
    Result setChannelSlot(uint8_t ch, uint8_t slot);

    // --- Reference & bias -------------------------------------------------

    Result setFullScale(FullScale);
    Result setMicBias(MicBias);

    // --- Signal chain (chip-global) --------------------------------------

    Result setDecimationFilter(DecimationFilter);
    Result setChannelSumMode(ChannelSumMode);
    Result setHpf(HpfCutoff);
    Result setDreAgcMode(DreAgcMode);

    Result setDreLevel(int8_t db);      // -12 / -18 / -24 / ... / -54 / -60 / -66
    Result setDreMaxGain(uint8_t db);   // 2 / 4 / 6 / ... / 24 / 26 / 28 / 30

    Result setAgcTargetLevel(int8_t db); // -6 / -8 / ... / -34 / -36
    Result setAgcMaxGain(uint8_t db);    // 3 / 6 / 9 / ... / 42

    Result setDvolGang(bool gang);
    Result setSoftStep(bool enabled);
    Result setBiquadsPerChannel(uint8_t n);  // 0..3

    // --- Per-channel accessor --------------------------------------------

    Channel channel(uint8_t n);

    // --- Power / enable ---------------------------------------------------

    // inMask / outMask bits: 0x80 = ch1, 0x40 = ch2, 0x20 = ch3, 0x10 = ch4
    Result setChannelEnable(uint8_t inMask, uint8_t outMask);

    // Power up / down the ADC core, MICBIAS, and PLL as a group.
    Result powerUp(bool adc, bool micbias, bool pll);

    // --- Raw register access (escape hatch) ------------------------------

    Result  writeRegister(uint8_t page, uint8_t reg, uint8_t value);
    uint8_t readRegister (uint8_t page, uint8_t reg);

private:
    TwoWire *_wire;
    uint8_t  _addr    = kDefaultI2cAddr;
    uint8_t  _curPage = 0;
    uint16_t _errors  = 0;

    // Cache the last-written BIAS_CFG value so setMicBias() can preserve
    // ADC_FSCALE and setFullScale() can preserve MBIAS_VAL without an
    // I²C round-trip. Seeded from reset().
    uint8_t  _biasCfgCache = 0x00;

    // Cache DSP_CFG0/1 for the same reason (HPF, biquads, DRE vs AGC).
    uint8_t  _dspCfg0Cache = 0x01;  // POR default: HPF=12 Hz, CH_SUM=off
    uint8_t  _dspCfg1Cache = 0x40;  // POR default: 2 biquads per channel

    Result   _selectPage(uint8_t page);
    Result   _write(uint8_t page, uint8_t reg, uint8_t value);
    Result   _rmw(uint8_t page, uint8_t reg, uint8_t mask, uint8_t value);
    uint8_t  _read(uint8_t page, uint8_t reg);

    // Re-seed the cached POR defaults. Called from reset() and begin().
    void     _dspCfgCache_init();
};

// --- Per-channel accessor (N = 1..4) --------------------------------------

class TLV320ADC6140::Channel {
public:
    // CFG0: front-end configuration
    Result setType(InputType);
    Result setSource(InputSource);
    Result setCoupling(Coupling);
    Result setImpedance(Impedance);
    Result setDreEnable(bool on);

    // Analog PGA gain, 0..42 dB in 1 dB steps.
    Result setGainDb(uint8_t db);

    // Digital volume, -100..+27 dB in 0.5 dB steps. Below -100 is mute.
    // Unity = 0.0 dB.
    Result setDvolDb(float db);

    // Gain trim ±0.7 dB in 0.1 dB steps (for inter-channel matching).
    Result setGainCalDb(float db);

    // Phase cal, 0..255 modulator clock cycles (~163 ns each at 48 kHz fs).
    Result setPhaseCalCycles(uint8_t cycles);

private:
    friend class TLV320ADC6140;
    TLV320ADC6140 *_codec = nullptr;
    uint8_t        _n     = 0;
    Channel(TLV320ADC6140 *c, uint8_t n) : _codec(c), _n(n) {}
    uint8_t _cfgReg(uint8_t offset) const;  // 0x3C + (n-1)*5 + offset
};

}  // namespace tlv320adc6140
