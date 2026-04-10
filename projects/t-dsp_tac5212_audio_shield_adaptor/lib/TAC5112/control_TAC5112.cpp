/*
 * TAC5112 Control Library Implementation
 * 
 * Texas Instruments TAC5112 Low-Power Stereo Audio Codec
 * 
 * This implementation is designed to work with the Teensy Audio Library
 */

#include "control_TAC5112.h"

// =============================================================================
// Constructor
// =============================================================================

AudioControlTAC5112::AudioControlTAC5112(uint8_t i2cAddr) {
    _i2cAddr = i2cAddr;
    _wire = &Wire;
    _currentPage = 0;
    _initialized = false;
}

// =============================================================================
// Initialization
// =============================================================================

bool AudioControlTAC5112::begin(void) {
    return begin(&Wire);
}

bool AudioControlTAC5112::begin(TwoWire *wire) {
    _wire = wire;
    _wire->begin();
    
    // Small delay for device to be ready after power-up
    delay(10);
    
    // Try to communicate with the device
    _wire->beginTransmission(_i2cAddr);
    if (_wire->endTransmission() != 0) {
        // Device not responding
        return false;
    }
    
    // Set to page 0
    setPage(0);
    
    _initialized = true;
    return true;
}

// =============================================================================
// Configure for Teensy Audio Library I2S
// 
// The Teensy Audio Library uses:
//   - I2S format (not TDM or LJ)
//   - MCLK = 256 × Fs (e.g., 11.2896 MHz at 44.1kHz)
//   - BCLK = 64 × Fs (e.g., 2.8224 MHz at 44.1kHz)
//   - LRCLK = Fs (44.1kHz or 48kHz typically)
//   - 32-bit word length per channel
//   - MSB first, data starts one BCLK after LRCLK edge
//   - Teensy is I2S Master (provides all clocks)
//   - Left channel when LRCLK is LOW
// =============================================================================

