// SynthBackendDexedPool.h — MPE-capable Dexed backend: a POOL of AudioSynthDexed
// engines instead of one, so per-note pitch bend / pressure work in MPE mode.
//
// Interchangeable with SynthBackendDexed.h (same synth* interface); selected by the
// TDSP_SYNTH_DEXED_POOL build flag. Included by main.cpp AFTER the outL/outR mixers
// exist, so the pool's summed output can feed mix slot 3.
//
// Audio graph (kPoolN engines, each int16 mono, converted to F32 and summed):
//   dxp0..7 --keydown--> [Dexed] --I16toF32--> dxpMixA/B (4 each) --> dxpSum --> outL/outR[3]
//
// The DexedPoolSink allocates the engines: one-per-note in MPE mode (real per-note
// expression), round-robin for polyphony in normal MIDI. See DexedPoolSink.h.
//
// Dexed is a MONO int16 engine: each engine bridges to F32 and its note goes to both
// output channels via the shared sum mixer.
#pragma once
#include <synth_dexed.h>
#include <AudioEffectGain_F32.h>
#include <AudioEffectCompressor2_F32.h>
#include "DexedPoolSink.h"
#include "DexedVoiceBank.h"
#include "DexedVoiceGains.h"

// Pool size / per-engine polyphony. kPoolN engines * kPoolVpe voices = normal-mode
// polyphony; kPoolN = max simultaneous MPE notes (one engine each). Starting point
// for the CPU-fit measurement — tune to the heartbeat's cpuMax.
static const int kPoolN   = 8;
static const int kPoolVpe = 2;

AudioSynthDexed dxp0(kPoolVpe, AUDIO_SAMPLE_RATE_EXACT), dxp1(kPoolVpe, AUDIO_SAMPLE_RATE_EXACT),
                dxp2(kPoolVpe, AUDIO_SAMPLE_RATE_EXACT), dxp3(kPoolVpe, AUDIO_SAMPLE_RATE_EXACT),
                dxp4(kPoolVpe, AUDIO_SAMPLE_RATE_EXACT), dxp5(kPoolVpe, AUDIO_SAMPLE_RATE_EXACT),
                dxp6(kPoolVpe, AUDIO_SAMPLE_RATE_EXACT), dxp7(kPoolVpe, AUDIO_SAMPLE_RATE_EXACT);

AudioConvert_I16toF32 dxpc0, dxpc1, dxpc2, dxpc3, dxpc4, dxpc5, dxpc6, dxpc7;

// engine (int16 mono) -> F32 bridge
AudioConnection cpk0(dxp0, 0, dxpc0, 0), cpk1(dxp1, 0, dxpc1, 0),
                cpk2(dxp2, 0, dxpc2, 0), cpk3(dxp3, 0, dxpc3, 0),
                cpk4(dxp4, 0, dxpc4, 0), cpk5(dxp5, 0, dxpc5, 0),
                cpk6(dxp6, 0, dxpc6, 0), cpk7(dxp7, 0, dxpc7, 0);

// two 4-input F32 mixers sum the 8 engines, then a third sums those two
AudioMixer4_F32 dxpMixA, dxpMixB, dxpSum;
AudioConnection_F32 cpa0(dxpc0, 0, dxpMixA, 0), cpa1(dxpc1, 0, dxpMixA, 1),
                    cpa2(dxpc2, 0, dxpMixA, 2), cpa3(dxpc3, 0, dxpMixA, 3);
AudioConnection_F32 cpb0(dxpc4, 0, dxpMixB, 0), cpb1(dxpc5, 0, dxpMixB, 1),
                    cpb2(dxpc6, 0, dxpMixB, 2), cpb3(dxpc7, 0, dxpMixB, 3);
AudioConnection_F32 cps0(dxpMixA, 0, dxpSum, 0), cps1(dxpMixB, 0, dxpSum, 1);
// Per-voice ReplayGain trim (set on instrument select) sits BETWEEN the raw sum and
// the slot-3 make-up, so normalization happens in a consistent-headroom spot and the
// ClipProbe below still taps the RAW sum (dxpSum) — the sweep must measure unnormalized
// voice loudness, independent of the trim it's computing.
AudioEffectGain_F32 dxpTrim;
AudioConnection_F32 cpTrim(dxpSum, 0, dxpTrim, 0);

