// ReplayGain.h — shared loudness-normalization building blocks for the mix-kit synths.
//
// See REPLAYGAIN.md for the full design. Two tiers:
//   Tier 1 (audition): one bus AudioEffectGain_F32 keyed to the picker instrument index.
//   Tier 2 (song norm): per-channel gain re-applied on Program Change (multitimbral only).
//
// This header supplies the pieces every backend shares so the system is defined ONCE:
//   * ILoudnessMeter  — the interface the generic 'N' gain sweep (runGainSweep) drives.
//   * LoudnessProbe_F32 — a reusable K-weighted (ITU-R BS.1770) meter tapping a raw sum.
//   * gmProgramTrim()  — Tier-2 per-GM-program table accessor (bounds-checked, unity dflt).
//
// A backend opts in by defining TDSP_HAS_REPLAYGAIN and exposing synthLoudness() /
// synthAuditionTrim() / synthVoiceTrim() / synthTrimSymbol() (see REPLAYGAIN.md).
#pragma once
#include <AudioEffectGain_F32.h>   // pulls in AudioStream_F32 / audio_block_f32_t / AUDIO_BLOCK_SAMPLES
#include <math.h>
#include <stdint.h>

namespace tdsp {

// The minimal interface the gain sweep needs from a backend's loudness meter. Both the
// lean LoudnessProbe_F32 below and the Dexed pool's richer ClipProbe_F32 implement it, so
// runGainSweep() is backend-agnostic. Called only from loop context (never the ISR), so
// virtual dispatch is free here.
class ILoudnessMeter {
public:
    virtual ~ILoudnessMeter() = default;
    virtual void  reset()     = 0;   // clear RMS accumulator, peak, and K-weight filter state
    virtual void  resetRms()  = 0;   // clear ONLY the RMS accumulator (keep peak + filter state)
    virtual float rms() const = 0;   // K-weighted RMS since last reset()/resetRms()
    virtual float peak() const= 0;   // raw peak since last reset()
};

// ITU-R BS.1770 K-weighting biquad coefficients @ 48 kHz (this build runs 48k).
// Stage 1 = high-shelf, stage 2 = RLB high-pass (b = {1,-2,1}). Shared by every meter so
// all backends measure loudness identically.
struct KWeight48k {
    static constexpr float B0_1 =  1.53512485958697f, B1_1 = -2.69169618940638f, B2_1 = 1.19839281085285f;
    static constexpr float A1_1 = -1.69065929318241f, A2_1 =  0.73248077421585f;
    static constexpr float A1_2 = -1.99004745483398f, A2_2 =  0.99007225036621f;   // stage2 b={1,-2,1}
};

// A lean K-weighted loudness + raw-peak meter. Tap it onto a backend's RAW synth sum
// (before the audition trim and mix make-up) so the sweep measures unnormalized voice
// loudness. This is the meter for backends that don't need the pool's full click/onset
// diagnostics (OPLL, TSF, OPL3, OPM); the Dexed pool keeps its own ClipProbe_F32.
class LoudnessProbe_F32 : public AudioStream_F32, public ILoudnessMeter {
public:
    LoudnessProbe_F32(void) : AudioStream_F32(1, inputQueueArray) {}

    void update(void) override {
        audio_block_f32_t *b = receiveReadOnly_f32(0);
        if (!b) return;
        float pk = 0.0f;
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
            float s = b->data[i];
            float m = s < 0 ? -s : s;
            if (m > pk) pk = m;
            // K-weight (perceptual loudness); peak stays on the RAW signal.
            float w1 = KWeight48k::B0_1 * s + KWeight48k::B1_1 * m_k1x1 + KWeight48k::B2_1 * m_k1x2
                       - KWeight48k::A1_1 * m_k1y1 - KWeight48k::A2_1 * m_k1y2;
            m_k1x2 = m_k1x1; m_k1x1 = s; m_k1y2 = m_k1y1; m_k1y1 = w1;
            float w2 = w1 - 2.0f * m_k2x1 + m_k2x2 - KWeight48k::A1_2 * m_k2y1 - KWeight48k::A2_2 * m_k2y2;
            m_k2x2 = m_k2x1; m_k2x1 = w1; m_k2y2 = m_k2y1; m_k2y1 = w2;
            m_sumSq += (double)w2 * (double)w2; m_rmsN++;
        }
        if (pk > m_peak) m_peak = pk;
        AudioStream_F32::release(b);
    }

    // --- ILoudnessMeter ---
    void  reset(void) override    { __disable_irq(); resetRmsLocked(); m_peak = 0.0f; resetKWeight(); __enable_irq(); }
    void  resetRms(void) override { __disable_irq(); resetRmsLocked(); __enable_irq(); }
    float rms(void) const override{ double n = (double)m_rmsN; return n > 0 ? (float)sqrt(m_sumSq / n) : 0.0f; }
    float peak(void) const override { return m_peak; }

private:
    void resetRmsLocked(void) { m_sumSq = 0.0; m_rmsN = 0; }
    void resetKWeight(void)   { m_k1x1 = m_k1x2 = m_k1y1 = m_k1y2 = 0.0f;
                                m_k2x1 = m_k2x2 = m_k2y1 = m_k2y2 = 0.0f; }

    audio_block_f32_t *inputQueueArray[1];
    volatile double   m_sumSq = 0.0;
    volatile uint32_t m_rmsN  = 0;
    volatile float    m_peak  = 0.0f;
    float m_k1x1 = 0, m_k1x2 = 0, m_k1y1 = 0, m_k1y2 = 0;   // stage1 state
    float m_k2x1 = 0, m_k2x2 = 0, m_k2y1 = 0, m_k2y2 = 0;   // stage2 state
};

// Tier-2 per-GM-program trim accessor. `table` is a 128-entry array of linear gains
// (1.0 = unity); a sink calls this on Program Change and multiplies the channel's gain.
// Bounds-checked so a stray program byte can't index out of range.
static inline float gmProgramTrim(const float *table, int program) {
    if (!table || program < 0 || program > 127) return 1.0f;
    return table[program];
}

}  // namespace tdsp
