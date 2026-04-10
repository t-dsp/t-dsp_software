/*
 * TAC5112 Control Library for Teensy Audio
 * 
 * Texas Instruments TAC5112 Low-Power Stereo Audio Codec
 * Control via I2C interface
 * 
 * Compatible with:
 *   - Teensy Audio Library I2S interface
 * 
 * The TAC5112 is register-compatible with TAC5212/TAC5211/TAC5111 family
 */

#ifndef control_TAC5112_h_
#define control_TAC5112_h_

#include <Arduino.h>
#include <Wire.h>

// =============================================================================
// I2C Address Configuration (7-bit addresses)
// The TAC5112 ADDR pin determines the I2C address via resistor network:
//   ADDR = Short to Ground:          0x50  (8-bit: 0xA0)
//   ADDR = Pull down 4.7kΩ to GND:   0x51  (8-bit: 0xA2)
//   ADDR = Pull up 22kΩ to AVDD:     0x52  (8-bit: 0xA4)
//   ADDR = Pull up 4.7kΩ to AVDD:    0x53  (8-bit: 0xA6)
// =============================================================================
#define TAC5112_I2C_ADDR_GND        0x50    // ADDR pin shorted to ground
#define TAC5112_I2C_ADDR_PD_4K7     0x51    // ADDR with 4.7kΩ pulldown to GND
#define TAC5112_I2C_ADDR_PU_22K     0x52    // ADDR with 22kΩ pullup to AVDD
#define TAC5112_I2C_ADDR_PU_4K7     0x53    // ADDR with 4.7kΩ pullup to AVDD
#define TAC5112_I2C_ADDR_DEFAULT    TAC5112_I2C_ADDR_GND

// =============================================================================
// Page 0 Register Addresses
// =============================================================================

// Software Reset and Sleep Control
#define TAC5112_REG_SW_RESET        0x01    // Software reset
#define TAC5112_REG_SLEEP_CFG       0x02    // Sleep/wake configuration

// Supply Voltage Status
#define TAC5112_REG_AVDD_IOVDD_STS  0x03    // AVDD/IOVDD supply status

// Misc Configuration
#define TAC5112_REG_MISC_CFG        0x04    // Misc configuration
#define TAC5112_REG_MISC_CFG1       0x05    // Misc configuration 1
#define TAC5112_REG_DAC_CFG_A0      0x06    // DAC configuration A0
#define TAC5112_REG_MISC_CFG0       0x07    // Misc configuration 0

// Device Status (read-only)
#define TAC5112_REG_DEV_STS0        0x79    // Device status 0 (channel power status)
#define TAC5112_REG_DEV_STS1        0x7A    // Device status 1 (mode, PLL, MICBIAS status)

// Interrupt Configuration (Page 0)
#define TAC5112_REG_INT_CFG         0x42    // Interrupt configuration

// GPIO Configuration
#define TAC5112_REG_GPIO1_IO_CFG    0x0A    // GPIO1 I/O configuration
#define TAC5112_REG_GPIO2_IO_CFG    0x0B    // GPIO2 I/O configuration
#define TAC5112_REG_GPO1_IO_CFG     0x0C    // GPO1 I/O configuration
#define TAC5112_REG_GPI1_IO_CFG     0x0D    // GPI1 I/O configuration

// Clock and Data Output Configuration (INTF registers)
#define TAC5112_REG_INTF_CFG0       0xF    // Interface config 0 (bus keeper, FSYNC/BCLK dir)
#define TAC5112_REG_INTF_CFG1       0x10    // Interface config 1 (DOUT config)
#define TAC5112_REG_INTF_CFG2       0x11    // Interface config 2 (DIN config)
#define TAC5112_REG_INTF_CFG3       0x12    // Interface config 3 (DOUT2 function)
#define TAC5112_REG_INTF_CFG4       0x13    // Interface config 4 (GPIO/BCLK/FSYNC output)
#define TAC5112_REG_INTF_CFG5       0x14    // Interface config 5 (BCLK ratio)
#define TAC5112_REG_INTF_CFG6       0x15    // Interface config 6 (more config)

