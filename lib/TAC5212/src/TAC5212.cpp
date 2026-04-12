// TAC5212 — implementation.
//
// See TAC5212.h for the rules this library honors. Values verified against
// TI datasheet SLASF23A (December 2023, revised January 2025).

#include "TAC5212.h"
#include "TAC5212_Registers.h"

namespace tac5212 {

namespace {

// Map enum values to hardware register-field values. These are small so they
// live as anonymous-namespace helpers rather than in the public header.

constexpr uint8_t adcModeToInsrc(AdcMode m) {
    switch (m) {
        case AdcMode::Differential:    return reg::adc_cfg0::INSRC_DIFFERENTIAL;
        case AdcMode::SingleEndedInp:  return reg::adc_cfg0::INSRC_SE_INP;
        case AdcMode::SingleEndedInm:  return reg::adc_cfg0::INSRC_SE_MUX_INM;
    }
    return reg::adc_cfg0::INSRC_DIFFERENTIAL;
}

constexpr uint8_t adcImpToField(AdcImpedance i) {
    switch (i) {
        case AdcImpedance::K5:  return reg::adc_cfg0::IMP_5K;
        case AdcImpedance::K10: return reg::adc_cfg0::IMP_10K;
        case AdcImpedance::K40: return reg::adc_cfg0::IMP_40K;
    }
    return reg::adc_cfg0::IMP_5K;
}

constexpr uint8_t adcCouplingToField(AdcCoupling c) {
    switch (c) {
        case AdcCoupling::Ac:           return reg::adc_cfg0::CM_TOL_AC;
        case AdcCoupling::DcLow:        return reg::adc_cfg0::CM_TOL_DC_LOW;
        case AdcCoupling::DcRailToRail: return reg::adc_cfg0::CM_TOL_DC_RAIL_TO_RAIL;
    }
    return reg::adc_cfg0::CM_TOL_AC;
}

constexpr uint8_t adcFullscaleToField(AdcFullscale f) {
    return (f == AdcFullscale::V4rms)
        ? reg::adc_cfg0::FULLSCALE_4VRMS
        : reg::adc_cfg0::FULLSCALE_2VRMS;
}

constexpr uint8_t adcBwToField(AdcBw b) {
    return (b == AdcBw::Wide96k)
        ? reg::adc_cfg0::BW_96K_WIDE
        : reg::adc_cfg0::BW_24K_AUDIO;
}

// Output mode → a full byte value for CFG0, CFG1, CFG2 respectively.
// Each OutMode picks a specific (SRC, ROUTE, DRIVE) triple and we also set
// the LVL_CTRL field to the POR default (100b = 0 dB) on CFG1/CFG2 because
// any other value is "Reserved; Don't use" outside analog bypass.
struct OutModeBytes { uint8_t cfg0; uint8_t cfg1; uint8_t cfg2; };

constexpr OutModeBytes outModeToBytes(OutMode m) {
    using namespace reg::out_cfg0;
    using namespace reg::out_cfg1;
    constexpr uint8_t SAFE_LVL = LVL_0DB_SAFE;

    switch (m) {
        case OutMode::DiffLine:
            return {
                static_cast<uint8_t>(SRC_DAC | ROUTE_DIFF),          // 0x20
                static_cast<uint8_t>(DRIVE_LINE | SAFE_LVL),          // 0x20
                static_cast<uint8_t>(DRIVE_LINE | SAFE_LVL),          // 0x20
            };
        case OutMode::SeLine:
            return {
                static_cast<uint8_t>(SRC_DAC | ROUTE_MONO_SE_P),     // 0x28
                static_cast<uint8_t>(DRIVE_LINE | SAFE_LVL),
                static_cast<uint8_t>(DRIVE_LINE | SAFE_LVL),
            };
        case OutMode::HpDriver:
            return {
                static_cast<uint8_t>(SRC_DAC | ROUTE_MONO_SE_P),     // 0x28
                static_cast<uint8_t>(DRIVE_HEADPHONE | SAFE_LVL),     // 0x60
                static_cast<uint8_t>(DRIVE_HEADPHONE | SAFE_LVL),     // 0x60
            };
        case OutMode::FdReceiver:
            return {
                static_cast<uint8_t>(SRC_DAC | ROUTE_DIFF),
                static_cast<uint8_t>(DRIVE_FD_RECEIVER | SAFE_LVL),
                static_cast<uint8_t>(DRIVE_FD_RECEIVER | SAFE_LVL),
            };
    }
    // Unreachable, but keep the compiler happy.
    return { 0, 0, 0 };
}

// Return the (CFG0, CFG1, CFG2) addresses for OUT N.
struct OutRegs { uint8_t cfg0; uint8_t cfg1; uint8_t cfg2; };
constexpr OutRegs outRegs(uint8_t n) {
    return (n == 2)
        ? OutRegs{reg::OUT2_CFG0, reg::OUT2_CFG1, reg::OUT2_CFG2}
        : OutRegs{reg::OUT1_CFG0, reg::OUT1_CFG1, reg::OUT1_CFG2};
}

// Return the CFG0 address for ADC channel N.
constexpr uint8_t adcCfg0Addr(uint8_t n) {
    return (n == 2) ? reg::ADC_CH2_CFG0 : reg::ADC_CH1_CFG0;
}

}  // anonymous namespace

// -----------------------------------------------------------------------------
// Constructor / lifecycle
// -----------------------------------------------------------------------------

TAC5212::TAC5212(TwoWire &wire) : _wire(&wire) {}

Result TAC5212::begin(uint8_t i2cAddr) {
    _addr    = i2cAddr;
    _curPage = 0;
    _errors  = 0;

    // Probe the bus. A zero-length transmission returns 0 if the device
    // ACKs its address, non-zero otherwise.
    _wire->beginTransmission(_addr);
    if (_wire->endTransmission() != 0) {
        ++_errors;
        return Result::error("TAC5212 not found on I2C bus");
    }

    return reset();
}

Result TAC5212::reset() {
    // Write the self-clearing reset bit on page 0.
    Result r = _write(0, reg::SW_RESET, reg::sw_reset::MASK_TRIGGER);
    if (r.isError()) return r;

    // Datasheet: wait at least 10 ms after SW reset before further writes.
    delay(15);

    // Re-select page 0 — the chip may have reset its page pointer, which
    // our cached _curPage wouldn't know about. Force a page write.
    _curPage = 0xFF;
    r = _selectPage(0);
    if (r.isError()) return r;

    // Wake. Writes the conservative default to DEV_MISC_CFG (SLEEP_ENZ = 1,
    // SLEEP_EXIT_VREF_EN = 1). This mirrors the value the existing
    // setupCodec() flow has been using successfully.
    return _write(0, reg::DEV_MISC_CFG, reg::dev_misc_cfg::VAL_WAKE);
}

Result TAC5212::wake(bool awake) {
    if (awake) {
        return _rmw(0, reg::DEV_MISC_CFG,
                    reg::dev_misc_cfg::MASK_SLEEP_ENZ,
                    reg::dev_misc_cfg::MASK_SLEEP_ENZ);
    }
    return _rmw(0, reg::DEV_MISC_CFG,
                reg::dev_misc_cfg::MASK_SLEEP_ENZ,
                0);
}

// -----------------------------------------------------------------------------
// Info / Status
// -----------------------------------------------------------------------------

DeviceInfo TAC5212::info() const {
    return { "TAC5212", _addr, _curPage };
}

Status TAC5212::readStatus() {
    Status s = {};
    s.devSts0 = _read(0, reg::DEV_STS0);
    s.devSts1 = _read(0, reg::DEV_STS1);
    s.pllLocked    = (s.devSts1 & reg::dev_sts1::MASK_PLL_LOCKED)  != 0;
    s.micBiasActive = (s.devSts1 & reg::dev_sts1::MASK_MICBIAS_STS) != 0;
    s.faultActive  = (s.devSts1 & reg::dev_sts1::MASK_FAULT_ACTIVE) != 0;

    uint8_t vrefMicbias = _read(0, reg::VREF_MICBIAS_CFG);
    s.micBiasGpioOverride =
        (vrefMicbias & reg::vref_micbias_cfg::MASK_EN_MBIAS_GPIO) != 0;

    return s;
}

void TAC5212::dumpStatus(Print &out) {
    Status s = readStatus();
    out.print(F("TAC5212 @0x"));   out.println(_addr, HEX);
    out.print(F("  DEV_STS0: 0x")); out.println(s.devSts0, HEX);
    out.print(F("  DEV_STS1: 0x")); out.println(s.devSts1, HEX);
    out.print(F("  PLL locked:   ")); out.println(s.pllLocked ? F("yes") : F("no"));
    out.print(F("  MICBIAS on:   ")); out.println(s.micBiasActive ? F("yes") : F("no"));
    out.print(F("  Fault active: ")); out.println(s.faultActive ? F("yes") : F("no"));
    out.print(F("  MICBIAS GPIO override: "));
    out.println(s.micBiasGpioOverride ? F("YES (I2C shadowed)") : F("no"));
    out.print(F("  I2C error count: ")); out.println(_errors);
}

// -----------------------------------------------------------------------------
// Chip-global MICBIAS / VREF (shared register 0x4D)
// -----------------------------------------------------------------------------

Result TAC5212::_readVrefMicbiasCfg(uint8_t *out) {
    *out = _read(0, reg::VREF_MICBIAS_CFG);
    return Result::ok();
}

Result TAC5212::_validateVrefMicbiasCombo(uint8_t regValue) {
    using namespace reg::vref_micbias_cfg;
    const uint8_t micbiasVal =
        (regValue & MASK_MICBIAS_VAL) >> SHIFT_MICBIAS_VAL;   // 0..3
    const uint8_t vrefFscale =
        (regValue & MASK_VREF_FSCALE) >> SHIFT_VREF_FSCALE;   // 0..3

    // VREF_FSCALE = 11b is reserved at any MICBIAS_VAL.
    if (vrefFscale == 0x3) {
        return Result::error("VREF_FSCALE reserved value");
    }
    // MICBIAS_VAL = 10b is always reserved.
    if (micbiasVal == 0x2) {
        return Result::error("MICBIAS_VAL reserved value");
    }
    // MICBIAS_VAL = 01b (half VREF) combined with VREF_FSCALE >= 10b is
    // reserved per datasheet Table 7-16.
    if (micbiasVal == 0x1 && vrefFscale >= 0x2) {
        return Result::error("MICBIAS half-VREF invalid with VREF_FSCALE 1.375v");
    }
    // All other combos are legal.
    return Result::ok();
}

Result TAC5212::_writeVrefMicbiasValidated(uint8_t newValue) {
    Result combo = _validateVrefMicbiasCombo(newValue);
    if (combo.isError()) return combo;
    return _write(0, reg::VREF_MICBIAS_CFG, newValue);
}

Result TAC5212::setMicbiasEnable(bool on) {
    // PWR_CFG[5] is the canonical enable. We also check EN_MBIAS_GPIO on
    // return and warn if the GPIO override is active — in that case the
    // I2C bit is silently shadowed by the pin state.
    Result r = _rmw(0, reg::PWR_CFG,
                    reg::pwr_cfg::MASK_MICBIAS_PDZ,
                    on ? reg::pwr_cfg::MASK_MICBIAS_PDZ : uint8_t{0});
    if (r.isError()) return r;

    uint8_t vrefMicbias = 0;
    _readVrefMicbiasCfg(&vrefMicbias);
    if ((vrefMicbias & reg::vref_micbias_cfg::MASK_EN_MBIAS_GPIO) != 0) {
        return Result::warning(
            "MICBIAS controlled by GPIO pin; I2C value shadowed");
    }
    return Result::ok();
}

Result TAC5212::setMicbiasLevel(MicbiasLevel level) {
    uint8_t current = 0;
    _readVrefMicbiasCfg(&current);

    uint8_t field = 0;
    switch (level) {
        case MicbiasLevel::SameAsVref: field = reg::vref_micbias_cfg::MICBIAS_VAL_SAME_AS_VREF; break;
        case MicbiasLevel::HalfVref:   field = reg::vref_micbias_cfg::MICBIAS_VAL_HALF_VREF;    break;
        case MicbiasLevel::Avdd:       field = reg::vref_micbias_cfg::MICBIAS_VAL_AVDD;         break;
    }
    const uint8_t updated =
        (current & ~reg::vref_micbias_cfg::MASK_MICBIAS_VAL) | field;
    return _writeVrefMicbiasValidated(updated);
}

// Chip-global ADC HPF. Uses read-modify-write on DSP_CFG0 to preserve the
// other fields (decimation filter, biquad count, soft-step, DVOL-gang).
//
//   on == false -> HPF_SEL_PROGRAMMABLE (00b). The default programmable-IIR
//                  coefficients are all-pass, so this is effectively "HPF
//                  off — signal passes through unchanged".
//   on == true  -> HPF_SEL_12HZ (10b). 12 Hz -3 dB cutoff, the typical
//                  audio rumble-removal setting.
Result TAC5212::setAdcHpf(bool on) {
    const uint8_t value = on
        ? reg::dsp_cfg0::HPF_SEL_12HZ
        : reg::dsp_cfg0::HPF_SEL_PROGRAMMABLE;
    return _rmw(0, reg::DSP_CFG0, reg::dsp_cfg0::MASK_HPF_SEL, value);
}

Result TAC5212::setVrefFscale(VrefFscale scale) {
    uint8_t current = 0;
    _readVrefMicbiasCfg(&current);

    uint8_t field = 0;
    switch (scale) {
        case VrefFscale::V2p75:  field = reg::vref_micbias_cfg::VREF_FSCALE_2p75;  break;
        case VrefFscale::V2p5:   field = reg::vref_micbias_cfg::VREF_FSCALE_2p5;   break;
        case VrefFscale::V1p375: field = reg::vref_micbias_cfg::VREF_FSCALE_1p375; break;
    }
    const uint8_t updated =
        (current & ~reg::vref_micbias_cfg::MASK_VREF_FSCALE) | field;
    return _writeVrefMicbiasValidated(updated);
}

// -----------------------------------------------------------------------------
// Accessors
// -----------------------------------------------------------------------------

TAC5212::Adc TAC5212::adc(uint8_t n) {
    if (n < 1) n = 1;
    if (n > 2) n = 2;
    return Adc(this, n);
}

TAC5212::Out TAC5212::out(uint8_t n) {
    if (n < 1) n = 1;
    if (n > 2) n = 2;
    return Out(this, n);
}

TAC5212::Pdm TAC5212::pdm() { return Pdm(this); }

// -----------------------------------------------------------------------------
// ADC channel setters (per channel, no cross-channel side effects)
// -----------------------------------------------------------------------------

Result TAC5212::Adc::setMode(AdcMode m) {
    return _codec->_rmw(0, adcCfg0Addr(_n),
                        reg::adc_cfg0::MASK_INSRC,
                        adcModeToInsrc(m));
}

Result TAC5212::Adc::setImpedance(AdcImpedance imp) {
    // Validate: 96 kHz wide BW mode requires 40k impedance. If the user
    // is dropping to 5k/10k and BW is currently 96k, that's an invalid
    // state. We read the current CFG0, swap the IMP field, and let the
    // combo validator catch the conflict.
    const uint8_t addr = adcCfg0Addr(_n);
    const uint8_t cur  = _codec->_read(0, addr);
    const uint8_t next =
        (cur & ~reg::adc_cfg0::MASK_IMP) | adcImpToField(imp);
    Result check = _codec->_validateAdcCfg0(_n, next);
    if (check.isError()) return check;
    return _codec->_write(0, addr, next);
}

Result TAC5212::Adc::setFullscale(AdcFullscale fs) {
    const uint8_t addr = adcCfg0Addr(_n);
    const uint8_t cur  = _codec->_read(0, addr);
    const uint8_t next =
        (cur & ~reg::adc_cfg0::MASK_FULLSCALE) | adcFullscaleToField(fs);
    Result check = _codec->_validateAdcCfg0(_n, next);
    if (check.isError()) return check;
    return _codec->_write(0, addr, next);
}

Result TAC5212::Adc::setCoupling(AdcCoupling c) {
    const uint8_t addr = adcCfg0Addr(_n);
    const uint8_t cur  = _codec->_read(0, addr);
    const uint8_t next =
        (cur & ~reg::adc_cfg0::MASK_CM_TOL) | adcCouplingToField(c);
    Result check = _codec->_validateAdcCfg0(_n, next);
    if (check.isError()) return check;
    return _codec->_write(0, addr, next);
}

Result TAC5212::Adc::setBw(AdcBw bw) {
    const uint8_t addr = adcCfg0Addr(_n);
    const uint8_t cur  = _codec->_read(0, addr);
    const uint8_t next =
        (cur & ~reg::adc_cfg0::MASK_BW_MODE) | adcBwToField(bw);
    Result check = _codec->_validateAdcCfg0(_n, next);
    if (check.isError()) return check;
    return _codec->_write(0, addr, next);
}

// ADC combo validation: check the compound rules that involve more than one
// field within ADC_CHx_CFG0, plus the chip-global VREF_FSCALE dependency.
Result TAC5212::_validateAdcCfg0(uint8_t /*ch*/, uint8_t newCfg0) {
    using namespace reg::adc_cfg0;
    const uint8_t fullscale = newCfg0 & MASK_FULLSCALE;
    const uint8_t bw        = newCfg0 & MASK_BW_MODE;
    const uint8_t imp       = newCfg0 & MASK_IMP;
    const uint8_t cmTol     = newCfg0 & MASK_CM_TOL;

    // 96 kHz wide-BW mode requires 40k impedance.
    if (bw == BW_96K_WIDE && imp != IMP_40K) {
        return Result::error("ADC bw 96khz requires impedance 40k");
    }

    // 4 Vrms fullscale has three prerequisites:
    //   - high-CMRR coupling (DC_RAIL_TO_RAIL)
    //   - VREF_FSCALE at 2.75 V (chip-global — read VREF_MICBIAS_CFG)
    //   - audio 24 kHz bandwidth
    if (fullscale == FULLSCALE_4VRMS) {
        if (cmTol != CM_TOL_DC_RAIL_TO_RAIL) {
            return Result::error(
                "ADC fullscale 4vrms requires coupling dc_rail_to_rail");
        }
        if (bw != BW_24K_AUDIO) {
            return Result::error(
                "ADC fullscale 4vrms requires bw 24khz");
        }
        const uint8_t vm = _read(0, reg::VREF_MICBIAS_CFG);
        const uint8_t vrefField =
            (vm & reg::vref_micbias_cfg::MASK_VREF_FSCALE)
                >> reg::vref_micbias_cfg::SHIFT_VREF_FSCALE;
        if (vrefField != reg::vref_micbias_cfg::VREF_FSCALE_2p75) {
            return Result::error(
                "ADC fullscale 4vrms requires vref/fscale 2.75v");
        }
    }

    return Result::ok();
}

// -----------------------------------------------------------------------------
// Output mode setter (driver type only — Rule A: no gain)
// -----------------------------------------------------------------------------

Result TAC5212::Out::setMode(OutMode m) {
    const OutRegs    regs  = outRegs(_n);
    const OutModeBytes bytes = outModeToBytes(m);

    Result r = _codec->_write(0, regs.cfg0, bytes.cfg0);
    if (r.isError()) return r;
    r = _codec->_write(0, regs.cfg1, bytes.cfg1);
    if (r.isError()) return r;
    r = _codec->_write(0, regs.cfg2, bytes.cfg2);
    return r;
}

// Read CFG0 (SRC|ROUTE) and CFG1 (DRIVE) and reverse the OutMode mapping
// from outModeToBytes(). CFG2 is intentionally ignored — for every mode we
// emit, CFG1 and CFG2 carry the same DRIVE/LVL pattern, so CFG1 alone is a
// reliable discriminator. SRC must be SRC_DAC; any other source means the
// chip is in a config we never set (analog bypass / mixed) and we surface
// it as an error rather than guessing.
Result TAC5212::Out::getMode(OutMode &out) {
    out = OutMode::DiffLine;
    const OutRegs regs = outRegs(_n);
    const uint8_t cfg0 = _codec->_read(0, regs.cfg0);
    const uint8_t cfg1 = _codec->_read(0, regs.cfg1);

    const uint8_t src   = cfg0 & reg::out_cfg0::MASK_SRC;
    const uint8_t route = cfg0 & reg::out_cfg0::MASK_ROUTE;
    const uint8_t drive = cfg1 & reg::out_cfg1::MASK_DRIVE;

    if (src != reg::out_cfg0::SRC_DAC) {
        return Result::error("output not in DAC source mode");
    }

    using namespace reg::out_cfg0;
    using namespace reg::out_cfg1;

    if (route == ROUTE_DIFF && drive == DRIVE_LINE) {
        out = OutMode::DiffLine;       return Result::ok();
    }
    if (route == ROUTE_MONO_SE_P && drive == DRIVE_LINE) {
        out = OutMode::SeLine;         return Result::ok();
    }
    if (route == ROUTE_MONO_SE_P && drive == DRIVE_HEADPHONE) {
        out = OutMode::HpDriver;       return Result::ok();
    }
    if (route == ROUTE_DIFF && drive == DRIVE_FD_RECEIVER) {
        out = OutMode::FdReceiver;     return Result::ok();
    }
    return Result::error("unrecognized output route/drive combo");
}

// -----------------------------------------------------------------------------
// PDM block (stubs — to be filled in after INTF_CFG4 bitfield verification)
// -----------------------------------------------------------------------------

Result TAC5212::Pdm::setEnable(bool on) {
    // Wraps multiple registers:
    //   GPIO1_CFG    = PDM clock out (function=4, drive=1)
    //   GPI1_CFG     = GPI1 enabled as digital input
    //   INTF_CFG4    = route GPI1 → PDM channels 3+4
    //   CH_EN bits   = IN_CH3 | IN_CH4 enable
    //   PWR_CFG      = ADC_PDZ (if not already on)
    //
    // The exact INTF_CFG4 bitfield needs datasheet re-verification before
    // implementation. Flagging as a deliberate stub.
    (void)on;
    return Result::error("TAC5212::Pdm::setEnable not yet implemented");
}

Result TAC5212::Pdm::setSource(PdmSource) {
    return Result::error("TAC5212::Pdm::setSource not yet implemented");
}

Result TAC5212::Pdm::setClkPin(PdmClkPin) {
    return Result::error("TAC5212::Pdm::setClkPin not yet implemented");
}

// -----------------------------------------------------------------------------
// Raw register access (escape hatch, and what reg/set + reg/get route to)
// -----------------------------------------------------------------------------

Result TAC5212::writeRegister(uint8_t page, uint8_t reg, uint8_t value) {
    return _write(page, reg, value);
}

uint8_t TAC5212::readRegister(uint8_t page, uint8_t reg) {
    return _read(page, reg);
}

// -----------------------------------------------------------------------------
// Boot-time setup (called from setupCodec() in main.cpp; not on OSC tree)
// -----------------------------------------------------------------------------

Result TAC5212::setSerialFormat(const SerialFormat &fmt) {
    uint8_t value = 0;

    switch (fmt.format) {
        case Format::Tdm:           value |= reg::pasi_cfg0::FORMAT_TDM;  break;
        case Format::I2s:           value |= reg::pasi_cfg0::FORMAT_I2S;  break;
        case Format::LeftJustified: value |= reg::pasi_cfg0::FORMAT_LJ;   break;
    }
    switch (fmt.wordLen) {
        case WordLen::Bits16: value |= reg::pasi_cfg0::WORDLEN_16; break;
        case WordLen::Bits20: value |= reg::pasi_cfg0::WORDLEN_20; break;
        case WordLen::Bits24: value |= reg::pasi_cfg0::WORDLEN_24; break;
        case WordLen::Bits32: value |= reg::pasi_cfg0::WORDLEN_32; break;
    }
    if (fmt.fsyncPol == Polarity::Inverted) value |= reg::pasi_cfg0::MASK_FSYNC_POL;
    if (fmt.bclkPol  == Polarity::Inverted) value |= reg::pasi_cfg0::MASK_BCLK_POL;
    if (fmt.busErrRecover)                  value |= reg::pasi_cfg0::MASK_BUS_ERR_RCOV;

    return _write(0, reg::PASI_CFG0, value);
}

Result TAC5212::setRxChannelSlot(uint8_t rxCh, uint8_t slot, bool enable) {
    uint8_t addr = 0;
    switch (rxCh) {
        case 1: addr = reg::RX_CH1_SLOT; break;
        case 2: addr = reg::RX_CH2_SLOT; break;
        case 3: addr = reg::RX_CH3_SLOT; break;
        case 4: addr = reg::RX_CH4_SLOT; break;
        default: return Result::error("setRxChannelSlot: rxCh out of range");
    }
    return _write(0, addr, reg::makeSlot(slot, enable));
}

Result TAC5212::setTxChannelSlot(uint8_t txCh, uint8_t slot, bool enable) {
    uint8_t addr = 0;
    switch (txCh) {
        case 1: addr = reg::TX_CH1_SLOT; break;
        case 2: addr = reg::TX_CH2_SLOT; break;
        case 3: addr = reg::TX_CH3_SLOT; break;
        case 4: addr = reg::TX_CH4_SLOT; break;
        default: return Result::error("setTxChannelSlot: txCh out of range");
    }
    return _write(0, addr, reg::makeSlot(slot, enable));
}

Result TAC5212::setRxSlotOffset(uint8_t bclks) {
    return _write(0, reg::PASI_RX_CFG0, bclks);
}

Result TAC5212::setTxSlotOffset(uint8_t bclks) {
    return _write(0, reg::PASI_TX_CFG2, bclks);
}

Result TAC5212::setChannelEnable(uint8_t inMask, uint8_t outMask) {
    const uint8_t value = static_cast<uint8_t>((inMask << 4) | (outMask & 0x0F));
    return _write(0, reg::CH_EN, value);
}

Result TAC5212::powerAdc(bool on) {
    return _rmw(0, reg::PWR_CFG,
                reg::pwr_cfg::MASK_ADC_PDZ,
                on ? reg::pwr_cfg::MASK_ADC_PDZ : uint8_t{0});
}

Result TAC5212::powerDac(bool on) {
    return _rmw(0, reg::PWR_CFG,
                reg::pwr_cfg::MASK_DAC_PDZ,
                on ? reg::pwr_cfg::MASK_DAC_PDZ : uint8_t{0});
}

// -----------------------------------------------------------------------------
// Private I2C + page management
// -----------------------------------------------------------------------------

Result TAC5212::_selectPage(uint8_t page) {
    if (_curPage == page) return Result::ok();

    _wire->beginTransmission(_addr);
    _wire->write(reg::PAGE_SELECT);
    _wire->write(page);
    const uint8_t err = _wire->endTransmission();
    if (err != 0) {
        ++_errors;
        return Result::error("I2C NACK during page select");
    }
    _curPage = page;
    return Result::ok();
}

Result TAC5212::_write(uint8_t page, uint8_t r, uint8_t value) {
    Result pg = _selectPage(page);
    if (pg.isError()) return pg;

    _wire->beginTransmission(_addr);
    _wire->write(r);
    _wire->write(value);
    const uint8_t err = _wire->endTransmission();
    if (err != 0) {
        ++_errors;
        return Result::error("I2C NACK during register write");
    }
    return Result::ok();
}

Result TAC5212::_rmw(uint8_t page, uint8_t r, uint8_t mask, uint8_t value) {
    const uint8_t current = _read(page, r);
    const uint8_t updated = static_cast<uint8_t>((current & ~mask) | (value & mask));
    return _write(page, r, updated);
}

uint8_t TAC5212::_read(uint8_t page, uint8_t r) {
    Result pg = _selectPage(page);
    if (pg.isError()) return 0xFF;

    _wire->beginTransmission(_addr);
    _wire->write(r);
    if (_wire->endTransmission(false) != 0) {  // repeated start
        ++_errors;
        return 0xFF;
    }
    if (_wire->requestFrom(_addr, static_cast<uint8_t>(1)) != 1) {
        ++_errors;
        return 0xFF;
    }
    return _wire->available() ? static_cast<uint8_t>(_wire->read()) : uint8_t{0xFF};
}

}  // namespace tac5212