// Soft-limiter safety net on the synth bus, AFTER the ReplayGain trim and BEFORE the
// mix — the last stage that can catch coincident loud voices summing past full scale
// (what made JUPITER "snap" on dense passages; per-voice ReplayGain can't prevent
// stacking). Stateless tanh soft-knee: y = x below the threshold (transparent), then a
// C1-continuous saturation asymptoting to a ceiling. No attack/release state -> no
// pumping and no ISR hardfault (unlike AudioEffectCompressor2_F32, the reason the
// earlier bus limiter was pulled).
class SoftLimit_F32 : public AudioStream_F32 {
public:
    SoftLimit_F32(void) : AudioStream_F32(1, inputQueueArray) {}
    void setThreshold(float t) { m_thresh = t; }
    void setCeiling(float c)   { m_ceil = c; }
    float threshold(void) const { return m_thresh; }
    float ceiling(void)   const { return m_ceil; }
    void update(void) override {
        audio_block_f32_t *b = receiveWritable_f32(0);
        if (!b) return;
        const float t = m_thresh, span = m_ceil - m_thresh;
        if (span > 0.0f) {
            for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
                float x = b->data[i];
                float a = x < 0 ? -x : x;
                if (a > t) {                                   // slope 1 at the knee -> transparent hand-off
                    a = t + span * tanhf((a - t) / span);      // asymptotes to m_ceil, never exceeds
                    b->data[i] = x < 0 ? -a : a;
                }
            }
        }
        transmit(b, 0);
        AudioStream_F32::release(b);
    }
private:
    audio_block_f32_t *inputQueueArray[1];
    volatile float m_thresh = 0.80f, m_ceil = 1.00f;   // synth bus caps at 1.0 -> x0.62 make-up = 0.62 in the mix
};
SoftLimit_F32 dxpLimit;
AudioConnection_F32 cpLim(dxpTrim, 0, dxpLimit, 0);
// limited synth bus -> both mix channels (slot 3; 0.62 make-up applied at the mixer)
AudioConnection_F32 cpoutL(dxpLimit, 0, outL, 3);
AudioConnection_F32 cpoutR(dxpLimit, 0, outR, 3);

// --- Clip probe (diagnostic) -------------------------------------------------
// Taps the raw synth SUM, BEFORE the 0.62 mix make-up in setup(). That ordering
// matters: a DX7 patch with a punchy attack (e.g. a pizz) can flat-top at each
// engine's float->int16 conversion (the setGain(0.8) rail); the mix then scales
// it DOWN to 0.62, so the final-bus peak meter (peakOut) reads a clean-looking
// ~0.62 while the waveform is already clipped. Here we see the railing directly:
// samples pinned at |x|>=kRail are per-engine int16 flat-topping.
// Onset-capture buffer lives in DMAMEM (RAM2), NOT as a class member — a 32 KB array
// in RAM1 leaves almost no stack, and float printf from the audio path then overflows
// it (hard fault). Must match ClipProbe_F32::kCapN.
DMAMEM static float g_dxpCapBuf[8192];

