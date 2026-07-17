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
#include "DexedSdBank.h"         // /dexed/*.syx carts off the SD card (thousands of DX7 voices)
#include "DexedVoiceGains.h"
#include "ReplayGain.h"          // shared ILoudnessMeter interface (Tier-1 sweep)

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
#if TDSP_VOICE2
// SPLIT pool: the ReplayGain trim must be PER-HALF, or picking a voice on one synth re-gains
// the other (voice 1 and voice 2 are independent instruments). So each half gets its own trim
// BEFORE the sum: dxpTrim = voice 1 (engines 0-3 / mixA), dxpTrimB = voice 2 (engines 4-7 / mixB).
// The ClipProbe still taps dxpSum and the 'N' sweep stays honest: synthAuditionTrim() returns
// &dxpTrim, so forcing it to unity makes the sum read RAW voice-1 loudness (voice 2 is silent
// during a sweep). dxpSum then feeds the limiter directly — the trims are already applied.
AudioEffectGain_F32 dxpTrim, dxpTrimB;
AudioConnection_F32 cpTrimA(dxpMixA, 0, dxpTrim, 0);
AudioConnection_F32 cpTrimB(dxpMixB, 0, dxpTrimB, 0);
AudioConnection_F32 cps0(dxpTrim, 0, dxpSum, 0), cps1(dxpTrimB, 0, dxpSum, 1);
#else
// UNSPLIT: voice 1 owns all 8 engines, so ONE trim on the summed pool is correct. It sits
// BETWEEN the raw sum and the slot-3 make-up, so normalization happens in a consistent-headroom
// spot and the ClipProbe below still taps the RAW sum (dxpSum) — the sweep must measure
// unnormalized voice loudness, independent of the trim it's computing.
AudioConnection_F32 cps0(dxpMixA, 0, dxpSum, 0), cps1(dxpMixB, 0, dxpSum, 1);
AudioEffectGain_F32 dxpTrim;
AudioConnection_F32 cpTrim(dxpSum, 0, dxpTrim, 0);
#endif

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
#if TDSP_VOICE2
AudioConnection_F32 cpLim(dxpSum, 0, dxpLimit, 0);    // per-half trims already applied pre-sum
#else
AudioConnection_F32 cpLim(dxpTrim, 0, dxpLimit, 0);
#endif
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
#ifdef TDSP_LEAN_RAM
DMAMEM static float g_dxpCapBuf[64];    // @PROOF onset-capture stubbed on lean-RAM builds (frees ~32 KB OCRAM); loudness metering below is unaffected
#else
DMAMEM static float g_dxpCapBuf[8192];
#endif

// Also implements tdsp::ILoudnessMeter so the backend-agnostic runGainSweep() can drive
// it through the shared interface (reset/resetRms/rms/peak below), identical to the lean
// LoudnessProbe_F32 the other backends use — the pool just carries extra diagnostics.
class ClipProbe_F32 : public AudioStream_F32, public tdsp::ILoudnessMeter {
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
    void     reset(void) override    { __disable_irq(); m_clip = 0; m_total = 0; m_peak = 0.0f; resetRmsLocked(); resetKWeight(); __enable_irq(); }
    // Zero just the RMS accumulator (keep peak + K-weight filter state) so the sweep can
    // measure back-to-back short-term windows within one held note without a filter restart.
    void     resetRms(void) override { __disable_irq(); resetRmsLocked(); __enable_irq(); }
    uint32_t clipped(void)  const { return m_clip; }
    uint32_t total(void)    const { return m_total; }
    float    peak(void)     const override { return m_peak; }
    // K-weighted RMS of the synth sum since the last resetRms()/reset() (perceptual loudness).
    float    rms(void)      const override { double n = (double)m_rmsN; return n > 0 ? (float)sqrt(m_sumSq / n) : 0.0f; }
    static constexpr float kRail = 0.999f;   // int16 rail = 32767/32768 ~ 0.99997

