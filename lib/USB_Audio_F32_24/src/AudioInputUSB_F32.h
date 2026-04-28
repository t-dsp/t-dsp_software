/*
 * AudioInputUSB_F32.h -- F32-native USB OUT endpoint (host -> Teensy).
 *
 * STORAGE CONVENTION (Q31, NOT Q23):
 *   24-bit samples are stored left-justified in int32_t in the ring
 *   buffer at usb_audio_f32_buffers.cpp; arm_q31_to_float converts
 *   them with no manual scale factor. See that file's banner block
 *   for the full convention.
 *
 * ARCHITECTURE: USB-D5 option (b). The USB ISR writes int24 raw
 *   (sign-extended to int32 via Q31 left-justification) into the
 *   ring; this class's update() does the Q31 -> F32 conversion in
 *   audio-thread context. No int16 round-trip.
 *
 * Active only when AUDIO_SUBSLOT_SIZE == 3. With the flag unset,
 * the class is empty and instantiating it produces silence.
 */

#pragma once

#include <Arduino.h>
#include <AudioStream_F32.h>
#include <AudioSettings_F32.h>
#include <usb_audio_f32_buffers.h>

class AudioInputUSB_F32 : public AudioStream_F32 {
//GUI: inputs:0, outputs:2  //OpenAudio Design Tool tag
public:
    AudioInputUSB_F32() : AudioStream_F32(0, nullptr) {}
    AudioInputUSB_F32(const AudioSettings_F32 &) : AudioStream_F32(0, nullptr) {}

    virtual void update(void);

    // Diagnostics for both directions; counters are global to the F32 path.
    struct Status {
        uint32_t rx_overruns;
        uint32_t tx_underruns;
    };
    static Status getStatus() {
#if defined(AUDIO_SUBSLOT_SIZE) && AUDIO_SUBSLOT_SIZE == 3
        return { usb_audio_f32_rx_overruns, usb_audio_f32_tx_underruns };
#else
        return { 0, 0 };
#endif
    }

    // M4g graph-side counters: did update() run, and did rx_pop hand back
    // real data (vs silence on underrun)?
    static volatile uint32_t updates;
    static volatile uint32_t pop_ok;
};
