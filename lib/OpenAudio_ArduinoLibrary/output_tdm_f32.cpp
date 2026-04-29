/*
 *  ***** output_tdm_f32.cpp  *****
 *
 * Audio Library for Teensy 3.X / 4.X
 * Copyright (c) 2017, Paul Stoffregen, paul@pjrc.com
 *
 * Development of this audio library was funded by PJRC.COM, LLC by sales of
 * Teensy and Audio Adaptor boards.  Please support PJRC's efforts to develop
 * open source software by purchasing Teensy or other PJRC products.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice, development funding notice, and this permission
 * notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
/*
 *  F32 conversion derived from output_i2s_quad_f32 (Chip Audette, OpenAudio).
 *  TDM port: 8 channels, 32-bit per slot.
 *  The F32 conversion is under the MIT License. Use at your own risk.
 */

#include "output_tdm_f32.h"

DMAChannel AudioOutputTDM_F32::dma(false);

// Ping-pong DMA buffer: 2 audio blocks worth, 8 slots per frame, 32-bit slots.
// Layout is frame-interleaved: tdm_tx_buffer[i*8 + ch] is the int32 sample
// for channel `ch` at sample index `i` within the half. Stored as float32 in
// the declaration so the same buffer slot can hold the in-flight scaled data
// (the int32 bit pattern is read by the DMA after scale_f32_to_i32 reinterprets
// the bits in place).
DMAMEM __attribute__((aligned(32)))
static float32_t tdm_tx_buffer[AUDIO_BLOCK_SAMPLES * 8 * 2];

void AudioOutputTDM_F32::begin(void)
{
    // Configure most of the SAI peripheral.
    AudioOutputTDM_F32::config_tdm(sample_rate_Hz);

    // Configure SAI1 TX_DATA0 pin (Teensy 4.x pin 7). Same pin stock TDM uses.
    CORE_PIN7_CONFIG = 3;

    // Zero the transmit buffer so the first DMA cycle ships silence rather
    // than whatever happened to be in DMAMEM at boot.
    memset(tdm_tx_buffer, 0, sizeof(tdm_tx_buffer));

    // Configure the DMA channel. Layout is identical to PJRC stock TDM:
    //   - 4 bytes per request (one 32-bit slot)
    //   - SOFF = 4 (advance source by 4 bytes per request)
    //   - DOFF = 0 (always write to TDR0; the SAI shifts each word into the
    //     next slot of the current frame automatically)
    //   - INTHALF + INTMAJOR -> half-buffer + end-of-buffer interrupts
    dma.begin(true);
    dma.TCD->SADDR         = tdm_tx_buffer;
    dma.TCD->SOFF          = 4;
    dma.TCD->ATTR          = DMA_TCD_ATTR_SSIZE(2) | DMA_TCD_ATTR_DSIZE(2);
    dma.TCD->NBYTES_MLNO   = 4;
    dma.TCD->SLAST         = -sizeof(tdm_tx_buffer);
    dma.TCD->DADDR         = (void *)((uint32_t)&I2S1_TDR0);
    dma.TCD->DOFF          = 0;
    dma.TCD->CITER_ELINKNO = sizeof(tdm_tx_buffer) / 4;
    dma.TCD->DLASTSGA      = 0;
    dma.TCD->BITER_ELINKNO = sizeof(tdm_tx_buffer) / 4;
    dma.TCD->CSR           = DMA_TCD_CSR_INTHALF | DMA_TCD_CSR_INTMAJOR;
    dma.triggerAtHardwareEvent(DMAMUX_SOURCE_SAI1_TX);
    dma.attachInterrupt(AudioOutputTDM_F32::isr);

    update_responsibility = update_setup();
    dma.enable();

    I2S1_RCSR |= I2S_RCSR_RE | I2S_RCSR_BCE;
    I2S1_TCSR  = I2S_TCSR_TE | I2S_TCSR_BCE | I2S_TCSR_FRDE;
}