// ASI (Audio Serial Interface) Configuration
#define TAC5112_REG_ASI_CFG0        0x18    // ASI config 0 (PASI/SASI enable, daisy)
#define TAC5112_REG_ASI_CFG1        0x19    // ASI config 1 (DOUT/DIN lane config)
#define TAC5112_REG_PASI_CFG0       0x1A    // PASI config 0 (format, word length, polarity)
#define TAC5112_REG_PASI_TX_CFG0    0x1B    // PASI TX config 0 (edge, fill, LSB)
#define TAC5112_REG_PASI_TX_CFG1    0x1C    // PASI TX config 1 (slot size, keeper)
#define TAC5112_REG_PASI_TX_CFG2    0x1D    // PASI TX config 2 (TX offset)

// ASI Transmit Channel Slot Assignment
#define TAC5112_REG_TX_CH1_SLOT     0x1E    // TX channel 1 slot config
#define TAC5112_REG_TX_CH2_SLOT     0x1F    // TX channel 2 slot config
#define TAC5112_REG_TX_CH3_SLOT     0x20    // TX channel 3 slot config
#define TAC5112_REG_TX_CH4_SLOT     0x21    // TX channel 4 slot config
#define TAC5112_REG_TX_CH5_SLOT     0x22    // TX channel 5 slot config
#define TAC5112_REG_TX_CH6_SLOT     0x23    // TX channel 6 slot config
#define TAC5112_REG_TX_CH7_SLOT     0x24    // TX channel 7 slot config
#define TAC5112_REG_TX_CH8_SLOT     0x25    // TX channel 8 slot config

// ASI Receive Configuration
#define TAC5112_REG_PASI_RX_CFG0    0x26    // PASI RX config 0 (edge, etc)
#define TAC5112_REG_PASI_RX_CFG1    0x27    // PASI RX config 1 (RX offset)
#define TAC5112_REG_RX_CH1_SLOT     0x28    // RX channel 1 slot config
#define TAC5112_REG_RX_CH2_SLOT     0x29    // RX channel 2 slot config
#define TAC5112_REG_RX_CH3_SLOT     0x2A    // RX channel 3 slot config
#define TAC5112_REG_RX_CH4_SLOT     0x2B    // RX channel 4 slot config
#define TAC5112_REG_RX_CH5_SLOT     0x2C    // RX channel 5 slot config
#define TAC5112_REG_RX_CH6_SLOT     0x2D    // RX channel 6 slot config
#define TAC5112_REG_RX_CH7_SLOT     0x2E    // RX channel 7 slot config
#define TAC5112_REG_RX_CH8_SLOT     0x2F    // RX channel 8 slot config    
 
// GPIO Configuration
#define TAC5112_REG_GPIO1_CFG       0x0A    // GPIO1 configuration
#define TAC5112_REG_GPIO2_CFG       0x0B    // GPIO2 configuration
#define TAC5112_REG_GPO1_CFG        0x0C    // GPO1 configuration
#define TAC5112_REG_GPI_CFG         0x0D    // GPI configuration
#define TAC5112_REG_GPO_GPI_VAL     0x0E    // GPO/GPI value register

// Clock Detection Results (read-only)
// Clock Detection Status (read-only)
#define TAC5112_REG_CLK_DET_STS0    0x3E    // PASI sample rate + PLL mode status
#define TAC5112_REG_CLK_DET_STS1    0x3F    // SASI sample rate status
#define TAC5112_REG_CLK_DET_STS2    0x40    // FSYNC to clock source ratio MSB
#define TAC5112_REG_CLK_DET_STS3    0x41    // FSYNC to clock source ratio LSB

// Custom Clock Configuration
#define TAC5112_REG_CUSTOM_CLK      0x32    // Custom clock configuration

// Reference and MICBIAS
#define TAC5112_REG_VREF_CFG        0x4D    // VREF and MICBIAS configuration

// Power Control
#define TAC5112_REG_PWR_CFG         0x78    // Power configuration

// ADC Channel 1 Configuration
#define TAC5112_REG_ADC_CH1_CFG0    0x50    // ADC channel 1 config 0 (input source, impedance, coupling)
#define TAC5112_REG_IADC_CH_CFG     0x51    // IADC mode configuration (shared, NOT channel gain)
#define TAC5112_REG_ADC_CH1_CFG2    0x52    // ADC channel 1 config 2 (digital volume)
#define TAC5112_REG_ADC_CH1_VOL     0x52    // ADC channel 1 digital volume (same as CFG2)
#define TAC5112_REG_ADC_CH1_CFG3    0x53    // ADC channel 1 config 3 (fine gain calibration ±0.8dB)
#define TAC5112_REG_ADC_CH1_CFG4    0x54    // ADC channel 1 config 4 (phase calibration)

