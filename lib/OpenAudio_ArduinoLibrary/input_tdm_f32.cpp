/*
 *  ***** input_tdm_f32.cpp  *****
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
 *  F32 conversion derived from input_i2s_f32 (Chip Audette, OpenAudio).
 *  TDM port: 8 channels, 32-bit per slot.
 *  The F32 conversion is under the MIT License. Use at your own risk.
 */

#include "input_tdm_f32.h"
#include "output_tdm_f32.h"   // for AudioOutputTDM_F32::config_tdm()
#include <utility/imxrt_hw.h>
#include <arm_math.h>
#include <string.h>

DMAChannel AudioInputTDM_F32::dma(false);

// Ping-pong DMA buffer: 2 audio blocks worth, 8 slots per frame, 32-bit slots.
// Same layout as the output: tdm_rx_buffer[i*8 + ch] is the int32 sample
// for channel `ch` at sample index `i`.
DMAMEM __attribute__((aligned(32)))
static int32_t tdm_rx_buffer[AUDIO_BLOCK_SAMPLES * 8 * 2];

void AudioInputTDM_F32::begin(void)
{
    dma.begin(true);

    // SAI1 register configuration. If the matching AudioOutputTDM_F32 already
    // ran begin(), the early-out at the top of config_tdm() (TCSR_TE check)
    // makes this a no-op. Either way, after this call the SAI is set up for
    // 8-slot / 32-bit TDM.
    AudioOutputTDM_F32::config_tdm(sample_rate_Hz);

    // SAI1 RX_DATA0 = Teensy 4.x pin 8.
    CORE_PIN8_CONFIG = 3;
    IOMUXC_SAI1_RX_DATA0_SELECT_INPUT = 2;

    // DMA: read one 32-bit word per request from I2S1_RDR0, advance dest.
    dma.TCD->SADDR         = &I2S1_RDR0;
    dma.TCD->SOFF          = 0;
    dma.TCD->ATTR          = DMA_TCD_ATTR_SSIZE(2) | DMA_TCD_ATTR_DSIZE(2);
    dma.TCD->NBYTES_MLNO   = 4;
    dma.TCD->SLAST         = 0;
    dma.TCD->DADDR         = tdm_rx_buffer;
    dma.TCD->DOFF          = 4;
    dma.TCD->CITER_ELINKNO = sizeof(tdm_rx_buffer) / 4;
    dma.TCD->DLASTSGA      = -sizeof(tdm_rx_buffer);
    dma.TCD->BITER_ELINKNO = sizeof(tdm_rx_buffer) / 4;
    dma.TCD->CSR           = DMA_TCD_CSR_INTHALF | DMA_TCD_CSR_INTMAJOR;
    dma.triggerAtHardwareEvent(DMAMUX_SOURCE_SAI1_RX);
    dma.attachInterrupt(AudioInputTDM_F32::isr);

    update_responsibility = update_setup();

    // Enable the RX path. RE = receiver enable; BCE = bit clock enable;
    // FRDE = FIFO request DMA enable; FR = FIFO reset.
    I2S1_RCSR |= I2S_RCSR_RE | I2S_RCSR_BCE | I2S_RCSR_FRDE | I2S_RCSR_FR;
    dma.enable();
}

// ----------------------------------------------------------------------------
// ISR: one half-buffer interrupt per audio block. Convert whichever half
// just finished from int32 -> F32 into the 8 incoming F32 blocks. update()
// hands those blocks downstream and allocates fresh ones for the next round.
// ----------------------------------------------------------------------------
void AudioInputTDM_F32::isr(void)
{
    int32_t *src;
    uint32_t daddr = (uint32_t)(dma.TCD->DADDR);
    dma.clearInterrupt();

    if (daddr < (uint32_t)tdm_rx_buffer + sizeof(tdm_rx_buffer) / 2) {
        // DMA is writing to first half -> the second half is the freshly
        // captured one we should pull from.
        src = &tdm_rx_buffer[audio_block_samples * 8];
    } else {
        src = &tdm_rx_buffer[0];
    }

    // Invalidate the L1 cache for the half we're about to read so we see
    // the bytes the DMA actually wrote (rather than stale cache lines).
    arm_dcache_delete((void *)src, sizeof(tdm_rx_buffer) / 2);

    if (block_incoming[0] != nullptr) {
        // Per channel: gather the int32 column and convert to F32 in place.
        // CMSIS-DSP's arm_q31_to_float divides by 2^31 with hardware FPU,
        // matching our I32_TO_F32_NORM_FACTOR semantics exactly.
        int32_t q31_tmp[AUDIO_BLOCK_SAMPLES];
        for (int ch = 0; ch < 8; ch++) {
            if (block_incoming[ch] == nullptr) continue;
            for (int i = 0; i < audio_block_samples; i++) {
                q31_tmp[i] = src[i * 8 + ch];
            }
            arm_q31_to_float(q31_tmp, block_incoming[ch]->data,
                             audio_block_samples);
        }
    }

    if (update_responsibility) AudioStream_F32::update_all();
}

// ----------------------------------------------------------------------------
// update(): allocate 8 new F32 blocks and atomically swap them in for the
// previous frame's blocks. Transmit the previous blocks downstream.
// ----------------------------------------------------------------------------
void AudioInputTDM_F32::update(void)
{
    audio_block_f32_t *new_block[8];
    audio_block_f32_t *out_block[8];

    // Allocate 8 fresh blocks. If any allocation fails, release the lot
    // and leave block_incoming alone for this frame (silent output).
    for (int i = 0; i < 8; i++) {
        new_block[i] = AudioStream_F32::allocate_f32();
        if (new_block[i] == nullptr) {
            for (int j = 0; j < i; j++) AudioStream_F32::release(new_block[j]);
            memset(new_block, 0, sizeof(new_block));
            break;
        }
    }

    __disable_irq();
    memcpy(out_block,      block_incoming, sizeof(out_block));
    memcpy(block_incoming, new_block,      sizeof(block_incoming));
    __enable_irq();

    if (out_block[0] != nullptr) {
        for (int i = 0; i < 8; i++) {
            AudioStream_F32::transmit(out_block[i], i);
            AudioStream_F32::release(out_block[i]);
        }
    }
}
