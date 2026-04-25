// TAC5212 — implementation.
//
// Values verified against TI datasheet SLASF23A (December 2023, revised
// January 2025).

#include "TAC5212.h"
#include "TAC5212_Registers.h"

namespace tac5212 {

namespace {

// Map enum values to hardware register-field values. These are small so they
// live as anonymous-namespace helpers rather than in the public header.

constexpr uint8_t adcModeToInsrc(AdcMode m) {
    switch (m) {
        case AdcMode::Differential:    return reg::adc_cfg0::INSRC_DIFFERENTIAL;
        case AdcMode::SingleEndedInp:       return reg::adc_cfg0::INSRC_SE_MUX_INP;
        case AdcMode::SingleEndedInpInmGnd: return reg::adc_cfg0::INSRC_SE_INP;
        case AdcMode::SingleEndedInm:       return reg::adc_cfg0::INSRC_SE_MUX_INM;
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

// Return the DVOL (CFG2) address for ADC channel N.
constexpr uint8_t adcDvolAddr(uint8_t n) {
    return (n == 2) ? reg::ADC_CH2_CFG2 : reg::ADC_CH1_CFG2;
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

Result TAC5212::Adc::setDvol(float dB) {
    if (dB > reg::adc_dvol::DB_MAX)
        return Result::error("ADC dvol exceeds +27 dB max");
    return _codec->_write(0, adcDvolAddr(_n), reg::adc_dvol::fromDb(dB));
}

Result TAC5212::Adc::getDvol(float &dB) {
    const uint8_t raw = _codec->_read(0, adcDvolAddr(_n));
    dB = reg::adc_dvol::toDb(raw);
    return Result::ok();
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
// Output mode setter
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
// Chip-global DAC DSP (DSP_CFG1, page 0, 0x73)
// -----------------------------------------------------------------------------

namespace {

constexpr uint8_t interpToField(InterpFilter f) {
    switch (f) {
        case InterpFilter::LinearPhase:     return reg::dsp_cfg1::INTX_LINEAR_PHASE;
        case InterpFilter::LowLatency:      return reg::dsp_cfg1::INTX_LOW_LATENCY;
        case InterpFilter::UltraLowLatency: return reg::dsp_cfg1::INTX_ULTRA_LOW_LATENCY;
        case InterpFilter::LowPower:        return reg::dsp_cfg1::INTX_LOW_POWER;
    }
    return reg::dsp_cfg1::INTX_LINEAR_PHASE;
}

constexpr InterpFilter fieldToInterp(uint8_t field) {
    switch (field) {
        case reg::dsp_cfg1::INTX_LINEAR_PHASE:      return InterpFilter::LinearPhase;
        case reg::dsp_cfg1::INTX_LOW_LATENCY:       return InterpFilter::LowLatency;
        case reg::dsp_cfg1::INTX_ULTRA_LOW_LATENCY: return InterpFilter::UltraLowLatency;
        case reg::dsp_cfg1::INTX_LOW_POWER:         return InterpFilter::LowPower;
    }
    return InterpFilter::LinearPhase;
}

constexpr uint8_t dacHpfToField(DacHpf h) {
    switch (h) {
        case DacHpf::Programmable: return reg::dsp_cfg1::HPF_PROGRAMMABLE;
        case DacHpf::Cut1Hz:       return reg::dsp_cfg1::HPF_1HZ;
        case DacHpf::Cut12Hz:      return reg::dsp_cfg1::HPF_12HZ;
        case DacHpf::Cut96Hz:      return reg::dsp_cfg1::HPF_96HZ;
    }
    return reg::dsp_cfg1::HPF_PROGRAMMABLE;
}

constexpr DacHpf fieldToDacHpf(uint8_t field) {
    switch (field) {
        case reg::dsp_cfg1::HPF_PROGRAMMABLE: return DacHpf::Programmable;
        case reg::dsp_cfg1::HPF_1HZ:          return DacHpf::Cut1Hz;
        case reg::dsp_cfg1::HPF_12HZ:         return DacHpf::Cut12Hz;
        case reg::dsp_cfg1::HPF_96HZ:         return DacHpf::Cut96Hz;
    }
    return DacHpf::Programmable;
}

constexpr uint8_t bqCountToField(uint8_t n) {
    switch (n) {
        case 0: return reg::dsp_cfg1::BQ_NONE;
        case 1: return reg::dsp_cfg1::BQ_1;
        case 2: return reg::dsp_cfg1::BQ_2;
        case 3: return reg::dsp_cfg1::BQ_3;
    }
    return reg::dsp_cfg1::BQ_2;  // POR default
}

constexpr uint8_t fieldToBqCount(uint8_t field) {
    switch (field) {
        case reg::dsp_cfg1::BQ_NONE: return 0;
        case reg::dsp_cfg1::BQ_1:    return 1;
        case reg::dsp_cfg1::BQ_2:    return 2;
        case reg::dsp_cfg1::BQ_3:    return 3;
    }
    return 2;
}

}  // anonymous namespace

Result TAC5212::setDacInterpolationFilter(InterpFilter f) {
    return _rmw(0, reg::DSP_CFG1, reg::dsp_cfg1::MASK_INTX_FILT, interpToField(f));
}

Result TAC5212::getDacInterpolationFilter(InterpFilter &out) {
    const uint8_t cfg = _read(0, reg::DSP_CFG1);
    out = fieldToInterp(cfg & reg::dsp_cfg1::MASK_INTX_FILT);
    return Result::ok();
}

Result TAC5212::setDacHpf(DacHpf h) {
    return _rmw(0, reg::DSP_CFG1, reg::dsp_cfg1::MASK_HPF_SEL, dacHpfToField(h));
}

Result TAC5212::getDacHpf(DacHpf &out) {
    const uint8_t cfg = _read(0, reg::DSP_CFG1);
    out = fieldToDacHpf(cfg & reg::dsp_cfg1::MASK_HPF_SEL);
    return Result::ok();
}

Result TAC5212::setDacBiquadsPerChannel(uint8_t n) {
    if (n > 3) return Result::error("DAC biquads per channel must be 0..3");
    return _rmw(0, reg::DSP_CFG1, reg::dsp_cfg1::MASK_BQ_CFG, bqCountToField(n));
}

Result TAC5212::getDacBiquadsPerChannel(uint8_t &n) {
    const uint8_t cfg = _read(0, reg::DSP_CFG1);
    n = fieldToBqCount(cfg & reg::dsp_cfg1::MASK_BQ_CFG);
    return Result::ok();
}

Result TAC5212::setDacDvolGang(bool ganged) {
    return _rmw(0, reg::DSP_CFG1, reg::dsp_cfg1::MASK_DVOL_GANG,
                ganged ? reg::dsp_cfg1::MASK_DVOL_GANG : uint8_t{0});
}

Result TAC5212::setDacSoftStep(bool enabled) {
    // The bit is DISABLE_SOFT_STEP (active high disables), so invert.
    return _rmw(0, reg::DSP_CFG1, reg::dsp_cfg1::MASK_DISABLE_SOFT_STEP,
                enabled ? uint8_t{0} : reg::dsp_cfg1::MASK_DISABLE_SOFT_STEP);
}

// -----------------------------------------------------------------------------
// Out per-channel DSP — DVOL + biquads
// -----------------------------------------------------------------------------

namespace {

// Map physical Out N to the DVOL register bases for sub-channels A/B
// (page 18). Output 1 spans CH1A + CH1B; output 2 spans CH2A + CH2B.
// In differential modes both sub-channels need the same volume to keep
// DC offset at zero on the line outputs.
struct DvolRegs { uint8_t a; uint8_t b; };
constexpr DvolRegs dvolRegsFor(uint8_t n) {
    return (n == 2)
        ? DvolRegs{reg::dac_dvol::CH2A_BASE, reg::dac_dvol::CH2B_BASE}
        : DvolRegs{reg::dac_dvol::CH1A_BASE, reg::dac_dvol::CH1B_BASE};
}

// Pack a 32-bit two's-complement big-endian into 4 bytes.
inline void packBE32(uint8_t *out, int32_t value) {
    const uint32_t v = static_cast<uint32_t>(value);
    out[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    out[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    out[2] = static_cast<uint8_t>((v >>  8) & 0xFF);
    out[3] = static_cast<uint8_t>( v        & 0xFF);
}

inline int32_t unpackBE32(const uint8_t *in) {
    const uint32_t v =
        (static_cast<uint32_t>(in[0]) << 24) |
        (static_cast<uint32_t>(in[1]) << 16) |
        (static_cast<uint32_t>(in[2]) <<  8) |
         static_cast<uint32_t>(in[3]);
    return static_cast<int32_t>(v);
}

}  // anonymous namespace

Result TAC5212::Out::setDvol(float dB) {
    if (dB > reg::dac_dvol::DB_MAX)
        return Result::error("DAC dvol exceeds +27 dB max");
    const uint8_t code = reg::dac_dvol::fromDb(dB);
    const DvolRegs r = dvolRegsFor(_n);
    // The DVOL registers are 4 bytes wide per the datasheet's auto-increment
    // protocol — only the MSB carries the encoded code, the remaining 3 are
    // zero. Ship as a single 4-byte burst per sub-channel.
    uint8_t bytes[4] = { code, 0, 0, 0 };
    Result rA = _codec->writeBurst(reg::dac_dvol::PAGE, r.a, bytes, sizeof(bytes));
    if (rA.isError()) return rA;
    return _codec->writeBurst(reg::dac_dvol::PAGE, r.b, bytes, sizeof(bytes));
}

Result TAC5212::Out::getDvol(float &dB) {
    // Read sub-channel A's MSB; A and B are kept in sync by setDvol.
    const DvolRegs r = dvolRegsFor(_n);
    const uint8_t code = _codec->_read(reg::dac_dvol::PAGE, r.a);
    dB = reg::dac_dvol::toDb(code);
    return Result::ok();
}

namespace {

// Hardware allocates 12 biquad slots across 4 channels per Table 7-48.
// For DAC channels 1 and 2, the per-channel band index `idx` (1..3) maps
// to global biquad number:
//   idx 1: BQ_n          where n = channel
//   idx 2: BQ_(n+4)
//   idx 3: BQ_(n+8)
constexpr uint8_t globalBiquadFor(uint8_t channel, uint8_t idx) {
    return channel + (idx - 1) * 4;
}

}  // anonymous namespace

Result TAC5212::Out::setBiquad(uint8_t idx, const BiquadCoeffs &c) {
    if (idx < 1 || idx > 3) return Result::error("biquad idx must be 1..3");

    const uint8_t bq    = globalBiquadFor(_n, idx);
    const uint8_t page  = reg::dac_biquad::pageFor(bq);
    const uint8_t base  = reg::dac_biquad::baseFor(bq);

    // Pack 5 × 4 bytes MSB-first.
    uint8_t bytes[20];
    packBE32(bytes +  0, c.n0);
    packBE32(bytes +  4, c.n1);
    packBE32(bytes +  8, c.n2);
    packBE32(bytes + 12, c.d1);
    packBE32(bytes + 16, c.d2);

    return _codec->writeBurst(page, base, bytes, sizeof(bytes));
}

Result TAC5212::Out::clearBiquad(uint8_t idx) {
    return setBiquad(idx, BiquadCoeffs::bypass());
}

Result TAC5212::Out::getBiquad(uint8_t idx, BiquadCoeffs &out) {
    if (idx < 1 || idx > 3) return Result::error("biquad idx must be 1..3");

    const uint8_t bq    = globalBiquadFor(_n, idx);
    const uint8_t page  = reg::dac_biquad::pageFor(bq);
    const uint8_t base  = reg::dac_biquad::baseFor(bq);

    // Read 20 bytes one at a time. Burst reads aren't a hot path here
    // (only used at snapshot) so the per-byte loop is fine.
    uint8_t bytes[20];
    for (uint8_t i = 0; i < 20; ++i) {
        bytes[i] = _codec->_read(page, static_cast<uint8_t>(base + i));
    }
    out.n0 = unpackBE32(bytes +  0);
    out.n1 = unpackBE32(bytes +  4);
    out.n2 = unpackBE32(bytes +  8);
    out.d1 = unpackBE32(bytes + 12);
    out.d2 = unpackBE32(bytes + 16);
    return Result::ok();
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

// Burst write across consecutive register addresses. Handles the auto
// increment-page-boundary case: after register 0x7F on page N, the chip
// auto-rolls to register 0x08 on page N+1.
//
// Implemented as a sequence of single-page chunks because the Wire library
// has a small TX buffer (~32 bytes on Teensy 4.x) and ChunkySplitting it
// per page also keeps the page-cache (`_curPage`) in sync.
Result TAC5212::writeBurst(uint8_t page, uint8_t startReg, const uint8_t *bytes, size_t n) {
    if (n == 0) return Result::ok();
    if (bytes == nullptr) return Result::error("writeBurst: null bytes pointer");

    size_t remaining = n;
    uint8_t curPage  = page;
    uint8_t curReg   = startReg;
    const uint8_t *cursor = bytes;

    while (remaining > 0) {
        Result pg = _selectPage(curPage);
        if (pg.isError()) return pg;

        // Chunk size: don't cross into the next page (after 0x7F → next page
        // 0x08). Also keep within the Wire library TX buffer; 16 data bytes
        // per transaction stays well under the Teensy default.
        const size_t inThisPage = (curReg <= 0x7F)
            ? static_cast<size_t>(0x80 - curReg)
            : 0;
        const size_t maxChunk   = 16;
        size_t chunk = remaining;
        if (chunk > inThisPage) chunk = inThisPage;
        if (chunk > maxChunk)   chunk = maxChunk;
        if (chunk == 0) {
            // Already at end-of-page; force next-page roll by re-selecting
            // and resetting curReg to 0x08 (the chip does this automatically
            // on auto-increment, but we mirror it explicitly for clarity).
            curPage = static_cast<uint8_t>(curPage + 1);
            curReg  = 0x08;
            continue;
        }

        _wire->beginTransmission(_addr);
        _wire->write(curReg);
        for (size_t i = 0; i < chunk; ++i) {
            _wire->write(cursor[i]);
        }
        const uint8_t err = _wire->endTransmission();
        if (err != 0) {
            ++_errors;
            return Result::error("I2C NACK during burst write");
        }

        cursor    += chunk;
        remaining -= chunk;
        // Advance curReg; if we hit 0x80 step into next page at 0x08.
        const uint16_t advanced = static_cast<uint16_t>(curReg) + chunk;
        if (advanced >= 0x80) {
            curPage = static_cast<uint8_t>(curPage + 1);
            curReg  = static_cast<uint8_t>(0x08 + (advanced - 0x80));
            // The chip auto-rolled the page pointer; sync our cache.
            _curPage = curPage;
        } else {
            curReg = static_cast<uint8_t>(advanced);
        }
    }
    return Result::ok();
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
