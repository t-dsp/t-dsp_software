/*
 * AudioOutputTDM_F32.cpp
 *
 * Float32 -> 24-bit packed TDM output for Teensy 4.1 (i.MX RT1062, SAI1).
 *
 * What changed vs. PJRC's AudioOutputTDM:
 *   - Class inherits AudioStream_F32 (8 F32 input ports)
 *   - update() pulls audio_block_f32_t* via receiveReadOnly_f32()
 *   - isr() runs arm_float_to_q31() per channel, then writes the resulting
 *     int32 directly into the 32-bit TDM slot. arm_float_to_q31 saturates
 *     inputs outside [-1.0, +1.0) and produces a value whose UPPER 24 BITS
 *     are signed 24-bit audio -- exactly what a 32-slot/24-data left-justified
 *     codec (TAC5212) reads. The lower 8 bits become extra LSBs that the
 *     codec discards in 24-bit mode (effectively free dither).
 *
 * What did NOT change:
 *   - SAI1 register configuration (config_tdm) is byte-identical to the
 *     stock driver. The stock driver already sends 32 bits per slot; only
 *     the data we pack into those bits is different.
 *   - DMA descriptor, ping-pong buffer layout, half-buffer ISR pattern.
 */

#include <Arduino.h>
#include "AudioOutputTDM_F32.h"
#include "utility/imxrt_hw.h"   // set_audioClock(); from Teensy Audio Library
#include <arm_math.h>
#include <string.h>

audio_block_f32_t * AudioOutputTDM_F32::block_input[8] = { nullptr };
bool                AudioOutputTDM_F32::update_responsibility = false;
DMAChannel          AudioOutputTDM_F32::dma(false);
float               AudioOutputTDM_F32::sample_rate_Hz   = AUDIO_SAMPLE_RATE_EXACT;
int                 AudioOutputTDM_F32::audio_block_samples = AUDIO_BLOCK_SAMPLES;
volatile uint32_t   AudioOutputTDM_F32::isr_count = 0;
volatile uint32_t   AudioOutputTDM_F32::update_calls = 0;
volatile uint32_t   AudioOutputTDM_F32::isr_data_chs = 0;
volatile uint32_t   AudioOutputTDM_F32::peak_slot0 = 0;

// Ping-pong DMA buffer: 2 audio blocks worth, 8 slots per frame.
// Lives in DMAMEM (OCRAM2 on Teensy 4.x), which is non-cacheable, so no
// arm_dcache_flush calls are required around the buffer.
DMAMEM __attribute__((aligned(32)))
static uint32_t tdm_tx_buffer[AUDIO_BLOCK_SAMPLES * 8 * 2];
//                            ^block^             ^ch ^ ^ping-pong

// ----------------------------------------------------------------------------
// begin(): identical structure to AudioOutputTDM::begin(); only the ISR
// installed at the end is the F32 version.
// ----------------------------------------------------------------------------
void AudioOutputTDM_F32::begin(void)
{
    dma.begin(true);

    for (int i = 0; i < 8; i++) block_input[i] = nullptr;

    // M4p: stock AudioOutputTDM does memset(tdm_tx_buffer, 0, ...) here.
    // F32 port had been omitting it -- DMAMEM is uninitialized at boot,
    // so the first DMA cycle would ship random bits to the codec until
    // the ISR caught up. Suspected cause of the persistent squeal: those
    // initial garbage bytes put the codec into a stuck state.
    memset(tdm_tx_buffer, 0, sizeof(tdm_tx_buffer));

    config_tdm();

    CORE_PIN7_CONFIG = 3;   // SAI1_TX_DATA0 on Teensy 4.1 pin 7

    dma.TCD->SADDR         = tdm_tx_buffer;
    dma.TCD->SOFF          = 4;
    dma.TCD->ATTR          = DMA_TCD_ATTR_SSIZE(2) | DMA_TCD_ATTR_DSIZE(2);
    dma.TCD->NBYTES_MLNO   = 4;
    dma.TCD->SLAST         = -sizeof(tdm_tx_buffer);
    dma.TCD->DADDR         = (void *)((uint32_t)&I2S1_TDR0 + 0);
    dma.TCD->DOFF          = 0;
    dma.TCD->CITER_ELINKNO = sizeof(tdm_tx_buffer) / 4;
    dma.TCD->DLASTSGA      = 0;
    dma.TCD->BITER_ELINKNO = sizeof(tdm_tx_buffer) / 4;
    dma.TCD->CSR           = DMA_TCD_CSR_INTHALF | DMA_TCD_CSR_INTMAJOR;
    dma.triggerAtHardwareEvent(DMAMUX_SOURCE_SAI1_TX);

    // M4p: match stock ordering -- update_setup BEFORE dma.enable, so the
    // first ISR firing has update_responsibility set correctly and pulls
    // real blocks instead of running with stale state.
    update_responsibility = update_setup();
    dma.enable();

    I2S1_RCSR |= I2S_RCSR_RE | I2S_RCSR_BCE;
    I2S1_TCSR  = I2S_TCSR_TE | I2S_TCSR_BCE | I2S_TCSR_FRDE;

    dma.attachInterrupt(isr);
}