bool AudioControlTAC5112::configureForTeensyI2S(void) {
    if (!_initialized) return false;
    
    // Set page 0
    setPage(0);
    
    // Software reset to ensure clean state
    writeRegister(TAC5112_REG_SW_RESET, 0x01);
    delay(5);
    
    // ------------------------------------------------------------------------
    // Wake Up and Basic Configuration
    // Based on TI EVM configuration examples and datasheet
    // ------------------------------------------------------------------------
    
    // SLEEP_CFG (0x02): Wake up device with AVDD > 2V and all VDDIO levels
    writeRegister(TAC5112_REG_SLEEP_CFG, 0x09);
    
    // INTF_CFG1 (0x10): Configure DOUT as Primary ASI DOUT with drive
    // Bits 7:4 = 5 (PASI DOUT), Bits 2:0 = 2 (drive active low/weak high)
    writeRegister(TAC5112_REG_INTF_CFG1, 0x52);
    
    // INTF_CFG2 (0x11): Enable Primary ASI DIN for playback
    // Bit 7 = 1 (PASI DIN enabled)
    writeRegister(TAC5112_REG_INTF_CFG2, 0x80);
    
    // ASI_CFG1 (0x19): Data lanes config - default is 1 DOUT + 1 DIN for PASI
    writeRegister(TAC5112_REG_ASI_CFG1, 0x00);
    
    // ------------------------------------------------------------------------
    // Primary ASI Configuration for Teensy I2S compatibility
    // All format/polarity settings are in PASI_CFG0 (0x1A)
    // ------------------------------------------------------------------------
    
    // PASI_CFG0 (0x1A): I2S format, 32-bit, standard polarity
    // Bits 7:6 = 01 (I2S), Bits 5:4 = 11 (32-bit), Bits 3:2 = 00 (normal polarity)
    writeRegister(TAC5112_REG_PASI_CFG0, 
                  TAC5112_ASI_FORMAT_I2S | TAC5112_ASI_WLEN_32BIT);
    
    // PASI_TX_CFG2 (0x1D): TX Offset = 0 for standard I2S
    writeRegister(TAC5112_REG_PASI_TX_CFG2, 0x00);
    
    // PASI_RX_CFG1 (0x27): RX Offset = 0 for standard I2S
    writeRegister(TAC5112_REG_PASI_RX_CFG1, 0x00);
    
    // ------------------------------------------------------------------------
    // Channel Slot Assignment
    // Per datasheet Table 8-29/8-39:
    //   Bit 5 = channel enable (1=enabled)
    //   Bits 4:0 = slot number (0=left/slot0, 16=right/slot0 in I2S)
    // For I2S stereo: CH1=0x20 (left), CH2=0x30 (right with slot 16)
    // ------------------------------------------------------------------------
    
    // TX (ADC output) slot assignment
    writeRegister(TAC5112_REG_TX_CH1_SLOT, 0x20);  // CH1: enabled, slot 0 (left)
    writeRegister(TAC5112_REG_TX_CH2_SLOT, 0x30);  // CH2: enabled, slot 16 (right)
    
    // RX (DAC input) slot assignment
    writeRegister(TAC5112_REG_RX_CH1_SLOT, 0x20);  // CH1: enabled, slot 0 (left)
    writeRegister(TAC5112_REG_RX_CH2_SLOT, 0x30);  // CH2: enabled, slot 16 (right)
    
    // ------------------------------------------------------------------------
    // Reference Voltage Configuration
    // For 3.3V AVDD, use 2.75V reference
    // ------------------------------------------------------------------------
    writeRegister(TAC5112_REG_VREF_CFG, TAC5112_VREF_2V75);
    
    // ------------------------------------------------------------------------
    // ADC Configuration (Recording Path)
    // Differential inputs, 5kΩ impedance, AC-coupled, 2Vrms, audio band
    // Register value 0x00 = differential, 5kΩ, AC-coupled, 2Vrms
    // ------------------------------------------------------------------------
    
    writeRegister(TAC5112_REG_ADC_CH1_CFG0, 0x00);  // Diff, 5kΩ, AC, 2Vrms
    writeRegister(TAC5112_REG_ADC_CH2_CFG0, 0x00);  // Diff, 5kΩ, AC, 2Vrms
    
    // ------------------------------------------------------------------------
    // Output Configuration (Playback Path)
    // Configure OUT1/OUT2 as differential line outputs driven by DAC, 0dB gain
    // Based on TI example values
    // ------------------------------------------------------------------------

    // Output Channel 1 (Left)
    // OUT1_CFG0 (0x64): Bits[7:5]=001 (DAC source), Bits[4:2]=000 (differential)
    writeRegister(TAC5112_REG_OUT1_CFG0, 0x20);     // OUT1x source = DAC, differential routing
    // OUT1_CFG1 (0x65): Bits[7:6]=00 (line driver), Bits[5:3]=100 (0dB level)
    writeRegister(TAC5112_REG_OUT1_CFG1, 0x20);     // OUT1P: Line driver, 0dB analog level
    // OUT1_CFG2 (0x66): Bits[7:6]=00 (line driver), Bits[5:3]=100 (0dB level)
    writeRegister(TAC5112_REG_OUT1_CFG2, 0x20);     // OUT1M: Line driver, 0dB analog level

    // Output Channel 2 (Right)
    // OUT2_CFG0 (0x6B): Bits[7:5]=001 (DAC source), Bits[4:2]=000 (differential)
    writeRegister(TAC5112_REG_OUT2_CFG0, 0x20);     // OUT2x source = DAC, differential routing
    // OUT2_CFG1 (0x6C): Bits[7:6]=00 (line driver), Bits[5:3]=100 (0dB level)
    writeRegister(TAC5112_REG_OUT2_CFG1, 0x20);     // OUT2P: Line driver, 0dB analog level
    // OUT2_CFG2 (0x6D): Bits[7:6]=00 (line driver), Bits[5:3]=100 (0dB level)
    writeRegister(TAC5112_REG_OUT2_CFG2, 0x20);     // OUT2M: Line driver, 0dB analog level
    
    // ------------------------------------------------------------------------
    // Channel Enable (0x76)
    // Enable both ADC input channels AND both DAC output channels
    // ------------------------------------------------------------------------
    writeRegister(TAC5112_REG_CH_EN, 
                  TAC5112_CH_EN_IN1 | TAC5112_CH_EN_IN2 |   // Input channels
                  TAC5112_CH_EN_OUT1 | TAC5112_CH_EN_OUT2); // Output channels
    
    // ------------------------------------------------------------------------
    // Power Configuration (0x78)
    // Power up ADC, DAC, and MICBIAS
    // ------------------------------------------------------------------------
    writeRegister(TAC5112_REG_PWR_CFG, 
                  TAC5112_PWR_ADC | TAC5112_PWR_DAC | TAC5112_PWR_MICBIAS);
    
    // Small delay for power-up sequence
    delay(10);
    
    return true;
}

