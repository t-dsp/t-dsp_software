/*
 * usb_audio_f32_buffers.h -- T-DSP F32 USB audio plumbing (M4a)
 *
 * Public API of the int24-raw / Q31-storage ring buffers used by the
 * F32-native USB audio path. See usb_audio_f32_buffers.cpp for the
 * storage convention and ownership notes.
 *
 * All symbols are gated on AUDIO_SUBSLOT_SIZE == 3 && AUDIO_INTERFACE so
 * the int16 path stays bit-identical when the flag is not set.
 *
 * M4a milestone: the data plumbing only. No F32 class API yet --
 * AudioInputUSB_F32 / AudioOutputUSB_F32 will land at M4b on top of
 * these calls.
 */

#pragma once

#include <stdint.h>
#include "usb_desc.h"  // pulls AUDIO_INTERFACE / AUDIO_SUBSLOT_SIZE definitions

#if defined(AUDIO_SUBSLOT_SIZE) && AUDIO_SUBSLOT_SIZE == 3 && defined(AUDIO_INTERFACE)

#ifdef __cplusplus
extern "C" {
#endif

/* ISR-side: invoked from usb_audio.cpp's rx_event path.
 * Parses int24 LE stereo-interleaved samples out of a USB packet and
 * stores them as Q31 (left-justified, low byte zero) in the RX ring. */
void usb_audio_f32_rx_isr_write(const uint8_t *packet, unsigned int len_bytes);

/* ISR-side: invoked from usb_audio.cpp's tx_event path.
 * Pulls Q31 samples from the TX ring and emits them as int24 LE
 * stereo-interleaved bytes into a USB packet. Returns bytes written.
 * On underrun, pads the remainder with silence. */
unsigned int usb_audio_f32_tx_isr_read(uint8_t *packet, unsigned int n_stereo_samples);

/* Audio-thread side: pop n_samples from the RX ring, deinterleave and
 * convert Q31 -> F32 via arm_q31_to_float (saturating, no scale).
 * Returns true on success; false on underrun (caller fills silence). */
bool usb_audio_f32_rx_pop(float *left_out, float *right_out,
                          unsigned int n_samples);

/* Audio-thread side: push n_samples to the TX ring, convert F32 -> Q31
 * via arm_float_to_q31 (saturating, no scale) and interleave.
 * Returns true on success; false on overrun (caller's data is dropped). */
bool usb_audio_f32_tx_push(const float *left_in, const float *right_in,
                           unsigned int n_samples);

/* Diagnostics. Incremented by ISR / audio thread respectively. */
extern volatile uint32_t usb_audio_f32_rx_overruns;
extern volatile uint32_t usb_audio_f32_tx_underruns;

/* M4e: per-callback packet counters (incremented in usb_audio.cpp,
 * not here). volatile uint32_t, ISR-only writers. */
extern volatile uint32_t usb_audio_f32_rx_packets;
extern volatile uint32_t usb_audio_f32_tx_packets;

#ifdef __cplusplus
} // extern "C"
#endif

#endif // AUDIO_SUBSLOT_SIZE == 3 && AUDIO_INTERFACE
