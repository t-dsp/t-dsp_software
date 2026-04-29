/*
 *  *****  output_tdm_f32.h  *****
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
 *  TDM port: 8 input ports, 8 slots per frame, 32-bit slot width on SAI1
 *  TX_DATA0 (Teensy 4.x pin 7). Frame layout matches PJRC's stock
 *  AudioOutputTDM exactly so existing TDM codecs (TAC5212, CS42448, AK4558,
 *  PCM3168, etc.) work without changes to their I2S/TDM register settings.
 *
 *  Unlike stock AudioOutputTDM, each 32-bit slot carries one full F32 sample
 *  scaled to int32 (saturating). This yields true 32-bit data per slot; with
 *  the codec configured for 16/24/32-bit data, the codec reads the upper
 *  N bits and discards the rest.
 *
 *  The F32 conversion is under the MIT License. Use at your own risk.
 */

#ifndef output_tdm_f32_h_
#define output_tdm_f32_h_

#include <Arduino.h>
#include "AudioStream_F32.h"
#include "DMAChannel.h"
#include <utility/imxrt_hw.h>   // From Teensy Audio library; for set_audioClock().

class AudioInputTDM_F32;  // forward decl for friend access to config_tdm

class AudioOutputTDM_F32 : public AudioStream_F32
{
    // GUI: inputs:8, outputs:0  //this line used for automatic generation of GUI node
    friend class AudioInputTDM_F32;
public:
    // Default constructor: AUDIO_SAMPLE_RATE / AUDIO_BLOCK_SAMPLES from
    // AudioStream_F32.h (i.e. whatever the project compiled against).
    AudioOutputTDM_F32(void) : AudioStream_F32(8, inputQueueArray) { begin(); }

    // Variable sample rate / block size constructor.
    AudioOutputTDM_F32(const AudioSettings_F32 &settings)
        : AudioStream_F32(8, inputQueueArray)
    {
        sample_rate_Hz      = settings.sample_rate_Hz;
        audio_block_samples = settings.audio_block_samples;
        begin();
    }

    // outputScale is a global gain applied to all 8 channels before the
    // F32 -> int32 conversion. If left at exactly 1.0f, the input data
    // passes through unscaled (no extra multiply).
    void setGain(float _oscale) { outputScale = _oscale; }

    virtual void update(void);

protected:
    void begin();
    static void config_tdm(int fs_Hz);
    static void isr(void);

    inline static audio_block_f32_t *block_input[8] = { nullptr };
    inline static bool               update_responsibility = false;
    static DMAChannel                dma;

private:
    void scale_f32_to_i32(float32_t *p_f32, int len);
    audio_block_f32_t *inputQueueArray[8];

    inline static int   sample_rate_Hz      = AUDIO_SAMPLE_RATE;
    inline static int   audio_block_samples = AUDIO_BLOCK_SAMPLES;
    const float32_t     F32_TO_I32_NORM_FACTOR = 2147483647.0f;  // 2^31 - 1
    float               outputScale         = 1.0f;
};

#endif  // output_tdm_f32_h_