// ----------------------------------------------------------------------------
// ISR: half-buffer interrupt. Fill whichever half the DMA engine is NOT
// currently transmitting. Per channel: float -> q31 saturating, then
// strided store into the slot column for that channel.
// ----------------------------------------------------------------------------
void AudioOutputTDM_F32::isr(void)
{
    isr_count++;   // M4e: half-buffer-IRQ heartbeat for diagnostics

    uint32_t *dest;
    uint32_t saddr = (uint32_t)(dma.TCD->SADDR);
    dma.clearInterrupt();

    if (saddr < (uint32_t)tdm_tx_buffer + sizeof(tdm_tx_buffer) / 2) {
        // DMA is reading first half -> fill second half
        dest = tdm_tx_buffer + AUDIO_BLOCK_SAMPLES * 8;
    } else {
        dest = tdm_tx_buffer;
    }

    if (update_responsibility) AudioStream_F32::update_all();

    // CMSIS-DSP: saturating float->Q31 conversion, vectorized on Cortex-M7.
    // Q31 maps 1.0f -> 0x7FFFFFFF, -1.0f -> 0x80000000. Upper 24 bits of
    // each sample carry signed 24-bit audio after this conversion.
    int32_t q31_tmp[AUDIO_BLOCK_SAMPLES];

    for (int ch = 0; ch < 8; ch++) {
        uint32_t *p = dest + ch;   // start of this channel's column
        if (block_input[ch] != nullptr) {
            isr_data_chs++;   // M4g
            arm_float_to_q31(block_input[ch]->data, q31_tmp, AUDIO_BLOCK_SAMPLES);
            for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
                p[i * 8] = (uint32_t)q31_tmp[i];
            }
            // M4i: peak |Q31| on slot 0 only — that's the L channel from USB.
            // Tells us whether real audio data is hitting the DMA buffer.
            if (ch == 0) {
                uint32_t local_peak = peak_slot0;
                for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
                    int32_t s = q31_tmp[i];
                    uint32_t a = (s < 0) ? (uint32_t)(-s) : (uint32_t)s;
                    if (a > local_peak) local_peak = a;
                }
                peak_slot0 = local_peak;
            }
        } else {
            for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
                p[i * 8] = 0;
            }
        }
    }
}

// ----------------------------------------------------------------------------
// update(): swap in new input blocks each audio frame, release prior ones.
// Same pattern as AudioOutputTDM::update() but using F32 block APIs.
// ----------------------------------------------------------------------------
void AudioOutputTDM_F32::update(void)
{
    update_calls++;   // M4g
    audio_block_f32_t *prev[8];

    __disable_irq();
    for (int i = 0; i < 8; i++) {
        prev[i]        = block_input[i];
        block_input[i] = receiveReadOnly_f32(i);
    }
    __enable_irq();

    for (int i = 0; i < 8; i++) {
        if (prev[i]) AudioStream_F32::release(prev[i]);
    }
}

