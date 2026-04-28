/*
 * usb_audio_f32_buffers.cpp -- T-DSP F32 USB audio plumbing (M4a)
 *
 * ================================================================
 * STORAGE CONVENTION (Q31, NOT Q23):
 * ================================================================
 *
 * 24-bit samples are stored left-justified in int32_t with the bottom
 * 8 bits zero. arm_q31_to_float / arm_float_to_q31 work on the result
 * directly with no manual scale factor; saturation is automatic.
 *
 *     USB ISR (input)   assembled int32 = (b2<<24) | (b1<<16) | (b0<<8);
 *                       stored as-is in the ring buffer. NO >>8.
 *     update() (input)  arm_q31_to_float(ring_ptr, f32_block, n);
 *                       no scale factor.
 *     update() (output) arm_float_to_q31(f32_block, ring_ptr, n);
 *                       no scale factor.
 *     USB ISR (output)  pull int32 from ring; transmit upper 3 bytes
 *                       (b2 = top byte, b1 = mid, b0 = bits 15..8).
 *
 * The lower 8 bits of each Q31 word are zero on the input path and
 * dropped on the output path. The codec / host receives 24-bit precision
 * either way; the extra bits cost nothing because the hardware ignores
 * them.
 *
 * ================================================================
 * RING BUFFERS:
 * ================================================================
 *
 * Two stereo-interleaved rings, one per direction. Sample slots are
 * int32_t Q31. Producer / consumer:
 *
 *     RX ring   producer = USB ISR (host -> Teensy)
 *               consumer = audio thread update() via _rx_pop()
 *     TX ring   producer = audio thread update() via _tx_push()
 *               consumer = USB ISR (Teensy -> host)
 *
 * Each ring is 1024 mono samples = 4 KB per direction (8 KB total),
 * power-of-two sized so masking replaces modulo. Holds ~4 audio blocks
 * of headroom at AUDIO_BLOCK_SAMPLES == 128.
 *
 * Lock-free SPSC: single-word index reads and writes are atomic on
 * Cortex-M7. `volatile` prevents compiler reordering. No DMB inserted
 * because the existing PJRC USB ring buffers (int16 path) operate the
 * same way and have not shown ordering issues; if the F32 path ever
 * does, add explicit __DMB() at the index commits.
 *
 * ================================================================
 * BUFFER OWNERSHIP / ALIGNMENT:
 * ================================================================
 *
 * Buffers live in DMAMEM (OCRAM2). OCRAM2 is non-cacheable on Teensy
 * 4.x, so NO cache flush calls are made and none are needed. The 32-
 * byte alignment is for safety against any future CPU-side prefetch
 * but is not load-bearing for cache coherency on this part.
 *
 * The USB controller does not DMA into these rings -- it DMAs into
 * rx_buffer / usb_audio_transmit_buffer in usb_audio.cpp, and our
 * ISR-side callbacks copy from those into the rings. So there is no
 * USB-DMA <-> ring-buffer cache concern; only CPU <-> CPU traffic.
 *
 * M4a milestone: data plumbing only. F32 classes (M4b) consume this
 * API. Spike wiring (M4c) instantiates the classes.
 */

#include "usb_audio_f32_buffers.h"

#if defined(AUDIO_SUBSLOT_SIZE) && AUDIO_SUBSLOT_SIZE == 3 && defined(AUDIO_INTERFACE)

#include <Arduino.h>
#include <arm_math.h>
#include <string.h>

namespace {

constexpr unsigned int RING_SAMPLES = 1024;          // mono slots; power of two
constexpr unsigned int RING_MASK    = RING_SAMPLES - 1;
constexpr unsigned int CONV_TMP_MAX = 256;           // upper bound for one-shot

DMAMEM int32_t rx_ring[RING_SAMPLES] __attribute__((aligned(32)));
DMAMEM int32_t tx_ring[RING_SAMPLES] __attribute__((aligned(32)));

volatile uint32_t rx_write = 0;
volatile uint32_t rx_read  = 0;
volatile uint32_t tx_write = 0;
volatile uint32_t tx_read  = 0;

} // namespace

volatile uint32_t usb_audio_f32_rx_overruns  = 0;
volatile uint32_t usb_audio_f32_tx_underruns = 0;

// =====================================================================
// ISR side -- called from usb_audio.cpp rx_event / tx_event.
// =====================================================================