// ADC Channel 2 Configuration  
#define TAC5112_REG_ADC_CH2_CFG0    0x55    // ADC channel 2 config 0 (input source, impedance, coupling)
#define TAC5112_REG_ADC_CH2_CFG2    0x57    // ADC channel 2 config 2 (digital volume)
#define TAC5112_REG_ADC_CH2_VOL     0x57    // ADC channel 2 digital volume (same as CFG2)
#define TAC5112_REG_ADC_CH2_CFG3    0x58    // ADC channel 2 config 3 (fine gain calibration ±0.8dB)
#define TAC5112_REG_ADC_CH2_CFG4    0x59    // ADC channel 2 config 4 (phase calibration)

// Output Channel 1 Configuration (OUT1x)
// Note: These control the analog output drivers, not the DAC digital signal path directly
#define TAC5112_REG_OUT1_CFG0       0x64    // OUT1x source routing and output mode config
#define TAC5112_REG_OUT1_CFG1       0x65    // OUT1P driver config (impedance, level, bypass)
#define TAC5112_REG_OUT1_CFG2       0x66    // OUT1M driver config (impedance, level, bypass)
#define TAC5112_REG_DAC_CH1A_VOL    0x67    // DAC channel 1A digital volume control
#define TAC5112_REG_DAC_CH1A_FGAIN  0x68    // DAC channel 1A fine gain calibration
#define TAC5112_REG_DAC_CH1B_VOL    0x69    // DAC channel 1B digital volume control
#define TAC5112_REG_DAC_CH1B_FGAIN  0x6A    // DAC channel 1B fine gain calibration

// Output Channel 2 Configuration (OUT2x)
// Note: These control the analog output drivers, not the DAC digital signal path directly
#define TAC5112_REG_OUT2_CFG0       0x6B    // OUT2x source routing and output mode config
#define TAC5112_REG_OUT2_CFG1       0x6C    // OUT2P driver config (impedance, level, bypass)
#define TAC5112_REG_OUT2_CFG2       0x6D    // OUT2M driver config (impedance, level, bypass)
#define TAC5112_REG_DAC_CH2A_VOL    0x6E    // DAC channel 2A digital volume control
#define TAC5112_REG_DAC_CH2A_FGAIN  0x6F    // DAC channel 2A fine gain calibration

// Legacy aliases for backward compatibility (DEPRECATED - use OUT1/OUT2 names above)
#define TAC5112_REG_DAC_CH1_CFG0    TAC5112_REG_OUT1_CFG0
#define TAC5112_REG_DAC_CH1_OUTP    TAC5112_REG_OUT1_CFG1
#define TAC5112_REG_DAC_CH1_OUTM    TAC5112_REG_OUT1_CFG2
#define TAC5112_REG_DAC_CH1_VOL     TAC5112_REG_DAC_CH1A_VOL
#define TAC5112_REG_DAC_CH1_FGAIN   TAC5112_REG_DAC_CH1A_FGAIN
#define TAC5112_REG_DAC_CH2_CFG0    TAC5112_REG_OUT2_CFG0
#define TAC5112_REG_DAC_CH2_OUTP    TAC5112_REG_OUT2_CFG1
#define TAC5112_REG_DAC_CH2_OUTM    TAC5112_REG_OUT2_CFG2
#define TAC5112_REG_DAC_CH2_VOL     TAC5112_REG_DAC_CH2A_VOL
#define TAC5112_REG_DAC_CH2_FGAIN   TAC5112_REG_DAC_CH2A_FGAIN

// Channel Enable
#define TAC5112_REG_CH_EN           0x76    // Input/Output channel enable

// Page selection (write 0x00 to access page 0, etc.)
#define TAC5112_REG_PAGE_SELECT     0x00    // Page select register

// =============================================================================
// Register Bit Definitions
// =============================================================================