// ----------------------------------------------------------------------------
// config_tdm(): VERBATIM port of AudioOutputTDM::config_tdm() from
// teensy/cores -> Teensy Audio Library output_tdm.cpp. Reproduced here so
// this class is a single drop-in pair of files. If PJRC updates the upstream
// version, mirror those changes here.
//
// Frame layout produced: 8 slots x 32 bits, MSB-first, 1-bit FSYNC at the
// start of frame (FSE), frame sync + bit clock are outputs (FSD, BCD).
// ----------------------------------------------------------------------------
void AudioOutputTDM_F32::config_tdm(void)
{
    CCM_CCGR5 |= CCM_CCGR5_SAI1(CCM_CCGR_ON);

    // Audio PLL setup -> targets MCLK such that BCLK = fs * 32 * 8 = fs * 256
    int   fs = (int)sample_rate_Hz;
    int   n1 = 4;
    int   n2 = 1 + (24000000 * 27) / (fs * 256 * n1);
    double C = ((double)fs * 256 * n1 * n2) / 24000000;
    int   c0 = C;
    int   c2 = 10000;
    int   c1 = C * c2 - (c0 * c2);
    // Reverted from M4p: force=true is required for the spike. Some other
    // init in the spike's USB-Audio-Class chain enables the audio PLL
    // before AudioOutputTDM_F32::begin() runs (probably the USB stack at
    // SAMPLE_RATE_EXACT=48000). Without force=true, set_audioClock returns
    // early and our intended PLL config never applies, leaving the SAI
    // clocked off the wrong PLL → buzz at output. Stock production
    // doesn't hit this because its USB-AC path doesn't pre-enable the PLL.
    set_audioClock(c0, c1, c2, true);

    CCM_CSCMR1 = (CCM_CSCMR1 & ~(CCM_CSCMR1_SAI1_CLK_SEL_MASK))
               |  CCM_CSCMR1_SAI1_CLK_SEL(2);

    // CRITICAL: TDM mode needs 2x the SAI clock that plain I2S would use,
    // because each frame contains 8x32 = 256 BCLKs vs I2S's 2x32 = 64.
    // PJRC's stock output_tdm.cpp halves n1 here, AFTER computing the PLL
    // multiplier and BEFORE writing it to the SAI clock divider. Without
    // this line the SAI clocks at fs/2, the codec sees 24 kHz FSYNC when
    // the descriptor says 48 kHz, the DAC PLL fails to lock, and the
    // analog output is noise. The prior `claude-32-bit-tdm` reference
    // dropped this line silently and called the result a "verbatim port".
    n1 = n1 / 2;  // Double Speed for TDM (verbatim from stock PJRC)

    CCM_CS1CDR = (CCM_CS1CDR & ~(CCM_CS1CDR_SAI1_CLK_PRED_MASK
                              |  CCM_CS1CDR_SAI1_CLK_PODF_MASK))
               |  CCM_CS1CDR_SAI1_CLK_PRED(n1 - 1)
               |  CCM_CS1CDR_SAI1_CLK_PODF(n2 - 1);

    IOMUXC_GPR_GPR1 = (IOMUXC_GPR_GPR1 & ~(IOMUXC_GPR_GPR1_SAI1_MCLK1_SEL_MASK))
                    |  (IOMUXC_GPR_GPR1_SAI1_MCLK_DIR
                       | IOMUXC_GPR_GPR1_SAI1_MCLK1_SEL(0));

    CORE_PIN23_CONFIG = 3; // MCLK
    CORE_PIN21_CONFIG = 3; // BCLK (RX_BCLK pin, shared)
    CORE_PIN20_CONFIG = 3; // FSYNC (RX_SYNC pin, shared)

    int rsync = 0;
    int tsync = 1;

    I2S1_TMR  = 0;
    I2S1_TCR1 = I2S_TCR1_RFW(4);
    I2S1_TCR2 = I2S_TCR2_SYNC(tsync) | I2S_TCR2_BCP | I2S_TCR2_MSEL(1)
              | I2S_TCR2_BCD | I2S_TCR2_DIV(0);
    I2S1_TCR3 = I2S_TCR3_TCE;
    I2S1_TCR4 = I2S_TCR4_FRSZ(7)   // 8 words per frame (FRSZ = N-1)
              | I2S_TCR4_SYWD(0)   // FSYNC pulse = 1 BCLK
              | I2S_TCR4_MF        // MSB first
              | I2S_TCR4_FSE       // FSYNC asserted one BCLK before frame
              | I2S_TCR4_FSD;      // FSYNC is an output
    I2S1_TCR5 = I2S_TCR5_WNW(31)   // 32-bit slot width (N-1)
              | I2S_TCR5_W0W(31)
              | I2S_TCR5_FBT(31);  // First bit at position 31 (MSB-first)

    I2S1_RMR  = 0;
    I2S1_RCR1 = I2S_RCR1_RFW(4);
    I2S1_RCR2 = I2S_RCR2_SYNC(rsync) | I2S_RCR2_BCP | I2S_RCR2_MSEL(1)
              | I2S_RCR2_BCD | I2S_RCR2_DIV(0);
    I2S1_RCR3 = I2S_RCR3_RCE;
    I2S1_RCR4 = I2S_RCR4_FRSZ(7) | I2S_RCR4_SYWD(0)
              | I2S_RCR4_MF | I2S_RCR4_FSE | I2S_RCR4_FSD;
    I2S1_RCR5 = I2S_RCR5_WNW(31) | I2S_RCR5_W0W(31) | I2S_RCR5_FBT(31);
}
