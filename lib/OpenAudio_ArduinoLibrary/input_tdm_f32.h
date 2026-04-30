/*
 *  *****  input_tdm_f32.h  *****
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
 *  TDM port: 8 output ports, 8 slots per frame, 32-bit slot width on SAI1
 *  RX_DATA0 (Teensy 4.x pin 8). Pairs with AudioOutputTDM_F32 -- the SAI
 *  is configured by whichever class begins first; the second one finds the
 *  TX path already running and only sets up its own DMA.
 *
 *  Each input slot is read as a signed int32 (32-bit-per-slot TDM frame),
 *  scaled to F32 by dividing by 2^31, and emitted on the matching output
 *  port. With a 24-bit codec like the TAC5212, the lower 8 bits are the
 *  codec's "always zero" padding and the upper 24 bits are signed audio.
 *
 *  The F32 conversion is under the MIT License. Use at your own risk.
 */

#ifndef input_tdm_f32_h_
#define input_tdm_f32_h_

#include <Arduino.h>
#include "AudioStream_F32.h"
#include "DMAChannel.h"

class AudioInputTDM_F32 : public AudioStream_F32
{
    // GUI: inputs:0, outputs:8  //this line used for automatic generation of GUI node
public:
    AudioInputTDM_F32(void) : AudioStream_F32(0, NULL) { begin(); }

    AudioInputTDM_F32(const AudioSettings_F32 &settings)
        : AudioStream_F32(0, NULL)
    {
        sample_rate_Hz      = settings.sample_rate_Hz;
        audio_block_samples = settings.audio_block_samples;
        begin();
    }

    virtual void update(void);

protected:
    void begin();
    static void isr(void);

    inline static audio_block_f32_t *block_incoming[8] = { nullptr };
    inline static bool               update_responsibility = false;
    static DMAChannel                dma;

private:
    inline static int sample_rate_Hz      = AUDIO_SAMPLE_RATE;
    inline static int audio_block_samples = AUDIO_BLOCK_SAMPLES;
    const float32_t   I32_TO_F32_NORM_FACTOR = 1.0f / 2147483648.0f;  // 1 / 2^31
};

#endif  // input_tdm_f32_h_