// =============================================================================
// Software Reset
// =============================================================================

bool AudioControlTAC5112::reset(void) {
    // Write 0x01 to software reset register
    if (!writeRegister(TAC5112_REG_SW_RESET, 0x01)) {
        return false;
    }
    _currentPage = 0;
    delay(1);  // Wait for reset to complete
    return true;
}

// =============================================================================
// ASI Configuration
// =============================================================================

bool AudioControlTAC5112::setASIFormat(TAC5112_ASIFormat format) {
    // Format is in PASI_CFG0 (0x1A), bits 7:6
    return modifyRegister(TAC5112_REG_PASI_CFG0, TAC5112_ASI_FORMAT_MASK, 
                          (uint8_t)format << 6);
}

bool AudioControlTAC5112::setWordLength(TAC5112_WordLength wlen) {
    // Word length is in PASI_CFG0 (0x1A), bits 5:4
    return modifyRegister(TAC5112_REG_PASI_CFG0, TAC5112_ASI_WLEN_MASK,
                          (uint8_t)wlen << 4);
}

bool AudioControlTAC5112::setFsyncPolarity(bool inverted) {
    // FSYNC polarity is bit 3 of PASI_CFG0 (0x1A)
    return modifyRegister(TAC5112_REG_PASI_CFG0, 0x08, inverted ? 0x08 : 0x00);
}

bool AudioControlTAC5112::setBclkPolarity(bool inverted) {
    // BCLK polarity is bit 2 of PASI_CFG0 (0x1A)
    return modifyRegister(TAC5112_REG_PASI_CFG0, 0x04, inverted ? 0x04 : 0x00);
}

bool AudioControlTAC5112::setTxOffset(uint8_t offset) {
    if (offset > 31) offset = 31;
    // TX offset is in PASI_TX_CFG2 (0x1D), bits 4:0
    return writeRegister(TAC5112_REG_PASI_TX_CFG2, offset & 0x1F);
}

bool AudioControlTAC5112::setRxOffset(uint8_t offset) {
    if (offset > 31) offset = 31;
    // RX offset is in PASI_RX_CFG1 (0x27), bits 4:0
    return writeRegister(TAC5112_REG_PASI_RX_CFG1, offset & 0x1F);
}

// =============================================================================
// Slot Assignment
// =============================================================================

bool AudioControlTAC5112::setTxSlot(uint8_t channel, uint8_t slot) {
    if (channel < 1 || channel > 2 || slot > 31) return false;
    uint8_t reg = (channel == 1) ? TAC5112_REG_TX_CH1_SLOT : TAC5112_REG_TX_CH2_SLOT;
    return writeRegister(reg, slot & 0x1F);
}

bool AudioControlTAC5112::setRxSlot(uint8_t channel, uint8_t slot) {
    if (channel < 1 || channel > 2 || slot > 31) return false;
    uint8_t reg = (channel == 1) ? TAC5112_REG_RX_CH1_SLOT : TAC5112_REG_RX_CH2_SLOT;
    return writeRegister(reg, slot & 0x1F);
}

// =============================================================================
// ADC Configuration
// =============================================================================

bool AudioControlTAC5112::setADCInputSource(uint8_t channel, TAC5112_InputSource source) {
    if (channel < 1 || channel > 2) return false;
    uint8_t reg = (channel == 1) ? TAC5112_REG_ADC_CH1_CFG0 : TAC5112_REG_ADC_CH2_CFG0;
    return modifyRegister(reg, 0xC0, (uint8_t)source << 6);
}

bool AudioControlTAC5112::setADCInputImpedance(uint8_t channel, TAC5112_InputImpedance impedance) {
    if (channel < 1 || channel > 2) return false;
    uint8_t reg = (channel == 1) ? TAC5112_REG_ADC_CH1_CFG0 : TAC5112_REG_ADC_CH2_CFG0;
    return modifyRegister(reg, 0x30, (uint8_t)impedance << 4);
}