// ----------------------------------------------------------------------------
// ISR: one half-buffer interrupt per audio block. Fill whichever half the DMA
// is NOT currently transmitting from the 8 input blocks. Each input block has
// already been F32 -> int32 scaled in place by update().
// ----------------------------------------------------------------------------
void AudioOutputTDM_F32::isr(void)
{
    int32_t *dest;
    uint32_t saddr = (uint32_t)(dma.TCD->SADDR);
    dma.clearInterrupt();

    if (saddr < (uint32_t)tdm_tx_buffer + sizeof(tdm_tx_buffer) / 2) {
        // DMA is reading the first half -> fill the second half.
        dest = (int32_t *)&tdm_tx_buffer[audio_block_samples * 8];
    } else {
        dest = (int32_t *)tdm_tx_buffer;
    }

    if (update_responsibility) AudioStream_F32::update_all();

    // For each channel, write its column of the frame-interleaved half-buffer.
    // dest[i*8 + ch] is the int32 sample for channel ch at frame index i.
    for (int ch = 0; ch < 8; ch++) {
        if (block_input[ch] != nullptr) {
            const int32_t *src = (const int32_t *)block_input[ch]->data;
            for (int i = 0; i < audio_block_samples; i++) {
                dest[i * 8 + ch] = src[i];
            }
        } else {
            for (int i = 0; i < audio_block_samples; i++) {
                dest[i * 8 + ch] = 0;
            }
        }
    }

    // Flush the L1 cache so the DMA reads the freshly-written samples.
    arm_dcache_flush_delete(dest, sizeof(tdm_tx_buffer) / 2);

    // Release the consumed input blocks (we read them to completion in
    // this ISR, so they can be returned to the pool now).
    for (int ch = 0; ch < 8; ch++) {
        if (block_input[ch]) {
            AudioStream_F32::release(block_input[ch]);
            block_input[ch] = nullptr;
        }
    }
}

// Saturating F32 -> int32 conversion, in place. F32 [-1.0, +1.0] maps to
// int32 [INT32_MIN, INT32_MAX]. Out-of-range inputs are clamped.
void AudioOutputTDM_F32::scale_f32_to_i32(float32_t *p_f32, int len)
{
    for (int i = 0; i < len; i++) {
        float32_t v = p_f32[i] * F32_TO_I32_NORM_FACTOR;
        if (v >  F32_TO_I32_NORM_FACTOR) v =  F32_TO_I32_NORM_FACTOR;
        if (v < -F32_TO_I32_NORM_FACTOR) v = -F32_TO_I32_NORM_FACTOR;
        // Type-pun: store the int32 bit pattern back into the float32 slot.
        // The ISR will read these as int32_t.
        ((int32_t *)p_f32)[i] = (int32_t)v;
    }
}

// Read the 8 input blocks, optionally apply outputScale, and convert each
// in place to int32. The ISR consumes the int32 bit patterns next half-cycle.
void AudioOutputTDM_F32::update(void)
{
    // Hold the just-scaled blocks so the ISR can read them. Swap the
    // previous frame's blocks out and release them after the swap.
    audio_block_f32_t *prev[8];

    audio_block_f32_t *fresh[8];
    for (int ch = 0; ch < 8; ch++) {
        fresh[ch] = receiveWritable_f32(ch);
        if (fresh[ch]) {
            if (outputScale < 1.0f || outputScale > 1.0f) {
                arm_scale_f32(fresh[ch]->data, outputScale,
                              fresh[ch]->data, fresh[ch]->length);
            }
            scale_f32_to_i32(fresh[ch]->data, audio_block_samples);
        }
    }

    __disable_irq();
    for (int ch = 0; ch < 8; ch++) {
        prev[ch]        = block_input[ch];
        block_input[ch] = fresh[ch];
    }
    __enable_irq();

    for (int ch = 0; ch < 8; ch++) {
        if (prev[ch]) AudioStream_F32::release(prev[ch]);
    }
}