extern "C"
void usb_audio_f32_rx_isr_write(const uint8_t *packet, unsigned int len_bytes)
{
    const unsigned int n_mono = len_bytes / 3;
    const uint32_t r = rx_read;
    uint32_t w = rx_write;

    for (unsigned int i = 0; i < n_mono; i++) {
        const uint32_t b0 = packet[3 * i + 0];
        const uint32_t b1 = packet[3 * i + 1];
        const uint32_t b2 = packet[3 * i + 2];
        // Q31 left-justified. b2 lands in bits 31..24, sign-extending the
        // 24-bit two's-complement value to int32_t correctly.
        const int32_t q31 = (int32_t)((b2 << 24) | (b1 << 16) | (b0 << 8));

        const uint32_t w_next = (w + 1) & RING_MASK;
        if (w_next == r) {
            // Audio thread has not consumed; drop new samples and bail.
            usb_audio_f32_rx_overruns++;
            break;
        }
        rx_ring[w] = q31;
        w = w_next;
    }
    rx_write = w;
}

extern "C"
unsigned int usb_audio_f32_tx_isr_read(uint8_t *packet, unsigned int n_stereo_samples)
{
    const uint32_t w = tx_write;
    uint32_t r = tx_read;
    const unsigned int n_mono_avail = (w - r) & RING_MASK;
    const unsigned int n_mono_want  = n_stereo_samples * 2;

    unsigned int n_mono_emit = (n_mono_avail >= n_mono_want)
                               ? n_mono_want
                               : (n_mono_avail & ~1u);  // even = whole stereo pairs

    for (unsigned int i = 0; i < n_mono_emit; i++) {
        const int32_t q31 = tx_ring[r];
        // Upper 3 bytes, little-endian: b0 = bits 15..8, b1 = bits 23..16,
        // b2 = bits 31..24. Lower byte (bits 7..0) is dropped (always zero
        // from arm_float_to_q31 storage anyway).
        packet[3 * i + 0] = (uint8_t)((uint32_t)q31 >> 8);
        packet[3 * i + 1] = (uint8_t)((uint32_t)q31 >> 16);
        packet[3 * i + 2] = (uint8_t)((uint32_t)q31 >> 24);
        r = (r + 1) & RING_MASK;
    }
    tx_read = r;

    if (n_mono_emit < n_mono_want) {
        // Underrun: pad remainder of packet with zeros.
        memset(packet + 3 * n_mono_emit, 0, 3 * (n_mono_want - n_mono_emit));
        usb_audio_f32_tx_underruns++;
    }
    return 3 * n_mono_want;
}

// =====================================================================
// Audio-thread side -- skeleton for M4b classes (AudioInputUSB_F32 /
// AudioOutputUSB_F32) to call from update().
// =====================================================================

extern "C"
bool usb_audio_f32_rx_pop(float *left_out, float *right_out, unsigned int n_samples)
{
    if (n_samples > CONV_TMP_MAX) return false;

    const uint32_t w = rx_write;
    uint32_t r = rx_read;
    const unsigned int n_mono_avail = (w - r) & RING_MASK;
    const unsigned int n_mono_need  = n_samples * 2;

    if (n_mono_avail < n_mono_need) return false;

    static int32_t q31_l[CONV_TMP_MAX];
    static int32_t q31_r[CONV_TMP_MAX];

    for (unsigned int i = 0; i < n_samples; i++) {
        q31_l[i] = rx_ring[r];
        r = (r + 1) & RING_MASK;
        q31_r[i] = rx_ring[r];
        r = (r + 1) & RING_MASK;
    }
    rx_read = r;

    arm_q31_to_float(q31_l, left_out,  n_samples);
    arm_q31_to_float(q31_r, right_out, n_samples);
    return true;
}

extern "C"
bool usb_audio_f32_tx_push(const float *left_in, const float *right_in, unsigned int n_samples)
{
    if (n_samples > CONV_TMP_MAX) return false;

    const uint32_t r = tx_read;
    uint32_t w = tx_write;
    const unsigned int n_mono_avail = RING_SAMPLES - 1 - ((w - r) & RING_MASK);
    const unsigned int n_mono_need  = n_samples * 2;

    if (n_mono_avail < n_mono_need) return false;

    static int32_t q31_l[CONV_TMP_MAX];
    static int32_t q31_r[CONV_TMP_MAX];

    // CMSIS-DSP's arm_float_to_q31 takes a non-const pSrc for legacy
    // reasons; the implementation does not write through it. Cast to
    // satisfy the prototype.
    arm_float_to_q31((float32_t *)left_in,  q31_l, n_samples);
    arm_float_to_q31((float32_t *)right_in, q31_r, n_samples);

    for (unsigned int i = 0; i < n_samples; i++) {
        tx_ring[w] = q31_l[i];
        w = (w + 1) & RING_MASK;
        tx_ring[w] = q31_r[i];
        w = (w + 1) & RING_MASK;
    }
    tx_write = w;
    return true;
}

#endif // AUDIO_SUBSLOT_SIZE == 3 && AUDIO_INTERFACE
