// AudioSynthYmfmOPLLPool.cpp — see header. N independent YM2413 chips, one note each,
// summed through one resampler, for true per-note MPE (bend + pressure + timbre).

#include "AudioSynthYmfmOPLLPool.h"
#include <math.h>

#ifndef AudioNoInterrupts
#define AudioNoInterrupts() (NVIC_DISABLE_IRQ(IRQ_SOFTWARE))
#define AudioInterrupts()   (NVIC_ENABLE_IRQ(IRQ_SOFTWARE))
#endif

// How far CC#74 swings the modulator Total Level around the patch's own value (in $02
// units, ~0.75 dB each). timbre 1.0 = brightest (TL down by this), 0.0 = darkest (up).
static constexpr int kTimbreDepth = 24;

static inline int16_t clamp16(int32_t v) {
    if (v > 32767)  return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

AudioSynthYmfmOPLLPool::AudioSynthYmfmOPLLPool() : AudioStream(0, nullptr) {
    for (int s = 0; s < kMaxSlots; s++) {
        m_active[s] = false; m_note[s] = 0; m_vel[s] = 100;
        m_baseNote[s] = 0.0f; m_bend[s] = 0.0f; m_pressure[s] = 0.0f; m_timbre[s] = 0.5f;
        m_reg2x[s] = 0; m_keyOn[s] = false; m_idle[s] = 0;
        for (int i = 0; i < 8; i++) m_patch[s][i] = 0;
    }
}

FLASHMEM void AudioSynthYmfmOPLLPool::begin() {
    for (int s = 0; s < kMaxSlots; s++) {
        m_slot[s].chip.reset();
        AudioNoInterrupts();
        writeReg(s, 0x0E, 0x00);   // rhythm off (pool is melodic only)
        writeReg(s, 0x20, 0x00);   // key off, block 0 (channel 0)
        writeReg(s, 0x30, 0x0F);   // instrument 0 (user voice), silent
        AudioInterrupts();
        m_active[s] = false; m_idle[s] = 0; m_keyOn[s] = false; m_reg2x[s] = 0;
    }
    // Resample ratio (native/graph). All chips share it.
    uint32_t nativeRate = m_slot[0].chip.sample_rate(kClockHz);
    m_ratio = (float)nativeRate / (float)AUDIO_SAMPLE_RATE_EXACT;
    m_pos = 0.0f;
    m_prevSum = m_curSum = 0;
    m_ready = true;
}

// MIDI note (float) -> OPLL 9-bit F-number + 3-bit block (same as the single-chip engine).
void AudioSynthYmfmOPLLPool::computeFB(float noteF, uint32_t &fnum, int &block) {
    if (noteF < 0.0f) noteF = 0.0f;
    float freq = 440.0f * powf(2.0f, (noteF - 69.0f) / 12.0f);
    for (block = 0; block <= 7; block++) {
        double fn = (double)freq * (double)(1UL << (19 - block)) / (double)kNativeRate;
        if (fn < 511.0) {
            int v = (int)(fn + 0.5);
            if (v < 0) v = 0; else if (v > 511) v = 511;
            fnum = (uint32_t)v;
            return;
        }
    }
    block = 7; fnum = 511;
}

// velocity + per-note pressure -> 4-bit attenuation (0 = loudest).
uint8_t AudioSynthYmfmOPLLPool::volNibble(int s) const {
    int v = (int)m_vel[s] + (int)(m_pressure[s] * 127.0f + 0.5f);
    if (v > 127) v = 127;
    int att = (127 - v) * 13 / 127;
    if (att < 0) att = 0; else if (att > 15) att = 15;
    return (uint8_t)att;
}

// patch modulator Total Level, brightened/darkened by timbre (CC#74). Preserves the
// patch's KSL bits ($02 top two). timbre 0.5 = the patch as-authored.
uint8_t AudioSynthYmfmOPLLPool::modTL(int s) const {
    uint8_t base = m_patch[s][2];
    int ksl  = base & 0xC0;
    int tl   = base & 0x3F;
    tl += (int)((0.5f - m_timbre[s]) * 2.0f * (float)kTimbreDepth);   // bright -> lower TL
    if (tl < 0) tl = 0; else if (tl > 63) tl = 63;
    return (uint8_t)(ksl | tl);
}

void AudioSynthYmfmOPLLPool::applyPitch(int s) {
    float nf = m_baseNote[s] + m_bend[s];
    uint32_t fnum; int block;
    computeFB(nf, fnum, block);
    writeReg(s, 0x10, (uint8_t)(fnum & 0xFF));
    uint8_t r2 = (uint8_t)(((fnum >> 8) & 0x01) | ((block & 0x07) << 1) | (m_keyOn[s] ? 0x10 : 0x00));
    m_reg2x[s] = r2;
    writeReg(s, 0x20, r2);
}

void AudioSynthYmfmOPLLPool::applyVolume(int s) { writeReg(s, 0x30, volNibble(s)); }   // inst 0 (user) | vol
void AudioSynthYmfmOPLLPool::applyTimbre(int s) { writeReg(s, 0x02, modTL(s)); }

void AudioSynthYmfmOPLLPool::slotNoteOn(int slot, uint8_t note, uint8_t vel, const uint8_t patch[8]) {
    if (!m_ready || slot < 0 || slot >= m_n) return;
    for (int i = 0; i < 8; i++) m_patch[slot][i] = patch[i];
    m_note[slot] = note; m_vel[slot] = vel; m_baseNote[slot] = (float)note;
    // Neutral expression for a fresh note; the sink applies the channel's live values next.
    m_bend[slot] = 0.0f; m_pressure[slot] = 0.0f; m_timbre[slot] = 0.5f;
    m_keyOn[slot] = false;

    AudioNoInterrupts();
    // Load the 8-byte user voice ($00..$07); $02 gets the timbre-modulated TL.
    for (uint8_t r = 0; r < 8; r++) writeReg(slot, r, patch[r]);
    applyTimbre(slot);                       // override $02 with modulated modulator TL
    applyVolume(slot);                       // $30 = inst 0 | volume
    applyPitch(slot);                        // $10/$20, key still off
    // Clock this chip once so it samples KON=0 before we raise KON=1 (clean re-attack).
    { ymfm::ym2413::output_data tmp; m_slot[slot].chip.generate(&tmp, 1); }
    m_keyOn[slot] = true;
    uint8_t r2 = (uint8_t)(m_reg2x[slot] | 0x10);
    m_reg2x[slot] = r2;
    writeReg(slot, 0x20, r2);
    AudioInterrupts();

    m_active[slot] = true; m_idle[slot] = 0;
}

void AudioSynthYmfmOPLLPool::slotNoteOff(int slot) {
    if (slot < 0 || slot >= m_n) return;
    AudioNoInterrupts();
    m_keyOn[slot] = false;
    uint8_t r2 = (uint8_t)(m_reg2x[slot] & ~0x10);
    m_reg2x[slot] = r2;
    writeReg(slot, 0x20, r2);
    AudioInterrupts();
    m_active[slot] = false;
    m_idle[slot] = kReleaseBlocks;           // keep rendering the release tail
}

void AudioSynthYmfmOPLLPool::slotBend(int slot, float semitones) {
    if (slot < 0 || slot >= m_n) return;
    m_bend[slot] = semitones;
    if (m_active[slot]) { AudioNoInterrupts(); applyPitch(slot); AudioInterrupts(); }
}

void AudioSynthYmfmOPLLPool::slotPressure(int slot, float amount) {
    if (slot < 0 || slot >= m_n) return;
    if (amount < 0.0f) amount = 0.0f; else if (amount > 1.0f) amount = 1.0f;
    m_pressure[slot] = amount;
    if (m_active[slot]) { AudioNoInterrupts(); applyVolume(slot); AudioInterrupts(); }
}

void AudioSynthYmfmOPLLPool::slotTimbre(int slot, float amount) {
    if (slot < 0 || slot >= m_n) return;
    if (amount < 0.0f) amount = 0.0f; else if (amount > 1.0f) amount = 1.0f;
    m_timbre[slot] = amount;
    if (m_active[slot]) { AudioNoInterrupts(); applyTimbre(slot); AudioInterrupts(); }
}

void AudioSynthYmfmOPLLPool::allOff() {
    AudioNoInterrupts();
    for (int s = 0; s < kMaxSlots; s++) {
        m_keyOn[s] = false;
        uint8_t r2 = (uint8_t)(m_reg2x[s] & ~0x10);
        m_reg2x[s] = r2;
        writeReg(s, 0x20, r2);
        m_active[s] = false; m_idle[s] = 0;
    }
    AudioInterrupts();
}

void AudioSynthYmfmOPLLPool::update(void) {
    if (activeSlots() == 0) return;          // nothing sounding: emit no block (silence downstream)

    audio_block_t *blockL = allocate();
    audio_block_t *blockR = allocate();
    if (!blockL || !blockR) {
        if (blockL) release(blockL);
        if (blockR) release(blockR);
        return;
    }

    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
        m_pos += m_ratio;
        while (m_pos >= 1.0f) {
            m_prevSum = m_curSum;
            int32_t sum = 0;
            for (int s = 0; s < m_n; s++) {
                if (m_active[s] || m_idle[s]) {
                    ymfm::ym2413::output_data tmp;
                    m_slot[s].chip.generate(&tmp, 1);
                    sum += tmp.data[0] + tmp.data[1];
                }
            }
            m_curSum = sum;
            m_pos -= 1.0f;
        }
        int32_t v = m_prevSum + (int32_t)((m_curSum - m_prevSum) * m_pos);
        v = (int32_t)(v * m_gain);
        int16_t o = clamp16(v);
        blockL->data[i] = o;
        blockR->data[i] = o;
    }

    // Age out release tails (one block elapsed).
    for (int s = 0; s < m_n; s++) if (!m_active[s] && m_idle[s]) m_idle[s]--;

    transmit(blockL, 0);
    transmit(blockR, 1);
    release(blockL);
    release(blockR);
}