// ----------------------------------------------------------------------------
// config_tdm(): SAI1 register configuration. Frame layout: 8 slots x 32 bits,
// MSB-first, 1-bit FSYNC at the start of frame (FSE), frame sync + bit clock
// are outputs (FSD, BCD). BCLK = fs * 256 (= 8 slots * 32 bits per slot).
//
// Mostly verbatim from PJRC's AudioOutputTDM::config_tdm() so codec
// configuration that works with stock TDM also works here.
// ----------------------------------------------------------------------------
void AudioOutputTDM_F32::config_tdm(int fs_Hz)
{
    CCM_CCGR5 |= CCM_CCGR5_SAI1(CCM_CCGR_ON);

    // If either transmitter or receiver is enabled, do nothing.
    if (I2S1_TCSR & I2S_TCSR_TE) return;
    if (I2S1_RCSR & I2S_RCSR_RE) return;

    // Audio PLL: target MCLK such that BCLK = fs * 256.
    // PLL between 27*24 = 648MHz and 54*24 = 1296MHz.
    int    fs = fs_Hz;
    int    n1 = 4;   // SAI prescaler factor; (n1 * n2) must be a multiple of 4
    int    n2 = 1 + (24000000 * 27) / (fs * 256 * n1);
    double C  = ((double)fs * 256 * n1 * n2) / 24000000;
    int    c0 = (int)C;
    int    c2 = 10000;
    int    c1 = (int)(C * c2 - (c0 * c2));
    set_audioClock(c0, c1, c2);

    CCM_CSCMR1 = (CCM_CSCMR1 & ~(CCM_CSCMR1_SAI1_CLK_SEL_MASK))
               |  CCM_CSCMR1_SAI1_CLK_SEL(2);    // PLL4

    // CRITICAL: TDM mode needs 2x the SAI clock that plain I2S would use,
    // because each frame is 8 * 32 = 256 BCLKs vs I2S's 2 * 32 = 64.
    // PJRC's stock output_tdm.cpp halves n1 here, AFTER the PLL multiplier
    // has been computed and BEFORE writing it to the SAI clock divider.
    n1 = n1 / 2;  // "Double Speed for TDM" (verbatim from stock PJRC)

    CCM_CS1CDR = (CCM_CS1CDR & ~(CCM_CS1CDR_SAI1_CLK_PRED_MASK
                              |  CCM_CS1CDR_SAI1_CLK_PODF_MASK))
               |  CCM_CS1CDR_SAI1_CLK_PRED(n1 - 1)
               |  CCM_CS1CDR_SAI1_CLK_PODF(n2 - 1);

    IOMUXC_GPR_GPR1 = (IOMUXC_GPR_GPR1 & ~(IOMUXC_GPR_GPR1_SAI1_MCLK1_SEL_MASK))
                    |  (IOMUXC_GPR_GPR1_SAI1_MCLK_DIR
                       | IOMUXC_GPR_GPR1_SAI1_MCLK1_SEL(0));

    CORE_PIN23_CONFIG = 3;   // MCLK
    CORE_PIN21_CONFIG = 3;   // BCLK (RX_BCLK pin, shared)
    CORE_PIN20_CONFIG = 3;   // FSYNC (RX_SYNC pin, shared)

    int rsync = 0;
    int tsync = 1;

    I2S1_TMR  = 0;
    I2S1_TCR1 = I2S_TCR1_RFW(4);
    I2S1_TCR2 = I2S_TCR2_SYNC(tsync) | I2S_TCR2_BCP | I2S_TCR2_MSEL(1)
              | I2S_TCR2_BCD | I2S_TCR2_DIV(0);
    I2S1_TCR3 = I2S_TCR3_TCE;
    I2S1_TCR4 = I2S_TCR4_FRSZ(7)   // 8 words per frame (FRSZ = N - 1)
              | I2S_TCR4_SYWD(0)   // FSYNC pulse = 1 BCLK
              | I2S_TCR4_MF        // MSB first
              | I2S_TCR4_FSE       // FSYNC asserted one BCLK before frame
              | I2S_TCR4_FSD;      // FSYNC is an output
    I2S1_TCR5 = I2S_TCR5_WNW(31)   // 32-bit slot width (N - 1)
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
