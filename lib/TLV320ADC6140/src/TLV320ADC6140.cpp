// TLV320ADC6140 — driver implementation. See header for design notes.

#include "TLV320ADC6140.h"
#include "TLV320ADC6140_Registers.h"

namespace tlv320adc6140 {

// --- Construction ----------------------------------------------------------

TLV320ADC6140::TLV320ADC6140(TwoWire &wire) : _wire(&wire) {}

// --- I²C primitives --------------------------------------------------------

Result TLV320ADC6140::_selectPage(uint8_t page) {
    if (page == _curPage) return Result::ok();
    _wire->beginTransmission(_addr);
    _wire->write(REG_PAGE_SELECT);
    _wire->write(page);
    if (_wire->endTransmission() != 0) {
        ++_errors;
        return Result::error("i2c nack on page select");
    }
    _curPage = page;
    return Result::ok();
}

Result TLV320ADC6140::_write(uint8_t page, uint8_t reg, uint8_t value) {
    Result r = _selectPage(page);
    if (!r.isOk()) return r;
    _wire->beginTransmission(_addr);
    _wire->write(reg);
    _wire->write(value);
    if (_wire->endTransmission() != 0) {
        ++_errors;
        return Result::error("i2c nack on register write");
    }
    return Result::ok();
}

uint8_t TLV320ADC6140::_read(uint8_t page, uint8_t reg) {
    if (!_selectPage(page).isOk()) return 0xFF;
    _wire->beginTransmission(_addr);
    _wire->write(reg);
    if (_wire->endTransmission(false) != 0) {
        ++_errors;
        return 0xFF;
    }
    _wire->requestFrom(_addr, (uint8_t)1);
    if (!_wire->available()) {
        ++_errors;
        return 0xFF;
    }
    return _wire->read();
}

Result TLV320ADC6140::_rmw(uint8_t page, uint8_t reg, uint8_t mask, uint8_t value) {
    uint8_t cur = _read(page, reg);
    uint8_t next = (cur & ~mask) | (value & mask);
    return _write(page, reg, next);
}

// --- Lifecycle -------------------------------------------------------------

Result TLV320ADC6140::begin(uint8_t addr) {
    _addr = addr;
    _curPage = 0xFF;  // force first page-select to transact

    // Probe.
    _wire->beginTransmission(_addr);
    if (_wire->endTransmission() != 0) {
        ++_errors;
        return Result::error("adc6140 not on i2c bus");
    }

    Result r = reset();
    if (!r.isOk()) return r;
    return wake(true);
}

Result TLV320ADC6140::reset() {
    // SW_RESET is self-clearing. All registers return to POR defaults.
    Result r = _write(0x00, REG_SW_RESET, SW_RESET_TRIGGER);
    if (!r.isOk()) return r;
    delay(10);  // datasheet §9.2.1.2 recommends >= 1 ms
    _dspCfgCache_init();
    return Result::ok();
}

Result TLV320ADC6140::wake(bool awake) {
    // SLEEP_CFG bit 0 SLEEP_ENZ: 0 = sleep, 1 = active.
    // Bit 7 AREG_SELECT: 1 = use internal regulator (AVDD ≥ 2.7 V).
    // Our board runs AVDD = 3.3 V, so pick internal regulator.
    uint8_t v = awake ? SLEEP_CFG_WAKE_INTERNAL_AREG : 0x80;
    Result r = _write(0x00, REG_SLEEP_CFG, v);
    if (!r.isOk()) return r;
    delay(2);  // datasheet §8.4.3 recommends >= 1 ms after wake
    return Result::ok();
}

// --- Info / status ---------------------------------------------------------

DeviceInfo TLV320ADC6140::info() const {
    return DeviceInfo{"TLV320ADC6140", _addr};
}

Status TLV320ADC6140::readStatus() {
    Status s{};
    s.devSts0 = _read(0x00, REG_DEV_STS0);
    s.devSts1 = _read(0x00, REG_DEV_STS1);
    s.asiSts  = _read(0x00, REG_ASI_STS);
    s.ch1Powered = (s.devSts0 & DEV_STS0_CH_STATUS(1)) != 0;
    s.ch2Powered = (s.devSts0 & DEV_STS0_CH_STATUS(2)) != 0;
    s.ch3Powered = (s.devSts0 & DEV_STS0_CH_STATUS(3)) != 0;
    s.ch4Powered = (s.devSts0 & DEV_STS0_CH_STATUS(4)) != 0;
    const uint8_t mode = s.devSts1 & DEV_STS1_MODE_MASK;
    s.sleepMode = (mode == DEV_STS1_MODE_SLEEP);
    s.active    = (mode == DEV_STS1_MODE_ACTIVE_IDLE) || (mode == DEV_STS1_MODE_RECORDING);
    s.recording = (mode == DEV_STS1_MODE_RECORDING);
    // ASI_STS: upper nibble FS_RATE_STS, lower nibble FS_RATIO_STS.
    // 0xF in either nibble means "invalid / not detected yet".
    const uint8_t rate  = (s.asiSts >> 4) & 0x0F;
    const uint8_t ratio = s.asiSts & 0x0F;
    s.asiClockValid = (rate != 0x0F) && (ratio != 0x0F);
    return s;
}

void TLV320ADC6140::dumpStatus(Print &out) {
    Status s = readStatus();
    out.print(F("ADC6140 mode: "));
    out.print(s.sleepMode ? F("sleep")
             : s.recording ? F("recording")
             : s.active    ? F("active-idle")
             :               F("unknown"));
    out.print(F("  CH1-4 power: "));
    out.print((unsigned)s.ch1Powered); out.print(' ');
    out.print((unsigned)s.ch2Powered); out.print(' ');
    out.print((unsigned)s.ch3Powered); out.print(' ');
    out.print((unsigned)s.ch4Powered);
    out.print(F("  ASI STS=0x"));
    if (s.asiSts < 0x10) out.print('0');
    out.print(s.asiSts, HEX);
    out.print(F("  clk="));
    out.println(s.asiClockValid ? F("valid") : F("INVALID"));
}

// --- Audio Serial Interface ------------------------------------------------

Result TLV320ADC6140::setSerialFormat(const SerialFormat &fmt) {
    uint8_t v = 0;
    switch (fmt.format) {
      case Format::Tdm:           v |= ASI_FORMAT_TDM; break;
      case Format::I2s:           v |= ASI_FORMAT_I2S; break;
      case Format::LeftJustified: v |= ASI_FORMAT_LJ;  break;
    }
    switch (fmt.wordLen) {
      case WordLen::Bits16: v |= ASI_WLEN_16; break;
      case WordLen::Bits20: v |= ASI_WLEN_20; break;
      case WordLen::Bits24: v |= ASI_WLEN_24; break;
      case WordLen::Bits32: v |= ASI_WLEN_32; break;
    }
    if (fmt.fsyncPol == Polarity::Inverted) v |= ASI_FSYNC_POL_INV;
    if (fmt.bclkPol  == Polarity::Inverted) v |= ASI_BCLK_POL_INV;
    if (fmt.txFillHiZ)                      v |= ASI_TX_FILL_HIZ;

    Result r = _write(0x00, REG_ASI_CFG0, v);
    if (!r.isOk()) return r;

    // TX_OFFSET in ASI_CFG1 bits[4:0].
    r = _rmw(0x00, REG_ASI_CFG1, 0x1F, fmt.txOffset & 0x1F);
    if (!r.isOk()) return r;

    return Result::ok();
}

Result TLV320ADC6140::setChannelSlot(uint8_t ch, uint8_t slot) {
    if (ch < 1 || ch > 8) return Result::error("channel out of range (1..8)");
    if (slot > 63) return Result::error("slot out of range (0..63)");
    uint8_t reg = REG_ASI_CH1 + (ch - 1);
    uint8_t v   = ASI_CH_OUTPUT_PRIMARY | (slot & 0x3F);
    return _write(0x00, reg, v);
}

// --- Reference & bias ------------------------------------------------------

Result TLV320ADC6140::setFullScale(FullScale fs) {
    uint8_t bits = 0;
    switch (fs) {
      case FullScale::V2Rms275:  bits = ADC_FSCALE_2V75;  break;
      case FullScale::V1Rms8250: bits = ADC_FSCALE_2V5;   break;
      case FullScale::V1Rms1375: bits = ADC_FSCALE_1V375; break;
    }
    _biasCfgCache = (_biasCfgCache & 0xFC) | bits;
    return _write(0x00, REG_BIAS_CFG, _biasCfgCache);
}

Result TLV320ADC6140::setMicBias(MicBias mb) {
    // Power enable (PWR_CFG) is separate from value (BIAS_CFG). Handle value
    // here; enable/disable via powerUp().
    uint8_t bits;
    switch (mb) {
      case MicBias::Off:
      case MicBias::Vref:        bits = MBIAS_VAL_VREF;  break;
      case MicBias::VrefBoosted: bits = MBIAS_VAL_1P096; break;
      case MicBias::Avdd:        bits = MBIAS_VAL_AVDD;  break;
    }
    _biasCfgCache = (_biasCfgCache & 0x8F) | bits;
    Result r = _write(0x00, REG_BIAS_CFG, _biasCfgCache);
    if (!r.isOk()) return r;

    // MICBIAS_PDZ lives in PWR_CFG bit 7. Flip it based on `Off`.
    // RMW so we don't disturb ADC_PDZ / PLL_PDZ.
    return _rmw(0x00, REG_PWR_CFG, PWR_CFG_MICBIAS_PDZ,
                (mb == MicBias::Off) ? 0x00 : PWR_CFG_MICBIAS_PDZ);
}

// --- Signal chain ----------------------------------------------------------

Result TLV320ADC6140::setDecimationFilter(DecimationFilter df) {
    uint8_t bits = 0;
    switch (df) {
      case DecimationFilter::LinearPhase:     bits = DECI_LINEAR;      break;
      case DecimationFilter::LowLatency:      bits = DECI_LOW_LATENCY; break;
      case DecimationFilter::UltraLowLatency: bits = DECI_ULTRA_LL;    break;
    }
    _dspCfg0Cache = (_dspCfg0Cache & ~0x30) | bits;
    return _write(0x00, REG_DSP_CFG0, _dspCfg0Cache);
}

Result TLV320ADC6140::setChannelSumMode(ChannelSumMode m) {
    uint8_t bits = 0;
    switch (m) {
      case ChannelSumMode::Off:   bits = CH_SUM_OFF; break;
      case ChannelSumMode::Pairs: bits = CH_SUM_2CH; break;
      case ChannelSumMode::Quad:  bits = CH_SUM_4CH; break;
    }
    _dspCfg0Cache = (_dspCfg0Cache & ~0x0C) | bits;
    return _write(0x00, REG_DSP_CFG0, _dspCfg0Cache);
}

Result TLV320ADC6140::setHpf(HpfCutoff hp) {
    uint8_t bits = 0;
    switch (hp) {
      case HpfCutoff::Programmable: bits = HPF_PROGRAMMABLE; break;
      case HpfCutoff::Cutoff12Hz:   bits = HPF_CUTOFF_12HZ;  break;
      case HpfCutoff::Cutoff96Hz:   bits = HPF_CUTOFF_96HZ;  break;
      case HpfCutoff::Cutoff384Hz:  bits = HPF_CUTOFF_384HZ; break;
    }
    _dspCfg0Cache = (_dspCfg0Cache & ~0x03) | bits;
    return _write(0x00, REG_DSP_CFG0, _dspCfg0Cache);
}

Result TLV320ADC6140::setDreAgcMode(DreAgcMode m) {
    const uint8_t bit = (m == DreAgcMode::Agc) ? DSP_CFG1_SELECT_AGC : 0;
    _dspCfg1Cache = (_dspCfg1Cache & ~DSP_CFG1_SELECT_AGC) | bit;
    return _write(0x00, REG_DSP_CFG1, _dspCfg1Cache);
}

Result TLV320ADC6140::setDreLevel(int8_t db) {
    // Map dB → 4-bit code per datasheet Table 44.
    // 0 → -12, 1 → -18, 2 → -24, 3 → -30, 4 → -36, 5 → -42, 6 → -48,
    // 7 → -54 (default), 8 → -60, 9 → -66.
    uint8_t code;
    switch (db) {
      case -12: code = 0;  break;
      case -18: code = 1;  break;
      case -24: code = 2;  break;
      case -30: code = 3;  break;
      case -36: code = 4;  break;
      case -42: code = 5;  break;
      case -48: code = 6;  break;
      case -54: code = 7;  break;
      case -60: code = 8;  break;
      case -66: code = 9;  break;
      default:  return Result::error("dre level must be -12..-66 dB in 6 dB steps");
    }
    return _rmw(0x00, REG_DRE_CFG0, 0xF0, code << 4);
}

Result TLV320ADC6140::setDreMaxGain(uint8_t db) {
    // 0 → 2 dB, 1 → 4 dB, ..., 11 → 24 dB, ..., 14 → 30 dB.
    if (db < 2 || db > 30 || (db & 1) != 0)
        return Result::error("dre max gain must be even 2..30 dB");
    uint8_t code = (db - 2) / 2;
    return _rmw(0x00, REG_DRE_CFG0, 0x0F, code);
}

Result TLV320ADC6140::setAgcTargetLevel(int8_t db) {
    // 0 → -6, 1 → -8, ..., 14 → -34 (default), 15 → -36.
    if (db > -6 || db < -36 || ((-db) & 1) != 0)
        return Result::error("agc target must be even -6..-36 dB");
    uint8_t code = (uint8_t)((-db - 6) / 2);
    return _rmw(0x00, REG_AGC_CFG0, 0xF0, code << 4);
}

Result TLV320ADC6140::setAgcMaxGain(uint8_t db) {
    // 0 → 3 dB, 1 → 6 dB, ..., 13 → 42 dB.
    if (db < 3 || db > 42 || (db % 3) != 0)
        return Result::error("agc max gain must be 3..42 in 3 dB steps");
    uint8_t code = (db / 3) - 1;
    return _rmw(0x00, REG_AGC_CFG0, 0x0F, code);
}

Result TLV320ADC6140::setDvolGang(bool gang) {
    const uint8_t bit = gang ? DSP_CFG1_DVOL_GANG : 0;
    _dspCfg1Cache = (_dspCfg1Cache & ~DSP_CFG1_DVOL_GANG) | bit;
    return _write(0x00, REG_DSP_CFG1, _dspCfg1Cache);
}

Result TLV320ADC6140::setSoftStep(bool enabled) {
    // bit 4 is DISABLE_SOFT_STEP — inverted polarity vs the arg.
    const uint8_t bit = enabled ? 0 : DSP_CFG1_SOFT_STEP_OFF;
    _dspCfg1Cache = (_dspCfg1Cache & ~DSP_CFG1_SOFT_STEP_OFF) | bit;
    return _write(0x00, REG_DSP_CFG1, _dspCfg1Cache);
}

Result TLV320ADC6140::setBiquadsPerChannel(uint8_t n) {
    if (n > 3) return Result::error("biquads per channel must be 0..3");
    uint8_t bits = 0;
    switch (n) {
      case 0: bits = DSP_CFG1_BIQUAD_0; break;
      case 1: bits = DSP_CFG1_BIQUAD_1; break;
      case 2: bits = DSP_CFG1_BIQUAD_2; break;
      case 3: bits = DSP_CFG1_BIQUAD_3; break;
    }
    _dspCfg1Cache = (_dspCfg1Cache & ~0x60) | bits;
    return _write(0x00, REG_DSP_CFG1, _dspCfg1Cache);
}

// --- Power / enable --------------------------------------------------------

Result TLV320ADC6140::setChannelEnable(uint8_t inMask, uint8_t outMask) {
    Result r = _write(0x00, REG_IN_CH_EN, inMask);
    if (!r.isOk()) return r;
    return _write(0x00, REG_ASI_OUT_CH_EN, outMask);
}

Result TLV320ADC6140::powerUp(bool adc, bool micbias, bool pll) {
    // Preserve dynamic-channel bits — RMW rather than clobbering.
    uint8_t bits = 0;
    if (micbias) bits |= PWR_CFG_MICBIAS_PDZ;
    if (adc)     bits |= PWR_CFG_ADC_PDZ;
    if (pll)     bits |= PWR_CFG_PLL_PDZ;
    return _rmw(0x00, REG_PWR_CFG,
                PWR_CFG_MICBIAS_PDZ | PWR_CFG_ADC_PDZ | PWR_CFG_PLL_PDZ,
                bits);
}

// --- Raw register access ---------------------------------------------------

Result  TLV320ADC6140::writeRegister(uint8_t page, uint8_t reg, uint8_t v) { return _write(page, reg, v); }
uint8_t TLV320ADC6140::readRegister (uint8_t page, uint8_t reg)           { return _read (page, reg); }

// --- Per-channel accessor --------------------------------------------------

TLV320ADC6140::Channel TLV320ADC6140::channel(uint8_t n) {
    if (n < 1) n = 1;
    if (n > 4) n = 4;
    return Channel(this, n);
}

uint8_t TLV320ADC6140::Channel::_cfgReg(uint8_t offset) const {
    return REG_CH1_CFG0 + (_n - 1) * 5 + offset;
}

Result TLV320ADC6140::Channel::setType(InputType t) {
    const uint8_t bits = (t == InputType::Line) ? CH_CFG0_INTYP_LINE : CH_CFG0_INTYP_MIC;
    return _codec->_rmw(0x00, _cfgReg(0), 0x80, bits);
}

Result TLV320ADC6140::Channel::setSource(InputSource s) {
    uint8_t bits = 0;
    switch (s) {
      case InputSource::Differential: bits = CH_CFG0_INSRC_DIFF; break;
      case InputSource::SingleEnded:  bits = CH_CFG0_INSRC_SE;   break;
      case InputSource::PdmDigital:   bits = CH_CFG0_INSRC_PDM;  break;
    }
    return _codec->_rmw(0x00, _cfgReg(0), 0x60, bits);
}

Result TLV320ADC6140::Channel::setCoupling(Coupling c) {
    const uint8_t bits = (c == Coupling::Dc) ? CH_CFG0_COUPLING_DC : CH_CFG0_COUPLING_AC;
    return _codec->_rmw(0x00, _cfgReg(0), 0x10, bits);
}

Result TLV320ADC6140::Channel::setImpedance(Impedance z) {
    uint8_t bits = 0;
    switch (z) {
      case Impedance::K2_5: bits = CH_CFG0_IMP_2_5K; break;
      case Impedance::K10:  bits = CH_CFG0_IMP_10K;  break;
      case Impedance::K20:  bits = CH_CFG0_IMP_20K;  break;
    }
    return _codec->_rmw(0x00, _cfgReg(0), 0x0C, bits);
}

Result TLV320ADC6140::Channel::setDreEnable(bool on) {
    return _codec->_rmw(0x00, _cfgReg(0), 0x01, on ? CH_CFG0_DRE_ENABLE : 0);
}

Result TLV320ADC6140::Channel::setGainDb(uint8_t db) {
    if (db > 42) return Result::error("analog gain must be 0..42 dB");
    // bits[7:2] = gain, bits[1:0] reserved 0.
    return _codec->_write(0x00, _cfgReg(1), (uint8_t)(db << 2));
}

Result TLV320ADC6140::Channel::setDvolDb(float db) {
    if (db < -101.0f) {
        return _codec->_write(0x00, _cfgReg(2), DVOL_MUTE);
    }
    if (db > 27.0f)  db = 27.0f;
    if (db < -100.0f) db = -100.0f;
    // reg = 201 + dB*2 (0.5 dB steps). 0 = mute.
    int code = 201 + (int)lrintf(db * 2.0f);
    if (code < 1)   code = 1;
    if (code > 255) code = 255;
    return _codec->_write(0x00, _cfgReg(2), (uint8_t)code);
}

Result TLV320ADC6140::Channel::setGainCalDb(float db) {
    // 0 = -0.8 dB, 8 = 0 dB, 15 = +0.7 dB. 0.1 dB/step.
    int code = 8 + (int)lrintf(db * 10.0f);
    if (code < 0)  code = 0;
    if (code > 15) code = 15;
    return _codec->_rmw(0x00, _cfgReg(3), 0xF0, (uint8_t)(code << 4));
}

Result TLV320ADC6140::Channel::setPhaseCalCycles(uint8_t cycles) {
    return _codec->_write(0x00, _cfgReg(4), cycles);
}

// --- private cache re-init helper (called from reset()) --------------------

void TLV320ADC6140::_dspCfgCache_init() {
    // Datasheet POR defaults for DSP_CFG0 / DSP_CFG1.
    _dspCfg0Cache = 0x01;  // HPF=12 Hz (POR), CH_SUM=off, decim=linear
    _dspCfg1Cache = 0x40;  // 2 biquads/ch, soft-step on, DRE selected
    _biasCfgCache = 0x00;  // MBIAS_VAL=VREF, ADC_FSCALE=2.75 V
}

}  // namespace tlv320adc6140
