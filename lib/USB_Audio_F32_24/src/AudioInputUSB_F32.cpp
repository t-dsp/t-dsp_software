/*
 * AudioInputUSB_F32.cpp
 *
 * STORAGE CONVENTION (Q31, NOT Q23):
 *   See lib/teensy_cores/teensy4/usb_audio_f32_buffers.cpp banner for
 *   the full convention. Q31 left-justified int24 in the ring buffer;
 *   arm_q31_to_float -- invoked inside usb_audio_f32_rx_pop -- does
 *   the conversion with no manual scale factor.
 */

#include "AudioInputUSB_F32.h"
#include <string.h>

void AudioInputUSB_F32::update(void)
{
    audio_block_f32_t *left  = AudioStream_F32::allocate_f32();
    audio_block_f32_t *right = AudioStream_F32::allocate_f32();
    if (!left || !right) {
        if (left)  AudioStream_F32::release(left);
        if (right) AudioStream_F32::release(right);
        return;
    }

#if defined(AUDIO_SUBSLOT_SIZE) && AUDIO_SUBSLOT_SIZE == 3
    if (!usb_audio_f32_rx_pop(left->data, right->data, AUDIO_BLOCK_SAMPLES)) {
        // Ring underrun; emit silence rather than stale data.
        memset(left->data,  0, sizeof(left->data));
        memset(right->data, 0, sizeof(right->data));
    }
#else
    memset(left->data,  0, sizeof(left->data));
    memset(right->data, 0, sizeof(right->data));
#endif

    transmit(left, 0);
    transmit(right, 1);
    AudioStream_F32::release(left);
    AudioStream_F32::release(right);
}