// PASI_CFG0 (0x1A) - Primary ASI Format, Word Length, Polarity
#define TAC5112_ASI_FORMAT_TDM      (0x00 << 6)  // Bits 7:6 = 00: TDM mode
#define TAC5112_ASI_FORMAT_I2S      (0x01 << 6)  // Bits 7:6 = 01: I2S mode
#define TAC5112_ASI_FORMAT_LJ       (0x02 << 6)  // Bits 7:6 = 10: Left-justified mode
#define TAC5112_ASI_FORMAT_MASK     (0x03 << 6)

#define TAC5112_ASI_WLEN_16BIT      (0x00 << 4)  // Bits 5:4 = 00: 16 bits
#define TAC5112_ASI_WLEN_20BIT      (0x01 << 4)  // Bits 5:4 = 01: 20 bits
#define TAC5112_ASI_WLEN_24BIT      (0x02 << 4)  // Bits 5:4 = 02: 24 bits
#define TAC5112_ASI_WLEN_32BIT      (0x03 << 4)  // Bits 5:4 = 03: 32 bits
#define TAC5112_ASI_WLEN_MASK       (0x03 << 4)

// PASI_CFG0 (0x1A) - Polarity bits (also in same register)
#define TAC5112_FSYNC_POL_NORMAL    (0x00 << 3)  // Bit 3 = 0: Default polarity
#define TAC5112_FSYNC_POL_INVERTED  (0x01 << 3)  // Bit 3 = 1: Inverted polarity
#define TAC5112_BCLK_POL_NORMAL     (0x00 << 2)  // Bit 2 = 0: Default polarity
#define TAC5112_BCLK_POL_INVERTED   (0x01 << 2)  // Bit 2 = 1: Inverted polarity

// PASI_CFG0 (0x1A) - Bus error bits
#define TAC5112_BUS_ERR_ENABLE      (0x00 << 1)  // Bit 1 = 0: Enable bus error detection
#define TAC5112_BUS_ERR_DISABLE     (0x01 << 1)  // Bit 1 = 1: Disable bus error detection

// VREF_CFG (0x4D) - Reference Voltage and MICBIAS
#define TAC5112_VREF_2V75           (0x00 << 0)  // 2.75V (default, for 3.0-3.6V AVDD)
#define TAC5112_VREF_2V5            (0x01 << 0)  // 2.5V (for 2.8-3.6V AVDD)
#define TAC5112_VREF_1V375          (0x02 << 0)  // 1.375V (for 1.7-1.9V AVDD)

#define TAC5112_MICBIAS_VREF        (0x00 << 2)  // MICBIAS = VREF
#define TAC5112_MICBIAS_VREF_HALF   (0x01 << 2)  // MICBIAS = 0.5 × VREF
#define TAC5112_MICBIAS_AVDD        (0x03 << 2)  // MICBIAS = AVDD

// PWR_CFG (0x78) - Power Configuration
// Based on TI examples: 0xA0 = ADC+MICBIAS, 0x40 = DAC, 0xE0 = all
#define TAC5112_PWR_MICBIAS         (0x01 << 5)  // Bit 5: MICBIAS power (0=off, 1=on)
#define TAC5112_PWR_DAC             (0x01 << 6)  // Bit 6: DAC block power (0=off, 1=on)
#define TAC5112_PWR_ADC             (0x01 << 7)  // Bit 7: ADC block power (0=off, 1=on)

// Channel Enable (0x76) - per Table 8-104
#define TAC5112_CH_EN_IN1           (0x01 << 7)  // Bit 7: Input channel 1 enable
#define TAC5112_CH_EN_IN2           (0x01 << 6)  // Bit 6: Input channel 2 enable
#define TAC5112_CH_EN_IN3           (0x01 << 5)  // Bit 5: Input channel 3 enable
#define TAC5112_CH_EN_IN4           (0x01 << 4)  // Bit 4: Input channel 4 enable
#define TAC5112_CH_EN_OUT1          (0x01 << 3)  // Bit 3: Output channel 1 enable
#define TAC5112_CH_EN_OUT2          (0x01 << 2)  // Bit 2: Output channel 2 enable
#define TAC5112_CH_EN_OUT3          (0x01 << 1)  // Bit 1: Output channel 3 enable
#define TAC5112_CH_EN_OUT4          (0x01 << 0)  // Bit 0: Output channel 4 enable

