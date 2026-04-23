// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.
//
// Looper — mono sample-accurate looper as an AudioStream node.
//
// What it does
// ------------
// Captures incoming samples into a caller-owned buffer, then plays them
// back on a loop. One mono input, one mono output. The output carries
// only the loop contribution; live monitoring is the caller's job (wire
// the source into the main mix separately and sum the looper's output
// into the same bus).
//
// Buffer ownership
// ----------------
// The sketch allocates the buffer — typically as DMAMEM for the stock
// Teensy 4.1 (no-PSRAM) build or EXTMEM once PSRAM is populated — and
// passes a pointer + length in samples. Keeping storage-class agnostic
// here makes the DMAMEM -> EXTMEM swap a one-line sketch edit.
//
// State model
// -----------
//   Idle        empty; record() clears and starts capture
//   Recording   input writes into buffer at writePos; length grows
//   Playing     output loops over [0..length); input is discarded
//   Stopped     have a take, not playing; output is silent
// clear() -> Idle and resets length to 0.
//
// Overflow
// --------
// If Recording reaches the end of the buffer, the take is auto-finalized
// at capacity and the state drops into Stopped. We do NOT wrap on
// overflow — that would silently erase the start of the take, which is
// surprising and hard to recover from.
//
// Thread safety
// -------------
// Transport calls (record/play/stop/clear) are expected from foreground
// (OSC handler). The audio ISR reads state and positions in update().
// Multi-field transitions are guarded by __disable_irq / __enable_irq
// so the ISR never catches half-written state.

#pragma once

#include <stdint.h>
#include <Arduino.h>      // pulls imxrt.h (F_CPU_ACTUAL, NVIC_SET_PENDING);
                          // AudioStream.h references these inline.
#include <AudioStream.h>

namespace tdsp {

class Looper : public AudioStream {
public:
    enum State : uint8_t {
        Idle      = 0,
        Recording = 1,
        Playing   = 2,
        Stopped   = 3,
    };

    // buffer: caller-owned int16 sample buffer; MUST remain valid for
    //   the lifetime of the Looper instance.
    // bufferSamples: number of int16_t elements in `buffer`.
    Looper(int16_t *buffer, uint32_t bufferSamples);

    void update() override;

    // Transport. Safe to call from foreground (OSC handler).
    void record();   // -> Recording (writePos=0, length=0)
    void play();     // -> Playing  (playPos=0; finalizes length if coming from Recording)
    void stop();     // -> Stopped  (keeps buffer); -> Idle if nothing was ever recorded
    void clear();    // -> Idle     (length=0)

    // Output gain applied to playback samples (q15 internally). Does not
    // affect the buffer contents. 0..1, clamped.
    void setReturnLevel(float g01);

    // Read-back — snapshot / echo helpers. All return values are safe
    // to read from foreground; the ISR may write them concurrently but
    // single-word reads are atomic on Cortex-M7.
    State    state()           const { return (State)_state; }
    uint32_t lengthSamples()   const { return _lengthSamples; }
    uint32_t capacitySamples() const { return _capacity; }
    float    lengthSeconds()   const;
    float    capacitySeconds() const;
    float    returnLevel()     const { return _returnLevel; }

private:
    int16_t * const    _buffer;
    const uint32_t     _capacity;
    volatile uint32_t  _writePos       = 0;
    volatile uint32_t  _playPos        = 0;
    volatile uint32_t  _lengthSamples  = 0;
    volatile uint8_t   _state          = Idle;
    float              _returnLevel    = 1.0f;
    int32_t            _returnScaleQ15 = 32767;
    audio_block_t     *_inputQueueArray[1];
};

}  // namespace tdsp