bool AudioControlTAC5112::setADCACCoupled(uint8_t channel, bool acCoupled) {
    if (channel < 1 || channel > 2) return false;
    uint8_t reg = (channel == 1) ? TAC5112_REG_ADC_CH1_CFG0 : TAC5112_REG_ADC_CH2_CFG0;
    // AC-coupled = 0, DC-coupled with high CM tolerance = 2
    return modifyRegister(reg, 0x0C, acCoupled ? 0x00 : 0x08);
}

bool AudioControlTAC5112::setADCDigitalVolume(uint8_t channel, float volume_dB) {
    if (channel < 1 || channel > 2) return false;
    
    // Digital Volume Control (DVC) encoding per Table 7-17:
    //   0 = Mute
    //   1 = -80dB
    //   161 = 0dB (default)
    //   255 = +47dB
    // Step size: 0.5dB, Formula: register = (dB × 2) + 161
    
    uint8_t regVal;
    if (volume_dB <= -100.0f) {
        regVal = 0x00;  // Mute
    } else {
        // Clamp to valid range
        if (volume_dB < -80.0f) volume_dB = -80.0f;
        if (volume_dB > 47.0f) volume_dB = 47.0f;
        
        // Convert: reg = (dB × 2) + 161
        int16_t calc = (int16_t)(volume_dB * 2.0f) + 161;
        if (calc < 1) calc = 1;      // Minimum non-mute
        if (calc > 255) calc = 255;  // Maximum
        regVal = (uint8_t)calc;
    }
    
    uint8_t reg = (channel == 1) ? TAC5112_REG_ADC_CH1_VOL : TAC5112_REG_ADC_CH2_VOL;
    return writeRegister(reg, regVal);
}

bool AudioControlTAC5112::setADCFineGainCalibration(uint8_t channel, float gain_dB) {
    if (channel < 1 || channel > 2) return false;
    
    // Fine gain calibration per Table 7-18 / Table 8-73:
    // Range: -0.8dB to +0.7dB in 0.1dB steps
    // Register bits 7:4 of ADC_CHx_CFG3
    // 0 = -0.8dB, 8 = 0dB (default), 15 = +0.7dB
    // Formula: reg = (gain_dB × 10) + 8
    //
    // This is for CHANNEL MATCHING CALIBRATION only!
    // For volume control, use setADCDigitalVolume() which has -80dB to +47dB range.
    
    // Clamp to valid range
    if (gain_dB < -0.8f) gain_dB = -0.8f;
    if (gain_dB > 0.7f) gain_dB = 0.7f;
    
    // Convert: reg = (gain × 10) + 8
    int16_t calc = (int16_t)(gain_dB * 10.0f) + 8;
    if (calc < 0) calc = 0;
    if (calc > 15) calc = 15;
    uint8_t regVal = (uint8_t)calc << 4;  // Bits 7:4
    
    uint8_t reg = (channel == 1) ? TAC5112_REG_ADC_CH1_CFG3 : TAC5112_REG_ADC_CH2_CFG3;
    return modifyRegister(reg, 0xF0, regVal);
}

bool AudioControlTAC5112::enableADC(uint8_t channel, bool enable) {
    if (channel < 1 || channel > 2) return false;
    
    // Step 1: Set channel enable in register 0x76
    uint8_t chMask = (channel == 1) ? TAC5112_CH_EN_IN1 : TAC5112_CH_EN_IN2;
    if (!modifyRegister(TAC5112_REG_CH_EN, chMask, enable ? chMask : 0x00)) {
        return false;
    }
    
    // Step 2: Ensure ADC block is powered in register 0x78
    if (enable) {
        return modifyRegister(TAC5112_REG_PWR_CFG, TAC5112_PWR_ADC, TAC5112_PWR_ADC);
    }
    return true;
}

// =============================================================================
// DAC/Output Configuration
// Note: The TAC5112 has separate DAC signal path (digital) and OUTx drivers (analog).
//   - DAC_CH1A/1B/2A/2B registers (0x67-0x6A, 0x6E-0x6F) control digital volume/gain
//   - OUT1/OUT2_CFGx registers (0x64-0x66, 0x6B-0x6D) control analog output routing/drivers
// =============================================================================

bool AudioControlTAC5112::setDACOutputConfig(uint8_t channel, TAC5112_OutputConfig config) {
    if (channel < 1 || channel > 2) return false;
    // Output routing config is in OUTx_CFG0 register (0x64/0x6B), bits 4:2
    uint8_t reg = (channel == 1) ? TAC5112_REG_OUT1_CFG0 : TAC5112_REG_OUT2_CFG0;
    return modifyRegister(reg, 0x1C, (uint8_t)config << 2);
}

