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
#include "DexedPoolSink.h"
#include "DexedVoiceBank.h"

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
// mono sum -> both mix channels (slot 3), same make-up gain path as the single engine
AudioConnection_F32 cpoutL(dxpSum, 0, outL, 3);
AudioConnection_F32 cpoutR(dxpSum, 0, outR, 3);

// --- Clip probe (diagnostic) -------------------------------------------------
// Taps the raw synth SUM, BEFORE the 0.62 mix make-up in setup(). That ordering
// matters: a DX7 patch with a punchy attack (e.g. a pizz) can flat-top at each
// engine's float->int16 conversion (the setGain(0.8) rail); the mix then scales
// it DOWN to 0.62, so the final-bus peak meter (peakOut) reads a clean-looking
// ~0.62 while the waveform is already clipped. Here we see the railing directly:
// samples pinned at |x|>=kRail are per-engine int16 flat-topping.
class ClipProbe_F32 : public AudioStream_F32 {
public:
    ClipProbe_F32(void) : AudioStream_F32(1, inputQueueArray) {}
    void update(void) override {
        audio_block_f32_t *b = receiveReadOnly_f32(0);
        if (!b) return;
        uint32_t clip = 0; float pk = 0.0f;
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
            float m = b->data[i]; if (m < 0) m = -m;
            if (m > pk)     pk = m;
            if (m >= kRail) clip++;
        }
        m_clip += clip; m_total += AUDIO_BLOCK_SAMPLES;
        if (pk > m_peak) m_peak = pk;
        if (m_arm) {                                     // record synth-sum samples for offline FFT/onset analysis
            for (int i = 0; i < AUDIO_BLOCK_SAMPLES && m_capIdx < kCapN; i++)
                m_cap[m_capIdx++] = b->data[i];
            if (m_capIdx >= kCapN) m_arm = false;
        }
        AudioStream_F32::release(b);
    }
    void     reset(void)    { __disable_irq(); m_clip = 0; m_total = 0; m_peak = 0.0f; __enable_irq(); }
    uint32_t clipped(void)  const { return m_clip; }
    uint32_t total(void)    const { return m_total; }
    float    peak(void)     const { return m_peak; }
    static constexpr float kRail = 0.999f;   // int16 rail = 32767/32768 ~ 0.99997

    // --- Onset capture: record kCapN samples of the synth sum starting when armed,
    // so the PC can FFT it (aliasing) and inspect note-onset (zero-crossing / step).
    static const int kCapN = 8192;           // ~171 ms @ 48 kHz
    void         armCapture(void)  { __disable_irq(); m_capIdx = 0; m_arm = true; __enable_irq(); }
    bool         captureDone(void) const { return !m_arm; }
    int          captureCount(void) const { return m_capIdx; }
    const float *capture(void)     const { return m_cap; }
private:
    audio_block_f32_t *inputQueueArray[1];
    volatile uint32_t m_clip = 0, m_total = 0;
    volatile float    m_peak = 0.0f;
    volatile bool     m_arm = false;
    volatile int      m_capIdx = 0;
    float             m_cap[kCapN];
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
    Serial.printf("[synth] pool instrument %d = %s (bank %d voice %d)\n", idx, synthInstrumentName(idx), bank, voice);
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
