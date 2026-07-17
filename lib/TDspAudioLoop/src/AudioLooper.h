// AudioLooper.h — stereo/mono audio loop recorder as an AudioStream_F32 node.
//
// PROTOTYPE (2026-07, overnight design run — see planning/audio-looper/DESIGN.md).
// The audio-domain sibling of tdsp::MidiLooper: captures the device's actual digital
// audio and loops it, bar-locked to the master Clock, with a CLICK-FREE seam.
//
// Instantiate ONE per loop track (N independent loops = N instances + N buffers).
//
// Signal: 2 F32 inputs (the record bus / selected source, EXCLUDING this looper's own
// return, so overdub never feeds back) -> 2 F32 outputs (the loop return, summed into
// the final mix downstream). Storage is int16 in a caller-owned buffer:
//   * stereo (default): interleaved L,R  -> 192 KB/s @ 48 kHz
//   * mono   (setMono): one channel      ->  96 KB/s (2x the loop length per byte)
//
// Smooth loops = equal-power overlap-crossfade: record loopFrames + kXfade frames; the
// tail is the audio that continued PAST the loop point. On playback the first kXfade
// frames blend head (fade in) with that tail (fade out) — sin/cos, so no value/slope
// step at the wrap. Clock-follow resamples playback to track master tempo (pitch shifts
// with tempo; toggle off to free-run at pitch).
//
// Threading: update() runs in the audio ISR; poll()/transport run in foreground loop().
#pragma once
#include <Arduino.h>
#include <math.h>
#include <AudioStream_F32.h>
#include <Clock.h>

#ifndef AUDIO_SAMPLE_RATE_EXACT
#define AUDIO_SAMPLE_RATE_EXACT 44100.0f
#endif

namespace tdsp {

class AudioLooper : public AudioStream_F32 {
public:
    enum State : uint8_t { Idle = 0, Armed = 1, Recording = 2, Overdub = 3, Playing = 4 };

    static constexpr uint32_t kXfade = 256;   // seam crossfade length in frames (~5.3 ms @ 48k)

    AudioLooper() : AudioStream_F32(2, inputQueueArray_) {}

    // buf: caller-owned int16 buffer of (channels()*capFrames) samples — stereo needs
    // 2*capFrames, mono needs capFrames. DMAMEM (no PSRAM) or EXTMEM (PSRAM). Set mono
    // BEFORE begin() so capacity/allocation match. Must outlive the looper.
    void begin(tdsp::Clock *clk, int16_t *buf, uint32_t capFrames) {
        clk_ = clk; buf_ = buf; capFrames_ = capFrames;
        sr_ = AUDIO_SAMPLE_RATE_EXACT;
        if (!s_fadeInit_) {                               // equal-power sin/cos ramps, shared by all loops
            for (uint32_t k = 0; k < kXfade; ++k) {
                float t = (kXfade > 1) ? (float)k / (float)(kXfade - 1) : 1.0f;
                s_fadeIn_[k]  = sinf(0.5f * (float)M_PI * t);
                s_fadeOut_[k] = cosf(0.5f * (float)M_PI * t);
            }
            s_fadeInit_ = true;
        }
        state_ = Idle; writePos_ = loopFrames_ = 0; pos_ = 0.0;
    }

    // ---- Configuration ----
    void setMono(bool m)               { mono_ = m; ch_ = m ? 1 : 2; }   // set before begin()
    void setBars(uint8_t b)            { if (b == 1 || b == 2 || b == 4 || b == 8) bars_ = b; }
    void setReturnLevel(float g)       { returnLevel_ = g < 0 ? 0 : (g > 1 ? 1 : g); }
    void setClockFollow(bool on)       { clockFollow_ = on; }
    uint8_t  bars()        const { return bars_; }
    bool     mono()        const { return mono_; }
    bool     clockFollow() const { return clockFollow_; }
    uint8_t  channels()    const { return ch_; }
    State    state()     const { return state_; }
    bool     hasClip()   const { return loopFrames_ > 0; }
    float    loopSeconds() const { return loopFrames_ / sr_; }
    float    capSeconds()  const { return (capFrames_ > kXfade ? capFrames_ - kXfade : 0) / sr_; }
    // For SD/WAV save (AudioLoopWav.h): the loop BODY, channels, rate.
    const int16_t *buffer() const { return buf_; }
    uint32_t loopFrames()   const { return loopFrames_; }
    float    sampleRate()   const { return sr_; }