bool AudioControlTAC5112::enableDACOutput(uint8_t channel, bool enable) {
    if (channel < 1 || channel > 2) return false;

    // Step 1: Set output source in OUTx_CFG0 register (bits 7:5)
    uint8_t cfgReg = (channel == 1) ? TAC5112_REG_OUT1_CFG0 : TAC5112_REG_OUT2_CFG0;
    if (!modifyRegister(cfgReg, 0xE0, enable ? TAC5112_OUT_SRC_DAC : TAC5112_OUT_SRC_RESERVED)) {
        return false;
    }

    // Step 2: Set channel enable in CH_EN register (0x76)
    uint8_t chMask = (channel == 1) ? TAC5112_CH_EN_OUT1 : TAC5112_CH_EN_OUT2;
    if (!modifyRegister(TAC5112_REG_CH_EN, chMask, enable ? chMask : 0x00)) {
        return false;
    }

    // Step 3: Ensure DAC block is powered in PWR_CFG register (0x78)
    if (enable) {
        return modifyRegister(TAC5112_REG_PWR_CFG, TAC5112_PWR_DAC, TAC5112_PWR_DAC);
    }
    return true;
}

bool AudioControlTAC5112::setDACDigitalVolume(uint8_t channel, float volume_dB) {
    if (channel < 1 || channel > 2) return false;

    // DAC Digital Volume Control (DVC) encoding per datasheet Table 8-88:
    // Applies to DAC_CHxA_CFG0 registers (0x67, 0x6E) - digital domain volume
    //   0 = Mute
    //   1 = -100dB
    //   201 = 0dB (default)
    //   255 = +27dB
    // Step size: 0.5dB, Formula: register = (dB × 2) + 201
    // Note: DAC encoding differs from ADC encoding!

    uint8_t regVal;
    if (volume_dB <= -110.0f) {
        regVal = 0x00;  // Mute
    } else {
        // Clamp to valid range
        if (volume_dB < -100.0f) volume_dB = -100.0f;
        if (volume_dB > 27.0f) volume_dB = 27.0f;

        // Convert: reg = (dB × 2) + 201
        int16_t calc = (int16_t)(volume_dB * 2.0f) + 201;
        if (calc < 1) calc = 1;      // Minimum non-mute
        if (calc > 255) calc = 255;  // Maximum
        regVal = (uint8_t)calc;
    }

    uint8_t reg = (channel == 1) ? TAC5112_REG_DAC_CH1A_VOL : TAC5112_REG_DAC_CH2A_VOL;
    return writeRegister(reg, regVal);
}

bool AudioControlTAC5112::setDACChannelGain(uint8_t channel, int8_t gain_dB) {
    if (channel < 1 || channel > 2) return false;

    // OUTxP/OUTxM_LVL_CTRL per datasheet Table 8-89/8-90 and 8-96/8-97:
    // Bits 5:3 of OUT1_CFG1/CFG2 (0x65/0x66) and OUT2_CFG1/CFG2 (0x6C/0x6D)
    // This controls the ANALOG output driver level (not digital DAC volume)
    // 2 = +12dB (bypass only mode)
    // 3 = +6dB (bypass or mix mode)
    // 4 = 0dB (default, all modes)
    // 5 = -6dB (bypass or mix mode)
    // 6 = -12dB (bypass or mix mode with 4.4kΩ input)
    //
    // NOTE: This level control is primarily for ANALOG BYPASS mode.
    // For normal DAC playback digital volume, use setDACDigitalVolume() instead.

    uint8_t lvlCtrl;
    if (gain_dB >= 12) lvlCtrl = 2;
    else if (gain_dB >= 6) lvlCtrl = 3;
    else if (gain_dB >= 0) lvlCtrl = 4;
    else if (gain_dB >= -6) lvlCtrl = 5;
    else lvlCtrl = 6;  // -12dB

    uint8_t regVal = lvlCtrl << 3;  // Bits 5:3

    // Apply to both OUTP (CFG1) and OUTM (CFG2) for the channel
    uint8_t regP = (channel == 1) ? TAC5112_REG_OUT1_CFG1 : TAC5112_REG_OUT2_CFG1;
    uint8_t regM = (channel == 1) ? TAC5112_REG_OUT1_CFG2 : TAC5112_REG_OUT2_CFG2;

    bool ok = modifyRegister(regP, 0x38, regVal);  // Mask 0x38 = bits 5:3
    ok &= modifyRegister(regM, 0x38, regVal);
    return ok;
}

