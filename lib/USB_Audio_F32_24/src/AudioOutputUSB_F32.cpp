/*
 * AudioOutputUSB_F32.cpp
 *
 * STORAGE CONVENTION (Q31, NOT Q23):
 *   See lib/teensy_cores/teensy4/usb_audio_f32_buffers.cpp banner.
 */

#include "AudioOutputUSB_F32.h"

void AudioOutputUSB_F32::update(void)
{
    audio_block_f32_t *left  = receiveReadOnly_f32(0);
    audio_block_f32_t *right = receiveReadOnly_f32(1);
    if (!left || !right) {
        if (left)  AudioStream_F32::release(left);
        if (right) AudioStream_F32::release(right);
        return;
    }

#if defined(AUDIO_SUBSLOT_SIZE) && AUDIO_SUBSLOT_SIZE == 3
    // Ring overrun returns false; drop block silently. Stub at this
    // milestone -- M5 may surface this as a counter, but the user
    // explicitly bounded telemetry to rx_overruns / tx_underruns.
    (void)usb_audio_f32_tx_push(left->data, right->data, AUDIO_BLOCK_SAMPLES);
#endif

    AudioStream_F32::release(left);
    AudioStream_F32::release(right);
}