class ClipProbe_F32 : public AudioStream_F32 {
public:
    ClipProbe_F32(void) : AudioStream_F32(1, inputQueueArray) {}
    void update(void) override {
        audio_block_f32_t *b = receiveReadOnly_f32(0);
        if (!b) return;
        uint32_t clip = 0; float pk = 0.0f;
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
            float s = b->data[i];
            float m = s < 0 ? -s : s;
            if (m > pk)     pk = m;
            if (m >= kRail) clip++;
            // K-weighted (ITU-R BS.1770) loudness for the gain sweep: a high-shelf +
            // high-pass so the RMS tracks PERCEIVED loudness (ear is ~+4 dB hotter at
            // 2-5 kHz), not flat energy. Bright FM patches no longer read "quiet". Peak
            // and clip above stay on the RAW signal (they're for clip protection).
            float w1 = kKB0_1 * s + kKB1_1 * m_k1x1 + kKB2_1 * m_k1x2 - kKA1_1 * m_k1y1 - kKA2_1 * m_k1y2;
            m_k1x2 = m_k1x1; m_k1x1 = s;  m_k1y2 = m_k1y1; m_k1y1 = w1;
            float w2 = w1 - 2.0f * m_k2x1 + m_k2x2 - kKA1_2 * m_k2y1 - kKA2_2 * m_k2y2;   // stage2 b={1,-2,1}
            m_k2x2 = m_k2x1; m_k2x1 = w1; m_k2y2 = m_k2y1; m_k2y1 = w2;
            m_sumSq += (double)w2 * (double)w2; m_rmsN++;   // integrated K-weighted energy
            // --- discontinuity (click / voice-steal) detector ---------------
            // A clean waveform slews smoothly; an abruptly stolen oscillator makes
            // a big single-sample step. Track the worst |x[i]-x[i-1]| and freeze a
            // window around it so the PC can see whether it's a real step (click).
            if (m_haveHist) {
                float dj = s - m_prev; if (dj < 0) dj = -dj;
                if (dj > m_maxJump) m_maxJump = dj;                 // per-second (heartbeat) max
                if (dj > m_worstJump && m_snapFill == 0) {          // new all-time worst -> freeze window
                    m_worstJump = dj;
                    for (int k = 0; k < kPre; k++)                  // preceding context, oldest->newest
                        m_snap[k] = m_ring[(m_ringHead + k) % kPre];
                    m_snapFill = kPre;                              // then collect kPost samples (from s)
                }
            }
            if (m_snapFill > 0 && m_snapFill < kSnapN) {
                m_snap[m_snapFill++] = s;
                if (m_snapFill >= kSnapN) { m_snapValid = true; m_snapFill = 0; }
            }
            m_ring[m_ringHead] = s; m_ringHead = (m_ringHead + 1) % kPre;
            m_prev = s; m_haveHist = true;
            // onset capture (armed single-note)
            if (m_arm && m_capIdx < kCapN) { g_dxpCapBuf[m_capIdx++] = s; if (m_capIdx >= kCapN) m_arm = false; }
        }
        m_clip += clip; m_total += AUDIO_BLOCK_SAMPLES;
        if (pk > m_peak) m_peak = pk;
        AudioStream_F32::release(b);
    }
    void     reset(void)    { __disable_irq(); m_clip = 0; m_total = 0; m_peak = 0.0f; resetRmsLocked(); resetKWeight(); __enable_irq(); }
    // Zero just the RMS accumulator (keep peak + K-weight filter state) so the sweep can
    // measure back-to-back short-term windows within one held note without a filter restart.
    void     resetRms(void) { __disable_irq(); resetRmsLocked(); __enable_irq(); }
    uint32_t clipped(void)  const { return m_clip; }
    uint32_t total(void)    const { return m_total; }
    float    peak(void)     const { return m_peak; }
    // K-weighted RMS of the synth sum since the last resetRms()/reset() (perceptual loudness).
    float    rms(void)      const { double n = (double)m_rmsN; return n > 0 ? (float)sqrt(m_sumSq / n) : 0.0f; }
    static constexpr float kRail = 0.999f;   // int16 rail = 32767/32768 ~ 0.99997

    // --- Onset capture: record kCapN samples of the synth sum starting when armed,
    // so the PC can FFT it (aliasing) and inspect note-onset (zero-crossing / step).
    static const int kCapN = 8192;           // ~171 ms @ 48 kHz
    void         armCapture(void)  { __disable_irq(); m_capIdx = 0; m_arm = true; __enable_irq(); }
    bool         captureDone(void) const { return !m_arm; }
    int          captureCount(void) const { return m_capIdx; }
    const float *capture(void)     const { return g_dxpCapBuf; }

    // --- Discontinuity (click) detector: worst single-sample step + frozen window ---
    static const int kPre = 128, kPost = 128, kSnapN = kPre + kPost;
    float        maxJump(void)   const { return m_maxJump; }         // rolling (heartbeat) peak step
    void         resetPeriod(void)     { m_maxJump = 0.0f; }
    void         resetWorst(void)      { __disable_irq(); m_worstJump = 0.0f; m_snapValid = false; m_snapFill = 0; __enable_irq(); }
    float        worstJump(void) const { return m_worstJump; }
    bool         snapValid(void) const { return m_snapValid; }
    const float *snap(void)      const { return m_snap; }
private:
    void resetRmsLocked(void) { m_sumSq = 0.0; m_rmsN = 0; }
    void resetKWeight(void)   { m_k1x1 = m_k1x2 = m_k1y1 = m_k1y2 = 0.0f;
                                m_k2x1 = m_k2x2 = m_k2y1 = m_k2y2 = 0.0f; }
    // ITU-R BS.1770 K-weighting biquad coefficients @ 48 kHz (this build runs 48k;
    // heartbeat total=48000/s confirms it). Stage1 = high-shelf, stage2 = RLB high-pass.
    static constexpr float kKB0_1 =  1.53512485958697f, kKB1_1 = -2.69169618940638f, kKB2_1 = 1.19839281085285f;
    static constexpr float kKA1_1 = -1.69065929318241f, kKA2_1 =  0.73248077421585f;
    static constexpr float kKA1_2 = -1.99004745483398f, kKA2_2 =  0.99007225036621f;   // stage2 b={1,-2,1}

    audio_block_f32_t *inputQueueArray[1];
    volatile uint32_t m_clip = 0, m_total = 0;
    volatile float    m_peak = 0.0f;
    volatile double   m_sumSq = 0.0;    // sum of squares of the K-weighted signal (gain sweep)
    volatile uint32_t m_rmsN = 0;
    float             m_k1x1 = 0, m_k1x2 = 0, m_k1y1 = 0, m_k1y2 = 0;   // K-weight stage1 state
    float             m_k2x1 = 0, m_k2x2 = 0, m_k2y1 = 0, m_k2y2 = 0;   // K-weight stage2 state
    volatile bool     m_arm = false;
    volatile int      m_capIdx = 0;
    // discontinuity detector state
    float             m_ring[kPre];
    volatile int      m_ringHead = 0;
    float             m_snap[kSnapN];
    volatile int      m_snapFill = 0;
    volatile bool     m_snapValid = false;
    volatile float    m_maxJump = 0.0f, m_worstJump = 0.0f;
    float             m_prev = 0.0f;
    volatile bool     m_haveHist = false;
};