bool AudioControlTAC5112::setDACFineGainCalibration(uint8_t channel, float gain_dB) {
    if (channel < 1 || channel > 2) return false;

    // DAC Fine gain calibration per datasheet Table 8-90:
    // Applies to DAC_CHxA_CFG1 registers (0x68, 0x6F), bits 7:4
    // Range: -0.8dB to +0.7dB in 0.1dB steps
    // 0 = -0.8dB, 8 = 0dB (default), 15 = +0.7dB
    // Formula: reg = (gain_dB × 10) + 8
    //
    // This is for CHANNEL MATCHING CALIBRATION only!
    // For volume control, use setDACDigitalVolume() which has -100dB to +27dB range.

    // Clamp to valid range
    if (gain_dB < -0.8f) gain_dB = -0.8f;
    if (gain_dB > 0.7f) gain_dB = 0.7f;

    // Convert: reg = (gain × 10) + 8
    int16_t calc = (int16_t)(gain_dB * 10.0f) + 8;
    if (calc < 0) calc = 0;
    if (calc > 15) calc = 15;
    uint8_t regVal = (uint8_t)calc << 4;  // Bits 7:4

    uint8_t reg = (channel == 1) ? TAC5112_REG_DAC_CH1A_FGAIN : TAC5112_REG_DAC_CH2A_FGAIN;
    return modifyRegister(reg, 0xF0, regVal);
}

// =============================================================================
// MICBIAS Control
// =============================================================================

bool AudioControlTAC5112::enableMicBias(bool enable) {
    return modifyRegister(TAC5112_REG_PWR_CFG, TAC5112_PWR_MICBIAS, 
                          enable ? TAC5112_PWR_MICBIAS : 0x00);
}

bool AudioControlTAC5112::setMicBiasVoltage(uint8_t setting) {
    // 0 = VREF, 1 = VREF/2, 3 = AVDD
    return modifyRegister(TAC5112_REG_VREF_CFG, 0x0C, (setting & 0x03) << 2);
}

// =============================================================================
// Reference Voltage
// =============================================================================

bool AudioControlTAC5112::setVREF(uint8_t setting) {
    // 0 = 2.75V, 1 = 2.5V, 2 = 1.375V
    return modifyRegister(TAC5112_REG_VREF_CFG, 0x03, setting & 0x03);
}

// =============================================================================
// Volume Control (convenience functions matching Teensy Audio Library style)
// =============================================================================

bool AudioControlTAC5112::inputLevel(float level) {
    // Map 0.0-1.0 to full ADC digital volume range
    // ADC DVC range: -80dB to +47dB (127dB total)
    // 0.0 = mute, ~0.63 = 0dB (unity), 1.0 = +47dB (max boost)
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    
    float volume_dB;
    if (level <= 0.001f) {
        volume_dB = -100.0f;  // Will trigger mute
    } else {
        // Map 0.0-1.0 to -80dB to +47dB range (127dB span)
        volume_dB = -80.0f + (level * 127.0f);
    }
    
    bool ok = setADCDigitalVolume(1, volume_dB);
    ok &= setADCDigitalVolume(2, volume_dB);
    return ok;
}

bool AudioControlTAC5112::outputLevel(float level) {
    // Map 0.0-1.0 to full DAC digital volume range
    // DAC DVC range: -100dB to +27dB (127dB total)
    // 0.0 = mute, ~0.79 = 0dB (unity), 1.0 = +27dB (max boost)
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    
    float volume_dB;
    if (level <= 0.001f) {
        volume_dB = -110.0f;  // Will trigger mute
    } else {
        // Map 0.0-1.0 to -100dB to +27dB range (127dB span)
        volume_dB = -100.0f + (level * 127.0f);
    }
    
    bool ok = setDACDigitalVolume(1, volume_dB);
    ok &= setDACDigitalVolume(2, volume_dB);
    return ok;
}

bool AudioControlTAC5112::volume(float level) {
    return outputLevel(level);
}

// =============================================================================
// Mute Control
// =============================================================================