// ADC Input Source Configuration
#define TAC5112_ADC_INSRC_DIFF      (0x00 << 6)  // Differential input
#define TAC5112_ADC_INSRC_SE_2PIN   (0x01 << 6)  // Single-ended (signal + ground pins)
#define TAC5112_ADC_INSRC_SE_INP    (0x02 << 6)  // Single-ended on INxP only
#define TAC5112_ADC_INSRC_SE_INM    (0x03 << 6)  // Single-ended on INxM only

// ADC Input Impedance
#define TAC5112_ADC_IMP_5K          (0x00 << 4)  // 5kΩ (default)
#define TAC5112_ADC_IMP_10K         (0x01 << 4)  // 10kΩ
#define TAC5112_ADC_IMP_40K         (0x02 << 4)  // 40kΩ

// ADC Common Mode Tolerance (AC/DC coupling)
#define TAC5112_ADC_CM_AC_COUPLED   (0x00 << 2)  // AC-coupled input (best performance)
#define TAC5112_ADC_CM_DC_COUPLED   (0x02 << 2)  // DC-coupled, high CM tolerance

// OUTx_CFG0 (0x64, 0x6B) - Output Source and Routing Configuration
// Bits 7:5 - OUTx_SRC: Output source selection
#define TAC5112_OUT_SRC_RESERVED    (0x00 << 5)  // Reserved - don't use
#define TAC5112_OUT_SRC_DAC         (0x01 << 5)  // Input from DAC signal chain
#define TAC5112_OUT_SRC_BYPASS      (0x02 << 5)  // Input from analog bypass path
#define TAC5112_OUT_SRC_MIX         (0x03 << 5)  // Input from DAC + analog bypass mixed
#define TAC5112_OUT_SRC_INDEP_DP    (0x04 << 5)  // Independent: DAC->OUTxP, INxP->OUTxM
#define TAC5112_OUT_SRC_INDEP_MD    (0x05 << 5)  // Independent: INxM->OUTxP, DAC->OUTxM
#define TAC5112_OUT_SRC_MASK        (0x07 << 5)

// Bits 4:2 - OUTx_CFG: DAC/Analog bypass routing configuration
#define TAC5112_OUT_CFG_DIFF        (0x00 << 2)  // Differential (DACxAP+DACxBP->OUTxP, DACxAM+DACxBM->OUTxM)
#define TAC5112_OUT_CFG_SE_STEREO   (0x01 << 2)  // Stereo SE (DACxA->OUTxP, DACxB->OUTxM)
#define TAC5112_OUT_CFG_SE_OUTP     (0x02 << 2)  // Mono SE on OUTxP only (DACxA+DACxB->OUTxP)
#define TAC5112_OUT_CFG_SE_OUTM     (0x03 << 2)  // Mono SE on OUTxM only (DACxA+DACxB->OUTxM)
#define TAC5112_OUT_CFG_PSEUDO_P    (0x04 << 2)  // Pseudo-diff: DACxA,DACxB->OUTxP, VCOM->OUTxM
#define TAC5112_OUT_CFG_PSEUDO_P_S  (0x05 << 2)  // Pseudo-diff with OUT2M sensing (OUT1 only)
#define TAC5112_OUT_CFG_PSEUDO_M    (0x06 << 2)  // Pseudo-diff: INxP->OUTxM, VCOM->OUTxP
#define TAC5112_OUT_CFG_MASK        (0x07 << 2)

// Bit 1 - OUTx_VCOM: VCOM voltage selection
#define TAC5112_OUT_VCOM_0P6VREF    (0x00 << 1)  // VCOM = 0.6 × VREF (0.654×VREF for 1.375V mode)
#define TAC5112_OUT_VCOM_AVDD_2     (0x01 << 1)  // VCOM = AVDD / 2

// OUTx_CFG1/CFG2 (0x65-0x66, 0x6C-0x6D) - Driver Configuration
// Bits 7:6 - OUTxP/OUTxM_DRIVE: Driver strength/impedance
#define TAC5112_OUT_DRV_LINE        (0x00 << 6)  // Line output (min 300Ω SE impedance)
#define TAC5112_OUT_DRV_HP          (0x01 << 6)  // Headphone driver (min 16Ω SE impedance)
#define TAC5112_OUT_DRV_4OHM        (0x02 << 6)  // High current (min 4Ω SE impedance)
#define TAC5112_OUT_DRV_HSNR        (0x03 << 6)  // High SNR/DR for FD receiver loads
#define TAC5112_OUT_DRV_MASK        (0x03 << 6)