ClipProbe_F32       dxpClip;
AudioConnection_F32 cpClip(dxpSum, 0, dxpClip, 0);

AudioSynthDexed *g_pool[kPoolN] = { &dxp0, &dxp1, &dxp2, &dxp3, &dxp4, &dxp5, &dxp6, &dxp7 };
DexedPoolSink    g_poolSink(g_pool, kPoolN, kPoolVpe);
tdsp::MidiSink  *g_synthSink = &g_poolSink;

// Full DX7 voice set, browsable by bank — identical to SynthBackendDexed.h (index =
// bank * kVoicesPerBank + voice, streamed as "<bankName>: <voiceName>").
static const int kNumInstruments = tdsp::dexed::kNumBanks * tdsp::dexed::kVoicesPerBank;  // 320
static int g_synthInstrument = 0;

static const char *synthName()        { return "Dexed MPE"; }
static const char *synthDescription() { return "6-op FM (DX7), 8-engine pool: per-note bend/pressure in MPE mode, 16-voice poly in normal MIDI."; }
static bool        synthIsGM()         { return false; }
static void        synthSetMpeMode(bool mpe) { g_poolSink.setMpeMode(mpe); }
static int         synthNumInstruments()     { return kNumInstruments; }
static int         synthInstrument()         { return g_synthInstrument; }

static const char *synthInstrumentName(int i) {
    static char buf[32];
    int bank  = i / tdsp::dexed::kVoicesPerBank;
    int voice = i % tdsp::dexed::kVoicesPerBank;
    char vname[tdsp::dexed::kVoiceNameBufBytes];
    if (!tdsp::dexed::copyVoiceName(bank, voice, vname, sizeof(vname))) vname[0] = 0;
    snprintf(buf, sizeof(buf), "%s: %s", tdsp::dexed::bankName(bank), vname);
    return buf;
}

// Load a voice into EVERY engine so any engine can play the current sound (runs from
// loop/handlers, never the audio ISR).
static void synthSetInstrument(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= kNumInstruments) idx = kNumInstruments - 1;
    int bank  = idx / tdsp::dexed::kVoicesPerBank;
    int voice = idx % tdsp::dexed::kVoicesPerBank;
    for (int i = 0; i < kPoolN; ++i) {
        g_pool[i]->panic();
        tdsp::dexed::loadVoice(*g_pool[i], bank, voice);
    }
    g_synthInstrument = idx;
    // ReplayGain-style per-voice loudness trim (baked table in DexedVoiceGains.h). One
    // bus gain for the whole pool is correct: the pool is single-timbre (one voice at a
    // time). 1.0 = unity until the 'N' sweep has been run and its output baked in.
    dxpTrim.setGain(dexedVoiceTrim(idx));
    Serial.printf("[synth] pool instrument %d = %s (bank %d voice %d) trim=%.3f\n",
                  idx, synthInstrumentName(idx), bank, voice, (double)dexedVoiceTrim(idx));
}

static void synthBegin() {
    for (int i = 0; i < kPoolN; ++i) {
        g_pool[i]->setGain(0.8f);       // below unity so punchy notes don't flat-top (see single backend)
        g_pool[i]->setPitchbendRange(2);
        g_pool[i]->setPitchbend((int16_t)0);
        g_pool[i]->setModWheel(0);
        g_pool[i]->setSustain(false);
    }
    // Unity sum across the pool; the mix-slot-3 make-up gain in setup() restores level.
    for (int i = 0; i < 4; ++i) { dxpMixA.gain(i, 1.0f); dxpMixB.gain(i, 1.0f); }
    dxpSum.gain(0, 1.0f); dxpSum.gain(1, 1.0f); dxpSum.gain(2, 0.0f); dxpSum.gain(3, 0.0f);
    synthSetInstrument(g_synthInstrument);
}
