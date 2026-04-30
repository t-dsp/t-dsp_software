/*
 * AudioOutputUSB_F32.cpp
 *
 * STORAGE CONVENTION (Q31, NOT Q23):
 *   See lib/teensy_cores/teensy4/usb_audio_f32_buffers.cpp banner.
 */

#include "AudioOutputUSB_F32.h"
#include <usb_audio.h>   // for AudioOutputUSB::features (FU 0x30)

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

// ----- Feature Unit accessors (FU 0x30, Windows recording slider) -----

float AudioOutputUSB_F32::volume() {
    if (AudioOutputUSB::features.mute) return 0.0f;
    return (float)AudioOutputUSB::features.volume *
           (1.0f / (float)FEATURE_MAX_VOLUME);
}

bool AudioOutputUSB_F32::mute() {
    return AudioOutputUSB::features.mute != 0;
}