// Bits 5:3 - OUTxP/OUTxM_LVL_CTRL: Analog level control (primarily for bypass mode)
#define TAC5112_OUT_LVL_12DB        (0x02 << 3)  // +12dB (bypass only mode)
#define TAC5112_OUT_LVL_6DB         (0x03 << 3)  // +6dB (bypass or mix mode)
#define TAC5112_OUT_LVL_0DB         (0x04 << 3)  // 0dB (all modes, default)
#define TAC5112_OUT_LVL_M6DB        (0x05 << 3)  // -6dB (bypass or mix mode)
#define TAC5112_OUT_LVL_M12DB       (0x06 << 3)  // -12dB (bypass or mix with 4.4kΩ)
#define TAC5112_OUT_LVL_MASK        (0x07 << 3)

// Bit 2 - AINxM/AINxP_BYP_IMP: Analog bypass input impedance
#define TAC5112_BYP_IMP_4K4         (0x00 << 2)  // 4.4kΩ bypass input impedance
#define TAC5112_BYP_IMP_20K         (0x01 << 2)  // 20kΩ bypass input impedance

// Bit 1 - AINx_BYP_CFG: Bypass input configuration (CFG1/CFG2 only)
#define TAC5112_BYP_CFG_DIFF        (0x00 << 1)  // Fully-differential or pseudo-diff
#define TAC5112_BYP_CFG_SE          (0x01 << 1)  // Single-ended

// Bit 0 - DAC_CHx_BW_MODE: DAC bandwidth mode (CFG1 only)
#define TAC5112_DAC_BW_AUDIO        (0x00 << 0)  // Audio bandwidth (24kHz mode)
#define TAC5112_DAC_BW_WIDE         (0x01 << 0)  // Wide bandwidth (96kHz mode)

// Bit 0 - DAC_CHx_CM_TOL: DAC input coupling (CFG2 only)
#define TAC5112_DAC_CM_AC           (0x00 << 0)  // AC-coupled input
#define TAC5112_DAC_CM_DC           (0x01 << 0)  // AC/DC-coupled input

// Legacy aliases for backward compatibility (DEPRECATED)
#define TAC5112_DAC_SRC_OFF         TAC5112_OUT_SRC_RESERVED
#define TAC5112_DAC_SRC_DAC         TAC5112_OUT_SRC_DAC
#define TAC5112_DAC_SRC_BYPASS      TAC5112_OUT_SRC_BYPASS
#define TAC5112_DAC_SRC_MIX         TAC5112_OUT_SRC_MIX
#define TAC5112_DAC_CFG_DIFF        TAC5112_OUT_CFG_DIFF
#define TAC5112_DAC_CFG_SE_INDEP    TAC5112_OUT_CFG_SE_STEREO
#define TAC5112_DAC_CFG_SE_OUTP     TAC5112_OUT_CFG_SE_OUTP
#define TAC5112_DAC_CFG_SE_OUTM     TAC5112_OUT_CFG_SE_OUTM
#define TAC5112_DAC_CFG_PSEUDO_P    TAC5112_OUT_CFG_PSEUDO_P
#define TAC5112_DAC_CFG_PSEUDO_M    TAC5112_OUT_CFG_PSEUDO_M

// =============================================================================
// Enumerations
// =============================================================================

enum class TAC5112_SampleRate {
    RATE_8K    = 8000,
    RATE_16K   = 16000,
    RATE_24K   = 24000,
    RATE_32K   = 32000,
    RATE_44K1  = 44100,
    RATE_48K   = 48000,
    RATE_88K2  = 88200,
    RATE_96K   = 96000,
    RATE_176K4 = 176400,
    RATE_192K  = 192000
};

enum class TAC5112_ASIFormat {
    TDM = 0,
    I2S = 1,
    LEFT_JUSTIFIED = 2
};

enum class TAC5112_WordLength {
    BITS_16 = 0,
    BITS_20 = 1,
    BITS_24 = 2,
    BITS_32 = 3
};

enum class TAC5112_InputSource {
    DIFFERENTIAL = 0,
    SINGLE_ENDED_2PIN = 1,
    SINGLE_ENDED_INP = 2,
    SINGLE_ENDED_INM = 3
};