    // ---- Transport (foreground) ----
    void armRecord() { __disable_irq(); state_ = Armed; haveArmBar_ = false; __enable_irq(); }
    void armOverdub() {
        if (loopFrames_ == 0) { armRecord(); return; }
        __disable_irq(); state_ = Overdub; __enable_irq();
    }
    void stop() {
        __disable_irq();
        if (state_ == Recording)    finalizeToPlay();
        else if (state_ == Overdub) state_ = Playing;
        else                        state_ = Idle;
        __enable_irq();
    }
    void clear() { __disable_irq(); state_ = Idle; loopFrames_ = 0; writePos_ = 0; pos_ = 0.0; __enable_irq(); }
    void resume() { if (state_ == Idle && loopFrames_ > 0) { __disable_irq(); pos_ = 0.0; state_ = Playing; __enable_irq(); } }

    // ---- Per-loop() servicing (foreground) ----
    void poll() {
        // snapshot the clock-follow playback rate for the ISR (avoids per-sample division)
        rate_ = (clockFollow_ && recordedBpm_ > 0.0f && clk_)
                ? clampRate(clk_->bpm() / recordedBpm_) : 1.0f;
        if (!clk_ || state_ != Armed || !clk_->running()) return;
        const double pb = clk_->positionBeats();
        uint32_t bpb = clk_->beatsPerBar(); if (!bpb) bpb = 4;
        const uint32_t bar = (uint32_t)floor(pb / (double)bpb);
        if (!haveArmBar_) { armBar_ = bar; haveArmBar_ = true; }   // count-in bar, then
        else if (bar > armBar_) beginRecording();                  // start on the downbeat
    }

    uint16_t positionPermille() const {
        if (loopFrames_ == 0) return 0;
        if (state_ == Recording) {
            uint32_t w = writePos_; if (w > loopFrames_) w = loopFrames_;
            return (uint16_t)((uint64_t)w * 1000u / loopFrames_);
        }
        if (state_ == Playing || state_ == Overdub)
            return (uint16_t)((uint64_t)pos_ * 1000u / loopFrames_);
        return 0;
    }

    // ---- Audio ISR ----
    void update() override {
        audio_block_f32_t *inL = receiveReadOnly_f32(0);
        audio_block_f32_t *inR = receiveReadOnly_f32(1);
        audio_block_f32_t *oL  = allocate_f32();
        audio_block_f32_t *oR  = allocate_f32();
        if (!oL || !oR) { if (oL) release(oL); if (oR) release(oR);
                          if (inL) release(inL); if (inR) release(inR); return; }

        const State st = state_;
        const float rate = rate_;
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            const float xl = inL ? inL->data[i] : 0.0f;
            const float xr = inR ? inR->data[i] : 0.0f;
            float yl = 0.0f, yr = 0.0f;

            if (st == Recording) {
                uint32_t wp = writePos_;
                if (wp < loopFrames_ + kXfade && wp < capFrames_) { storeFrame(wp, xl, xr); writePos_ = wp + 1; }
                if (writePos_ >= loopFrames_ + kXfade || writePos_ >= capFrames_) finalizeToPlay();
            } else if (st == Playing || st == Overdub) {
                readInterp(pos_, yl, yr);                   // crossfade + resample read
                if (st == Overdub) {                        // sum input at the nearest frame (+ tail mirror)
                    uint32_t f = (uint32_t)(pos_ + 0.5); if (f >= loopFrames_) f = 0;
                    addFrame(f, xl, xr);
                    if (f < kXfade) addFrame(loopFrames_ + f, xl, xr);
                }
                yl *= returnLevel_; yr *= returnLevel_;
                pos_ += rate;
                if (pos_ >= (double)loopFrames_) pos_ -= (double)loopFrames_;
            }
            oL->data[i] = yl; oR->data[i] = yr;
        }

        transmit(oL, 0); transmit(oR, 1);
        release(oL); release(oR);
        if (inL) release(inL); if (inR) release(inR);
    }

private:
    static inline int16_t f2s(float x) {
        int32_t s = (int32_t)lrintf(x * 32767.0f);
        return (int16_t)(s > 32767 ? 32767 : (s < -32768 ? -32768 : s));
    }
    static inline float s2f(int16_t s) { return (float)s * (1.0f / 32768.0f); }
    static inline float clampRate(float r) { return r < 0.25f ? 0.25f : (r > 4.0f ? 4.0f : r); }