    // --- Onset capture: record kCapN samples of the synth sum starting when armed,
    // so the PC can FFT it (aliasing) and inspect note-onset (zero-crossing / step).
    static const int kCapN = (int)(sizeof(g_dxpCapBuf) / sizeof(g_dxpCapBuf[0]));   // follows the buffer (tiny on lean-RAM)
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

#ifndef TDSP_VOICE2
#define TDSP_VOICE2 0
#endif
#if TDSP_VOICE2
// --- Voices 2: split the 8-engine pool 4/4 so a second DEX voice can be driven by a
// separate MIDI source (a USB-host keyboard). Engines 0..3 stay on the main path
// (song player + arp + drums + app); engines 4..7 become an independent sink. A second
// DexedPoolSink over &g_pool[4] owns the top half; main.cpp routes the keyboard to it
// (g_synthSinkB) when the split is enabled at runtime (@VOICE2=1). See planning/synth-voices-2.
static const int kPoolSplitN = kPoolN / 2;   // 4 engines each half
DexedPoolSink    g_poolSinkB(&g_pool[kPoolSplitN], kPoolSplitN, kPoolVpe);
tdsp::MidiSink  *g_synthSinkB = &g_poolSinkB;
// PERMANENT 4/4 split on Voices-2 builds: engines 0..3 = voice 1, 4..7 = voice 2, fixed from
// boot. Static so switching the keyboard's owner is a pure MIDI re-route (main.cpp) that never
// resizes/reloads the pool — voice 1 (song/arp/drums) never drops out on a switch. The cost is
// voice 1 runs at 8-voice poly (4 engines) always, instead of 16 when voice 2 is idle.
static bool      g_voice2Split   = true;     // true = pool is split (main uses engines 0..kPoolSplitN)
static int       g_voice2VolPct  = 100;      // Voices-2 level, 0..150 % (scales the engines-4..7 mix)
#endif

// How many engines the MAIN sink currently spans: the whole pool normally, the low half
// once the Voices-2 split is on (the high half is then the keyboard's).
static inline int poolMainCount() {
#if TDSP_VOICE2
    return g_voice2Split ? kPoolSplitN : kPoolN;
#else
    return kPoolN;
#endif
}

// Flat voice index: [0..320) bundled PROGMEM voices, then [320..320+32*sdBanks)
// = /dexed/*.syx carts off the SD card — identical scheme to SynthBackendDexed.h,
// so the huge open DX7 patch libraries work with the polyphonic/MPE pool too.
static const int kNumBundled = tdsp::dexed::kNumBanks * tdsp::dexed::kVoicesPerBank;  // 320
static int g_synthInstrument = 0;
// Current /dexed cart voice (set by @DXPICK; g_synthInstrument's flat index doesn't cover
// the lazy paged-browser carts). Empty rel = a bundled/flat voice is current instead.
// Read by main.cpp's @STATE so the app can restore exactly which cart voice is loaded.
static char g_curCartRel[160]  = {0};
static int  g_curCartVoice     = -1;
static char g_curCartName[tdsp::dexed::kVoiceNameBufBytes] = {0};

#if TDSP_VOICE2
// Voices-2 selection (engines 4..7), mirrored from the voice-1 fields above so @STATE can
// report exactly what the keyboard half is playing on reconnect.
static int  g_synthInstrument2 = 0;
static char g_curCart2Rel[160] = {0};
static int  g_curCart2Voice    = -1;
static char g_curCart2Name[tdsp::dexed::kVoiceNameBufBytes] = {0};
#endif

static const char *synthName()        { return "Dexed MPE"; }
static const char *synthDescription() { return "6-op FM (DX7), 8-engine pool: per-note bend/pressure in MPE mode, 16-voice poly in normal MIDI. Bundled + /dexed/*.syx carts."; }
static bool        synthIsGM()         { return false; }
static void        synthSetMpeMode(bool mpe) { g_poolSink.setMpeMode(mpe); }
static int         synthNumInstruments()     { return kNumBundled + tdsp::dexed::numSdVoices(); }
static int         synthInstrument()         { return g_synthInstrument; }

FLASHMEM static const char *synthInstrumentName(int i) {
    static char buf[40];
    char vname[tdsp::dexed::kVoiceNameBufBytes];
    if (i < kNumBundled) {
        int bank  = i / tdsp::dexed::kVoicesPerBank;
        int voice = i % tdsp::dexed::kVoicesPerBank;
        if (!tdsp::dexed::copyVoiceName(bank, voice, vname, sizeof(vname))) vname[0] = 0;
        // [V]/[T] tag = patch's LFO natively does vibrato/tremolo.
        uint8_t tags = tdsp::dexed::voiceLfoTags(bank, voice);
        const char *tag = (tags == 3) ? " [V][T]" : (tags & 1) ? " [V]" : (tags & 2) ? " [T]" : "";
        snprintf(buf, sizeof(buf), "%s: %s%s", tdsp::dexed::bankName(bank), vname, tag);
    } else {
        int s     = i - kNumBundled;
        int bank  = s / tdsp::dexed::kVoicesPerBank;
        int voice = s % tdsp::dexed::kVoicesPerBank;
        if (!tdsp::dexed::copySdVoiceName(bank, voice, vname, sizeof(vname))) vname[0] = 0;
        snprintf(buf, sizeof(buf), "%s: %s", tdsp::dexed::sdBankName(bank), vname);
    }
    return buf;
}

// Load a flat/bundled or SD voice into engines [start, start+count) so any engine in
// that window can play the sound (runs from loop/handlers, never the audio ISR). The
// range lets the Voices-2 split load a different voice into each half of the pool.
FLASHMEM static void loadInstrumentRange(int idx, int start, int count) {
    const int total = kNumBundled + tdsp::dexed::numSdVoices();
    if (idx < 0) idx = 0;
    if (idx >= total) idx = total - 1;
    for (int i = start; i < start + count && i < kPoolN; ++i) {
        g_pool[i]->panic();
        if (idx < kNumBundled) {
            tdsp::dexed::loadVoice(*g_pool[i], idx / tdsp::dexed::kVoicesPerBank,
                                               idx % tdsp::dexed::kVoicesPerBank);
        } else {
            int s = idx - kNumBundled;
            tdsp::dexed::loadSdVoice(*g_pool[i], s / tdsp::dexed::kVoicesPerBank,
                                                 s % tdsp::dexed::kVoicesPerBank);
        }
    }
}

// Load a voice into the MAIN pool window (all 8 engines normally; engines 0..3 once the
// Voices-2 split is on, leaving the keyboard's top half untouched).
FLASHMEM static void synthSetInstrument(int idx) {
    const int total = kNumBundled + tdsp::dexed::numSdVoices();
    if (idx < 0) idx = 0;
    if (idx >= total) idx = total - 1;
    loadInstrumentRange(idx, 0, poolMainCount());
    // Re-apply the expression routing (mod-wheel + aftertouch targets / LFO) after loading
    // — loadVoice resets controller + LFO state. Configured via @MODWHEEL / @PRESSURE /
    // @LFOMODE; defaults: mod wheel -> vibrato, pressure -> volume + brightness, force LFO.
    g_poolSink.applyExprConfig();
    g_synthInstrument = idx;
    g_curCartRel[0] = 0; g_curCartVoice = -1;   // a flat/bundled voice is now current (not a @DXPICK cart)
    // ReplayGain per-voice trim only covers the bundled set (baked table). SD carts ship
    // at unity until you sweep them.
    float trim = (idx < kNumBundled) ? dexedVoiceTrim(idx) : 1.0f;
    dxpTrim.setGain(tdsp::auditionTrim(trim));
    Serial.printf("[synth] pool instrument %d = %s (trim=%.3f)\n",
                  idx, synthInstrumentName(idx), (double)trim);
}

// Load an arbitrary /dexed subfolder cart voice into engines [start, start+count).
// Returns the voice's display name (or nullptr on failure).
FLASHMEM static const char *pickCartVoiceRange(const char *relCart, int voice, int start, int count) {
    static char names[tdsp::dexed::kVoicesPerBank][tdsp::dexed::kVoiceNameBufBytes];
    bool ok = true;
    for (int i = start; i < start + count && i < kPoolN; ++i) {
        g_pool[i]->panic();
        if (!tdsp::dexed::sdLoadCartVoice(*g_pool[i], relCart, voice)) ok = false;
    }
    if (!ok) return nullptr;
    int n = tdsp::dexed::sdCartVoiceNames(relCart, names);
    return (n == tdsp::dexed::kVoicesPerBank && voice >= 0 &&
            voice < tdsp::dexed::kVoicesPerBank) ? names[voice] : "";
}

// Pick a cart voice for the MAIN pool window (the paged-browser @DXPICK path). Loads into
// every main-window engine so any can play it; returns the display name (nullptr on fail).
FLASHMEM static const char *synthPickCartVoice(const char *relCart, int voice) {
    const char *nm = pickCartVoiceRange(relCart, voice, 0, poolMainCount());
    if (!nm) return nullptr;
    g_poolSink.applyExprConfig();
    dxpTrim.setGain(tdsp::auditionTrim(1.0f));   // SD carts ship at unity trim
    // Remember it so @STATE can report the exact cart voice back to the app on reconnect.
    snprintf(g_curCartRel, sizeof(g_curCartRel), "%s", relCart);
    g_curCartVoice = voice;
    snprintf(g_curCartName, sizeof(g_curCartName), "%s", nm);
    Serial.printf("[synth] pick %s v%d = %s\n", relCart, voice, nm);
    return nm;
}

#if TDSP_VOICE2
static int g_poolSongPct = 100;   // voice-1 bus level, mirrored from @SONGVOL (see synthSetSongVol)

// Distribute the two independent volumes across the pool so the keyboard voice is NOT
// gated by the main synth's level:
//   * split ON  -> voice 1 (mixA) carries @SONGVOL, voice 2 (mixB) carries ONLY @VOICE2VOL,
//                  and slot 3 stays at the fixed make-up. The two halves are independent.
//   * split OFF -> both mixers unity and slot 3 carries @SONGVOL (the original whole-bus
//                  fader), since all 8 engines are voice 1.
static void applyPoolVols() {
    const float M  = TDSP_DEFAULT_SYNTH_MAKEUP;
    const float sv = g_poolSongPct  / 100.0f;
    const float v2 = g_voice2VolPct / 100.0f;
    if (g_voice2Split) {
        for (int i = 0; i < 4; ++i) { dxpMixA.gain(i, sv); dxpMixB.gain(i, v2); }
        outL.gain(3, M);      outR.gain(3, M);
    } else {
        for (int i = 0; i < 4; ++i) { dxpMixA.gain(i, 1.0f); dxpMixB.gain(i, 1.0f); }
        outL.gain(3, M * sv); outR.gain(3, M * sv);
    }
}
static void applyVoice2Vol() { applyPoolVols(); }   // kept for call sites; the split-aware path

// @SONGVOL hook: on Voices-2 pool builds, main.cpp routes the synth-bus fader here so it can
// be applied split-aware (voice 1 only when the pool is split) instead of the shared slot 3.
static void synthSetSongVol(int pct) { g_poolSongPct = pct; applyPoolVols(); }

// Load the Voices-2 instrument into the keyboard half (engines 4..7). Mirrors
// synthSetInstrument but targets the top window and configures the second sink.
FLASHMEM static void synthSetInstrument2(int idx) {
    const int total = kNumBundled + tdsp::dexed::numSdVoices();
    if (idx < 0) idx = 0;
    if (idx >= total) idx = total - 1;
    loadInstrumentRange(idx, kPoolSplitN, kPoolSplitN);
    g_poolSinkB.applyExprConfig();
    g_synthInstrument2 = idx;
    g_curCart2Rel[0] = 0; g_curCart2Voice = -1;
    // Voice 2's OWN ReplayGain trim (mirrors synthSetInstrument for voice 1). It lands on
    // dxpTrimB — the voice-2 half only — so picking here never re-gains voice 1.
    float trim2 = (idx < kNumBundled) ? dexedVoiceTrim(idx) : 1.0f;
    dxpTrimB.setGain(tdsp::auditionTrim(trim2));
    applyVoice2Vol();
    Serial.printf("[synth] VOICE2 instrument %d = %s (trim=%.3f)\n", idx, synthInstrumentName(idx), (double)trim2);
}

FLASHMEM static const char *synthPickCartVoice2(const char *relCart, int voice) {
    const char *nm = pickCartVoiceRange(relCart, voice, kPoolSplitN, kPoolSplitN);
    if (!nm) return nullptr;
    g_poolSinkB.applyExprConfig();
    dxpTrimB.setGain(tdsp::auditionTrim(1.0f));   // SD carts ship at unity trim (mirrors voice 1)
    snprintf(g_curCart2Rel, sizeof(g_curCart2Rel), "%s", relCart);
    g_curCart2Voice = voice;
    snprintf(g_curCart2Name, sizeof(g_curCart2Name), "%s", nm);
    applyVoice2Vol();
    Serial.printf("[synth] VOICE2 pick %s v%d = %s\n", relCart, voice, nm);
    return nm;
}

// Re-apply voice 2's Tier-1 trim under the CURRENT ReplayGain state (the @RG toggle). Recomputes
// the gain only — no engine reload — so it neither cuts voice 2's sounding notes nor clobbers a
// picked cart selection (which re-selecting the instrument would). Mirrors the trim rules above:
// a /dexed cart ships at unity; a bundled voice uses the baked table.
static void synthReapplyVoice2Trim() {
    const float t = g_curCart2Rel[0]                    ? 1.0f
                  : (g_synthInstrument2 < kNumBundled)  ? dexedVoiceTrim(g_synthInstrument2)
                                                        : 1.0f;
    dxpTrimB.setGain(tdsp::auditionTrim(t));
}

static void synthSetVoice2Vol(int pct) {
    if (pct < 0) pct = 0; if (pct > 150) pct = 150;
    g_voice2VolPct = pct;
    applyVoice2Vol();
}

// Switch the keyboard's owner. The pool is PERMANENTLY split (synthBegin), so this never
// resizes the main window or reloads voice 1 — voice 1's song/arp/drums keep sounding through
// the switch. All we do is clear the keyboard's own engines (4..7) so its notes start/stop
// cleanly; the actual re-route (and releasing held notes on the sink being left) is in main.cpp.
FLASHMEM static void synthSetVoice2Enabled(bool on) {
    g_poolSinkB.panic();
    Serial.printf("[synth] keyboard -> %s\n", on ? "Voices 2 (engines 4..7)" : "Voices 1 (main)");
}
#endif  // TDSP_VOICE2

FLASHMEM static void synthBegin() {
    // Scan /dexed/*.syx off the SD card (bundled voices always present; SD adds
    // thousands more). Create the folder if missing so it's a visible USB drop target.
    if (g_sdReady) {
        if (!SD.exists("/dexed")) SD.mkdir("/dexed");
        int sdBanks = tdsp::dexed::scanSdBanks();
        Serial.printf("[synth] Dexed pool: %d bundled + %d SD voices (%d /dexed carts)\n",
                      kNumBundled, tdsp::dexed::numSdVoices(), sdBanks);
    }
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
    synthSetInstrument(g_synthInstrument);   // voice 1 -> engines 0..poolMainCount()
#if TDSP_VOICE2
    // Arm the permanent split: cap the main sink to engines 0..3 and preload voice 2 into 4..7
    // so the keyboard voice is ready instantly and switching its owner never reshapes the pool.
    g_poolSink.setEngineCount(kPoolSplitN);
    synthSetInstrument2(g_synthInstrument2);
    applyVoice2Vol();
#endif
}

// --- ReplayGain hooks (see REPLAYGAIN.md) ------------------------------------
// The pool is single-timbre, so Tier-1 (audition) fully covers it; there is no Tier-2
// song norm. dxpClip is the loudness meter, dxpTrim the audition bus gain, and the baked
// per-voice table lives in DexedVoiceGains.h.
#define TDSP_HAS_REPLAYGAIN 1
static tdsp::ILoudnessMeter *synthLoudness()     { return &dxpClip; }
static AudioEffectGain_F32  *synthAuditionTrim() { return &dxpTrim; }
static float                 synthVoiceTrim(int idx) { return dexedVoiceTrim(idx); }
static const char           *synthTrimSymbol()   { return "kDexedVoiceTrim"; }
