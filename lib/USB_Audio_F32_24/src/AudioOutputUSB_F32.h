/*
 * AudioOutputUSB_F32.h -- F32-native USB IN endpoint (Teensy -> host).
 *
 * STORAGE CONVENTION (Q31, NOT Q23):
 *   See lib/teensy_cores/teensy4/usb_audio_f32_buffers.cpp banner.
 *   arm_float_to_q31 -- invoked inside usb_audio_f32_tx_push -- does
 *   the F32 -> Q31 conversion with no manual scale factor.
 *
 * ARCHITECTURE: USB-D5 option (b). This class's update() converts
 *   F32 -> Q31 in audio-thread context; the USB ISR pulls raw int24
 *   from the ring. No int16 round-trip.
 *
 * Active only when AUDIO_SUBSLOT_SIZE == 3. With the flag unset,
 * the class drains its inputs and produces no host-side audio.
 */

#pragma once

#include <Arduino.h>
#include <AudioStream_F32.h>
#include <AudioSettings_F32.h>
#include <usb_audio_f32_buffers.h>

class AudioOutputUSB_F32 : public AudioStream_F32 {
//GUI: inputs:2, outputs:0  //OpenAudio Design Tool tag
public:
    AudioOutputUSB_F32() : AudioStream_F32(2, inputQueueArray) {}
    AudioOutputUSB_F32(const AudioSettings_F32 &) : AudioStream_F32(2, inputQueueArray) {}

    virtual void update(void);

private:
    audio_block_f32_t *inputQueueArray[2];
};