enum class TAC5112_InputImpedance {
    IMP_5K = 0,
    IMP_10K = 1,
    IMP_40K = 2
};

enum class TAC5112_OutputConfig {
    DIFFERENTIAL = 0,
    SINGLE_ENDED_INDEPENDENT = 1,
    SINGLE_ENDED_OUTP = 2,
    SINGLE_ENDED_OUTM = 3,
    PSEUDO_DIFF_P = 4,
    PSEUDO_DIFF_M = 6
};

// =============================================================================
// TAC5112 Control Class
// =============================================================================

class AudioControlTAC5112 {
public:
    // Constructor
    AudioControlTAC5112(uint8_t i2cAddr = TAC5112_I2C_ADDR_DEFAULT);
    
    // Initialization
    bool begin(void);
    bool begin(TwoWire *wire);
    
    // Configure for Teensy Audio Library I2S (recommended starting point)
    bool configureForTeensyI2S(void);
    
    // Software Reset
    bool reset(void);
    
    // ASI (Audio Serial Interface) Configuration
    bool setASIFormat(TAC5112_ASIFormat format);
    bool setWordLength(TAC5112_WordLength wlen);
    bool setFsyncPolarity(bool inverted);
    bool setBclkPolarity(bool inverted);
    bool setTxOffset(uint8_t offset);
    bool setRxOffset(uint8_t offset);
    
    // Slot Assignment
    bool setTxSlot(uint8_t channel, uint8_t slot);  // channel 1-2, slot 0-31
    bool setRxSlot(uint8_t channel, uint8_t slot);  // channel 1-2, slot 0-31
    
    // ADC Configuration
    bool setADCInputSource(uint8_t channel, TAC5112_InputSource source);
    bool setADCInputImpedance(uint8_t channel, TAC5112_InputImpedance impedance);
    bool setADCACCoupled(uint8_t channel, bool acCoupled);
    bool setADCDigitalVolume(uint8_t channel, float volume_dB);  // -80 to +47 dB, 0.5dB steps
    bool setADCFineGainCalibration(uint8_t channel, float gain_dB); // Channel matching: -0.8 to +0.7 dB
    bool enableADC(uint8_t channel, bool enable);
    
    // DAC Configuration
    bool setDACOutputConfig(uint8_t channel, TAC5112_OutputConfig config);
    bool enableDACOutput(uint8_t channel, bool enable);
    bool setDACDigitalVolume(uint8_t channel, float volume_dB);  // -100 to +27 dB, 0.5dB steps
    bool setDACChannelGain(uint8_t channel, int8_t gain_dB);      // Analog bypass level: -12 to +12 dB (6dB steps)
    bool setDACFineGainCalibration(uint8_t channel, float gain_dB); // Channel matching: -0.8 to +0.7 dB
    
    // MICBIAS Control
    bool enableMicBias(bool enable);
    bool setMicBiasVoltage(uint8_t setting);  // 0=VREF, 1=VREF/2, 3=AVDD
    
    // Reference Voltage
    bool setVREF(uint8_t setting);  // 0=2.75V, 1=2.5V, 2=1.375V
    
    // Volume Control (convenience functions)
    bool inputLevel(float level);   // 0.0-1.0 maps to -80dB to +47dB (0.63 ≈ 0dB)
    bool outputLevel(float level);  // 0.0-1.0 maps to -100dB to +27dB (0.79 ≈ 0dB)
    bool volume(float level);       // Alias for outputLevel
    
    // Mute Control
    bool muteInput(bool mute);
    bool muteOutput(bool mute);
    
    // Status
    bool isClockValid(void);
    bool isDeviceActive(void);
    uint8_t getDetectedSampleRate(void);
    uint8_t getDetectedBclkRatio(void);
    
    // Low-level Register Access
    bool writeRegister(uint8_t reg, uint8_t value);
    uint8_t readRegister(uint8_t reg);
    bool modifyRegister(uint8_t reg, uint8_t mask, uint8_t value);
    bool setPage(uint8_t page);
    
private:
    TwoWire *_wire;
    uint8_t _i2cAddr;
    uint8_t _currentPage;
    bool _initialized;
    
    // Internal helpers
    bool writeRegisterDirect(uint8_t reg, uint8_t value);
    uint8_t readRegisterDirect(uint8_t reg);
};

#endif // control_TAC5112_h_
