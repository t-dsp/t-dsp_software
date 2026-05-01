/*
 * AudioOutputTDM_F32.h
 *
 * Float32 input -> 24-bit-in-32-bit TDM output on Teensy 4.1 (SAI1, pin 7)
 * Drop-in F32 sibling of PJRC's AudioOutputTDM. 8 slots per frame, 32-bit
 * slot width, 24-bit signed data left-justified in each slot. Compatible
 * with TAC5212 and other 32/24 TDM codecs.
 *
 * Patterned after AudioOutputI2S_F32 (Chip Audette / OpenAudio_ArduinoLibrary)
 * and AudioOutputTDM (Paul Stoffregen / Teensy Audio Library).
 *
 * License: same terms as the libraries it derives from (MIT-style).
 */

#ifndef AudioOutputTDM_F32_h_
#define AudioOutputTDM_F32_h_

#include <Arduino.h>
#include <AudioStream_F32.h>
#include <AudioSettings_F32.h>
#include <DMAChannel.h>

class AudioOutputTDM_F32 : public AudioStream_F32 {
//GUI: inputs:8, outputs:0  //this line used by the Tympan/OpenAudio Design Tool
public:
    AudioOutputTDM_F32(void)
        : AudioStream_F32(8, inputQueueArray)
    {
        sample_rate_Hz       = AUDIO_SAMPLE_RATE_EXACT;
        audio_block_samples  = AUDIO_BLOCK_SAMPLES;
        begin();
    }

    AudioOutputTDM_F32(const AudioSettings_F32 &settings)
        : AudioStream_F32(8, inputQueueArray)
    {
        sample_rate_Hz       = settings.sample_rate_Hz;
        audio_block_samples  = settings.audio_block_samples;
        begin();
    }

    virtual void update(void);
    void begin(void);

protected:
    static void config_tdm(void);
    static void isr(void);

private:
    static audio_block_f32_t *block_input[8];
    static bool               update_responsibility;
    static DMAChannel         dma;
    audio_block_f32_t        *inputQueueArray[8];
    static float              sample_rate_Hz;
    static int                audio_block_samples;
};

#endif