bool AudioControlTAC5112::muteInput(bool mute) {
    // ADC mute: register value 0 = mute, 161 = 0dB
    // Write directly to volume registers for true mute
    uint8_t regVal = mute ? 0x00 : 0xA1;  // 0=mute, 0xA1=161=0dB
    bool ok = writeRegister(TAC5112_REG_ADC_CH1_VOL, regVal);
    ok &= writeRegister(TAC5112_REG_ADC_CH2_VOL, regVal);
    return ok;
}

bool AudioControlTAC5112::muteOutput(bool mute) {
    // DAC mute: register value 0 = mute, 201 = 0dB
    // Write directly to DAC digital volume registers for true mute
    uint8_t regVal = mute ? 0x00 : 0xC9;  // 0=mute, 0xC9=201=0dB
    bool ok = writeRegister(TAC5112_REG_DAC_CH1A_VOL, regVal);
    ok &= writeRegister(TAC5112_REG_DAC_CH2A_VOL, regVal);
    return ok;
}

// =============================================================================
// Status Functions
// =============================================================================

bool AudioControlTAC5112::isClockValid(void) {
    // Read DEV_STS1 (0x7A) to check PLL status
    // Bit 4 (PLL_STS): 0 = PLL not enabled, 1 = PLL enabled
    // Bits 7:5 (MODE_STS): 7 = active with channels on, 6 = active channels off, 4 = sleep
    uint8_t status = readRegister(TAC5112_REG_DEV_STS1);
    
    // Check if PLL is enabled (bit 4)
    return (status & 0x10) != 0;
}

bool AudioControlTAC5112::isDeviceActive(void) {
    // Read DEV_STS1 (0x7A) to check device mode
    // Bits 7:5 (MODE_STS): 7 = active with channels, 6 = active no channels, 4 = sleep
    uint8_t status = readRegister(TAC5112_REG_DEV_STS1);
    uint8_t mode = (status >> 5) & 0x07;
    return (mode == 6 || mode == 7);  // Active modes
}

uint8_t AudioControlTAC5112::getDetectedSampleRate(void) {
    // CLK_DET_STS0 (0x3E): bits 7:2 = PASI sample rate code
    // See Table 8-59 for decoding (e.g., 20 = 48kHz, 25 = 24000Hz)
    return readRegister(TAC5112_REG_CLK_DET_STS0);
}

uint8_t AudioControlTAC5112::getDetectedBclkRatio(void) {
    // CLK_DET_STS2/STS3 contain FSYNC to clock source ratio
    // This returns the MSB portion
    return readRegister(TAC5112_REG_CLK_DET_STS2);
}

// =============================================================================
// Low-level Register Access
// =============================================================================

bool AudioControlTAC5112::setPage(uint8_t page) {
    if (_currentPage == page) return true;
    
    _wire->beginTransmission(_i2cAddr);
    _wire->write(TAC5112_REG_PAGE_SELECT);
    _wire->write(page);
    if (_wire->endTransmission() != 0) {
        return false;
    }
    _currentPage = page;
    return true;
}

bool AudioControlTAC5112::writeRegister(uint8_t reg, uint8_t value) {
    // Ensure we're on page 0 for these registers
    if (!setPage(0)) return false;
    return writeRegisterDirect(reg, value);
}

bool AudioControlTAC5112::writeRegisterDirect(uint8_t reg, uint8_t value) {
    _wire->beginTransmission(_i2cAddr);
    _wire->write(reg);
    _wire->write(value);
    return (_wire->endTransmission() == 0);
}

uint8_t AudioControlTAC5112::readRegister(uint8_t reg) {
    // Ensure we're on page 0 for these registers
    setPage(0);
    return readRegisterDirect(reg);
}

uint8_t AudioControlTAC5112::readRegisterDirect(uint8_t reg) {
    _wire->beginTransmission(_i2cAddr);
    _wire->write(reg);
    _wire->endTransmission(false);  // Repeated start
    
    _wire->requestFrom(_i2cAddr, (uint8_t)1);
    if (_wire->available()) {
        return _wire->read();
    }
    return 0x00;
}

bool AudioControlTAC5112::modifyRegister(uint8_t reg, uint8_t mask, uint8_t value) {
    uint8_t current = readRegister(reg);
    current = (current & ~mask) | (value & mask);
    return writeRegister(reg, current);
}