    void storeFrame(uint32_t f, float xl, float xr) {
        if (ch_ == 1) buf_[f] = f2s(0.5f * (xl + xr));
        else { buf_[2 * f] = f2s(xl); buf_[2 * f + 1] = f2s(xr); }
    }
    void addFrame(uint32_t f, float xl, float xr) {
        if (f >= capFrames_) return;
        if (ch_ == 1) buf_[f] = clampAdd(buf_[f], f2s(0.5f * (xl + xr)));
        else { buf_[2 * f] = clampAdd(buf_[2 * f], f2s(xl)); buf_[2 * f + 1] = clampAdd(buf_[2 * f + 1], f2s(xr)); }
    }
    static inline int16_t clampAdd(int16_t a, int16_t b) {
        int32_t s = (int32_t)a + (int32_t)b;
        return (int16_t)(s > 32767 ? 32767 : (s < -32768 ? -32768 : s));
    }

    // Stored frame -> stereo (mono duplicated), no crossfade.
    void frameStereo(uint32_t f, float &l, float &r) const {
        if (ch_ == 1) { l = r = s2f(buf_[f]); }
        else { l = s2f(buf_[2 * f]); r = s2f(buf_[2 * f + 1]); }
    }
    // Loop frame with equal-power head/tail crossfade in the first kXfade frames.
    void loopFrameXfade(uint32_t f, float &l, float &r) const {
        if (f < kXfade && loopFrames_ > kXfade) {
            float hl, hr, tl, tr;
            frameStereo(f, hl, hr);
            frameStereo(loopFrames_ + f, tl, tr);          // recorded tail past the loop end
            const float fi = s_fadeIn_[f], fo = s_fadeOut_[f];
            l = hl * fi + tl * fo; r = hr * fi + tr * fo;
        } else frameStereo(f, l, r);
    }
    // Fractional (resampled) read: linear-interp between two crossfaded loop frames.
    void readInterp(double p, float &sl, float &sr) const {
        uint32_t pi = (uint32_t)p; if (pi >= loopFrames_) pi = 0;
        const float frac = (float)(p - (double)pi);
        float al, ar, bl, br;
        loopFrameXfade(pi, al, ar);
        uint32_t pn = pi + 1; if (pn >= loopFrames_) pn = 0;
        loopFrameXfade(pn, bl, br);
        sl = al + (bl - al) * frac; sr = ar + (br - ar) * frac;
    }

    void beginRecording() {
        uint32_t bpb = clk_->beatsPerBar(); if (!bpb) bpb = 4;
        float bpm = clk_->bpm(); if (bpm <= 0.0f) bpm = 120.0f;
        const double spb  = (double)sr_ * 60.0 / (double)bpm;
        double       lf   = spb * (double)bpb * (double)bars_;
        uint32_t     frames = (uint32_t)(lf + 0.5);
        const uint32_t maxBody = (capFrames_ > kXfade) ? capFrames_ - kXfade : 0;
        if (frames > maxBody) frames = maxBody;
        recordedBpm_ = bpm;
        __disable_irq();
        loopFrames_ = frames; writePos_ = 0; state_ = frames ? Recording : Idle;
        __enable_irq();
    }
    void finalizeToPlay() { state_ = Playing; pos_ = 0.0; }   // IRQs already masked

    audio_block_f32_t *inputQueueArray_[2];
    tdsp::Clock *clk_ = nullptr;
    int16_t     *buf_ = nullptr;
    uint32_t     capFrames_ = 0;
    float        sr_ = AUDIO_SAMPLE_RATE_EXACT;

    volatile State    state_      = Idle;
    volatile uint32_t writePos_   = 0;
    volatile double   pos_        = 0.0;   // fractional playback frame
    volatile uint32_t loopFrames_ = 0;     // body length; crossfade tail at [loopFrames_, +kXfade)
    volatile float    rate_       = 1.0f;  // playback rate (clock-follow), set in poll()

    uint8_t  ch_          = 2;     // 1 = mono, 2 = stereo
    bool     mono_        = false;
    uint8_t  bars_        = 4;
    float    returnLevel_ = 1.0f;
    bool     clockFollow_ = true;  // track master tempo by default (decision #3)
    float    recordedBpm_ = 120.0f;

    bool     haveArmBar_ = false;
    uint32_t armBar_     = 0;

    // Equal-power crossfade ramps — identical for every loop, so share one copy
    // (C++17 inline statics) instead of 2 KB per instance.
    static inline float s_fadeIn_[kXfade]  = {};
    static inline float s_fadeOut_[kXfade] = {};
    static inline bool  s_fadeInit_ = false;
};

}  // namespace tdsp
