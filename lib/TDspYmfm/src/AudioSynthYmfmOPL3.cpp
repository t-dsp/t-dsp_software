// AudioSynthYmfmOPL3.cpp — see header. Wraps ymfm::ymf262 (OPL3) as a Teensy audio
// source and adds the GM MIDI synth layer (18-voice allocator, per-channel program,
// channel-10 drums) on top of the DMXOPL bank.

#include "AudioSynthYmfmOPL3.h"
#include "dmxopl_genmidi_op2.h"   // kDmxOplGenmidi[] + kDmxOplGenmidiLen (baked default bank)
#include <math.h>

// See OPM wrapper: gate the audio-update software IRQ so a batch of register
// writes can't be torn by the render ISR. Defined here (guarded) to avoid pulling
// in all of Audio.h.
#ifndef AudioNoInterrupts
#define AudioNoInterrupts() (NVIC_DISABLE_IRQ(IRQ_SOFTWARE))
#define AudioInterrupts()   (NVIC_ENABLE_IRQ(IRQ_SOFTWARE))
#endif

using namespace tdsp::ymfmopl;

// How far low velocity attenuates the carrier (0x40 units, ~0.75 dB each).
static constexpr int kVelDepth = 24;

// Per-channel operator offsets within a 9-channel bank.
static const uint8_t kChanOp[9] = { 0x00, 0x01, 0x02, 0x08, 0x09, 0x0A, 0x10, 0x11, 0x12 };
// Operator register bases in mod[]/car[] order.
static const uint8_t kOpReg[5] = { 0x20, 0x40, 0x60, 0x80, 0xE0 };

// The parsed DMXOPL bank (~11 KB) lives in OCRAM (RAM2), not the scarce DTCM: it's
// read at note-on from the loop thread, never the audio ISR, so the slower access
// is fine and it keeps RAM1 stack headroom healthy.
DMAMEM static tdsp::ymfmopl::OplInstrument s_oplBank[tdsp::ymfmopl::kOp2NumInst];

static inline int16_t clamp16(int32_t v) {
    if (v > 32767)  return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

uint8_t AudioSynthYmfmOPL3::modOff(int c) { return kChanOp[c % 9]; }
uint8_t AudioSynthYmfmOPL3::carOff(int c) { return (uint8_t)(kChanOp[c % 9] + 3); }

AudioSynthYmfmOPL3::AudioSynthYmfmOPL3()
    : AudioStream(0, nullptr), m_chip(m_intf) {
    m_inst = s_oplBank;
    for (int c = 0; c < kNumChannels; c++) {
        m_note[c] = -1; m_chan[c] = 0; m_age[c] = 0;
        m_baseNote[c] = 0.0f; m_b0[c] = 0; m_keyOn[c] = false;
    }
    for (int i = 0; i < 17; i++) { m_program[i] = 0; m_bend[i] = 0.0f; m_sustain[i] = false; }
}

FLASHMEM void AudioSynthYmfmOPL3::begin() {
    m_chip.reset();

    AudioNoInterrupts();
    // OPL3 mode MUST be enabled before any bank-1 register write, otherwise the
    // chip masks the high address bit (compatibility mode). 0x105 itself is exempt.
    writeReg(0x105, 0x01);   // NEW = 1: OPL3 enabled (18x 2-op, 4 waveforms, stereo)
    writeReg(0x104, 0x00);   // no 4-op connections: all 18 channels are 2-op
    writeReg(0x08,  0x00);   // NTS = 0
    writeReg(0x01,  0x20);   // WSE = 1: enable waveform select (harmless with NEW=1)
    // Silence and reset every channel.
    for (int c = 0; c < kNumChannels; c++) {
        uint16_t bank = chanBank(c); int lc = chanLocal(c);
        writeReg(bank + 0xB0 + lc, 0x00);   // key off, block 0
        m_note[c] = -1; m_chan[c] = 0; m_age[c] = 0;
        m_baseNote[c] = 0.0f; m_b0[c] = 0; m_keyOn[c] = false;
    }
    AudioInterrupts();

    // Resample ratio: chip native rate (clock/288) per one output sample.
    uint32_t nativeRate = m_chip.sample_rate(kClockHz);
    m_ratio = (float)nativeRate / (float)AUDIO_SAMPLE_RATE_EXACT;
    m_pos   = 0.0f;

    // Prime the resampler taps so the first update() has two valid samples.
    m_cur.clear();
    m_chip.generate(&m_cur, 1);
    m_prev = m_cur;

    for (int i = 1; i <= 16; i++) { m_program[i] = 0; m_bend[i] = 0.0f; m_sustain[i] = false; }

    // Parse the baked DMXOPL bank so the engine is playable with no SD card.
    m_numInst = loadBankOp2(kDmxOplGenmidi, kDmxOplGenmidiLen);
    m_ready   = (m_numInst > 0);
}

// MIDI note (float, includes bend/offsets) -> OPL F-number + block. Picks the
// lowest block whose F-number fits 0..1023 (keeps ~10 bits of precision).
void AudioSynthYmfmOPL3::computeFB(float noteF, uint32_t &fnum, int &block) {
    if (noteF < 0.0f) noteF = 0.0f;
    float freq = 440.0f * powf(2.0f, (noteF - 69.0f) / 12.0f);
    for (block = 0; block <= 7; block++) {
        double fn = (double)freq * (double)(1UL << (20 - block)) / (double)kNativeRate;
        if (fn < 1023.0) {
            int v = (int)(fn + 0.5);
            if (v < 0) v = 0; else if (v > 1023) v = 1023;
            fnum = (uint32_t)v;
            return;
        }
    }
    block = 7; fnum = 1023;
}

uint8_t AudioSynthYmfmOPL3::carrierTL(const OplPatch &p, uint8_t velocity) {
    uint8_t base = (uint8_t)(p.car[1] & 0x3F);   // patch total-level (attenuation)
    uint8_t ksl  = (uint8_t)(p.car[1] & 0xC0);   // key-scale-level bits preserved
    int add = ((127 - (int)velocity) * kVelDepth) / 127;   // low velocity -> more attenuation
    int tl  = (int)base + add;
    if (tl > 63) tl = 63;
    return (uint8_t)(ksl | (uint8_t)tl);
}

// Write a full 2-op voice into OPL channel c, overriding the carrier 0x40 with the
// velocity-scaled total level. Does NOT key on (caller does that after applyPitch).
void AudioSynthYmfmOPL3::loadPatch(int c, const OplPatch &p, uint8_t carTL) {
    uint16_t bank = chanBank(c);
    int lc = chanLocal(c);
    uint8_t mo = modOff(c), co = carOff(c);
    for (int i = 0; i < 5; i++) writeReg(bank + kOpReg[i] + mo, p.mod[i]);
    writeReg(bank + 0x20 + co, p.car[0]);
    writeReg(bank + 0x40 + co, carTL);          // velocity-scaled carrier level
    writeReg(bank + 0x60 + co, p.car[2]);
    writeReg(bank + 0x80 + co, p.car[3]);
    writeReg(bank + 0xE0 + co, p.car[4]);
    // 0xC0: feedback/connection (low nibble) + L/R enable (0x30) for OPL3 stereo.
    writeReg(bank + 0xC0 + lc, (uint8_t)((p.feedbackConn & 0x0F) | 0x30));
}

void AudioSynthYmfmOPL3::applyPitch(int c) {
    if (m_note[c] < 0) return;
    float nf = m_baseNote[c] + m_bend[m_chan[c] <= 16 ? m_chan[c] : 0];
    uint32_t fnum; int block;
    computeFB(nf, fnum, block);
    uint16_t bank = chanBank(c);
    int lc = chanLocal(c);
    writeReg(bank + 0xA0 + lc, (uint8_t)(fnum & 0xFF));
    uint8_t b0 = (uint8_t)(((fnum >> 8) & 0x03) | ((block & 0x07) << 2));
    if (m_keyOn[c]) b0 |= 0x20;
    m_b0[c] = b0;
    writeReg(bank + 0xB0 + lc, b0);
}

void AudioSynthYmfmOPL3::keyOff(int c) {
    uint16_t bank = chanBank(c);
    int lc = chanLocal(c);
    m_keyOn[c] = false;
    uint8_t b0 = (uint8_t)(m_b0[c] & ~0x20);
    m_b0[c] = b0;
    writeReg(bank + 0xB0 + lc, b0);
}

int AudioSynthYmfmOPL3::allocChannel(uint8_t midiCh, uint8_t note) {
    for (int c = 0; c < kNumChannels; c++)
        if (m_note[c] < 0) {
            m_note[c] = (int8_t)note; m_chan[c] = midiCh; m_age[c] = ++m_ageCounter;
            return c;
        }
    int oldest = 0;
    for (int c = 1; c < kNumChannels; c++)
        if (m_age[c] < m_age[oldest]) oldest = c;
    keyOff(oldest);
    m_note[oldest] = (int8_t)note; m_chan[oldest] = midiCh; m_age[oldest] = ++m_ageCounter;
    return oldest;
}

void AudioSynthYmfmOPL3::noteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (!m_ready || m_numInst == 0) return;
    if (velocity == 0) { noteOff(channel, note); return; }
    if (channel < 1 || channel > 16) return;

    const OplInstrument *ins;
    uint8_t pitchNote;
    if (channel == 10) {
        int idx = 128 + (int)note - 35;
        if (idx < 128) idx = 128;
        if (idx >= m_numInst) idx = m_numInst - 1;
        if (idx < 0) return;
        ins = &m_inst[idx];
        pitchNote = (ins->flags & 0x01) ? ins->fixedNote : note;
    } else {
        int idx = m_program[channel];
        if (idx < 0 || idx >= m_numInst) idx = 0;
        ins = &m_inst[idx];
        pitchNote = (ins->flags & 0x01) ? ins->fixedNote : note;
    }
    const OplPatch &p = ins->voice[0];

    AudioNoInterrupts();
    int c = allocChannel(channel, note);
    m_baseNote[c] = (float)pitchNote + (float)p.noteOffset;
    m_keyOn[c] = false;
    loadPatch(c, p, carrierTL(p, velocity));
    applyPitch(c);                       // 0xA0/0xB0 with key still off
    m_keyOn[c] = true;                   // now key on
    uint16_t bank = chanBank(c); int lc = chanLocal(c);
    uint8_t b0 = (uint8_t)(m_b0[c] | 0x20);
    m_b0[c] = b0;
    writeReg(bank + 0xB0 + lc, b0);
    AudioInterrupts();
}

void AudioSynthYmfmOPL3::noteOff(uint8_t channel, uint8_t note) {
    if (channel < 1 || channel > 16) return;
    AudioNoInterrupts();
    for (int c = 0; c < kNumChannels; c++)
        if (m_note[c] == (int8_t)note && m_chan[c] == channel) {
            keyOff(c);
            m_note[c] = -1;
        }
    AudioInterrupts();
}

void AudioSynthYmfmOPL3::programChange(uint8_t channel, uint8_t program) {
    if (channel < 1 || channel > 16) return;
    m_program[channel] = (program < kNumMelodic) ? program : (uint8_t)(kNumMelodic - 1);
}

void AudioSynthYmfmOPL3::pitchBend(uint8_t channel, float semitones) {
    if (channel < 1 || channel > 16) return;
    m_bend[channel] = semitones;
    AudioNoInterrupts();
    for (int c = 0; c < kNumChannels; c++)
        if (m_note[c] >= 0 && m_chan[c] == channel) applyPitch(c);
    AudioInterrupts();
}

void AudioSynthYmfmOPL3::controlChange(uint8_t channel, uint8_t cc, uint8_t v) {
    if (channel < 1 || channel > 16) return;
    switch (cc) {
        case 64:                         // sustain pedal (stored; hold not implemented)
            m_sustain[channel] = (v >= 64);
            break;
        case 120:                        // all sound off
        case 123:                        // all notes off (this channel)
            AudioNoInterrupts();
            for (int c = 0; c < kNumChannels; c++)
                if (m_note[c] >= 0 && m_chan[c] == channel) { keyOff(c); m_note[c] = -1; }
            AudioInterrupts();
            break;
        default:
            break;                       // mod wheel / volume: no-op for now
    }
}

void AudioSynthYmfmOPL3::allNotesOff() {
    AudioNoInterrupts();
    for (int c = 0; c < kNumChannels; c++) {
        if (m_note[c] >= 0) keyOff(c);
        m_note[c] = -1;
    }
    AudioInterrupts();
}

FLASHMEM const char *AudioSynthYmfmOPL3::melodicName(int gm) const {
    if (gm < 0 || gm >= kNumMelodic || gm >= m_numInst) return "";
    return m_inst[gm].name;
}

FLASHMEM int AudioSynthYmfmOPL3::loadBankOp2(const uint8_t *data, size_t len) {
    int n = parseOp2Bank(data, len, m_inst, kOp2NumInst);
    if (n > 0) { m_numInst = n; m_ready = true; }
    return n;
}

FLASHMEM int AudioSynthYmfmOPL3::loadBankWopl(const uint8_t *data, size_t len) {
    (void)data; (void)len;
    return -1;   // WOPL parsing not implemented yet; keep the baked OP2 bank.
}

void AudioSynthYmfmOPL3::update(void) {
    // Idle gate: once nothing is held, keep rendering release tails for
    // kIdleHoldBlocks, then emit nothing (silence downstream, chip skipped).
    if (activeVoices() == 0) {
        if (m_idleBlocks >= kIdleHoldBlocks) return;
        m_idleBlocks++;
    } else {
        m_idleBlocks = 0;
    }

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
            m_prev = m_cur;
            m_chip.generate(&m_cur, 1);
            m_pos -= 1.0f;
        }
        float f = m_pos;
        // OPL3 has four outputs; fold to stereo: L = ch0+ch2, R = ch1+ch3.
        int32_t pL = m_prev.data[0] + m_prev.data[2];
        int32_t pR = m_prev.data[1] + m_prev.data[3];
        int32_t cL = m_cur.data[0]  + m_cur.data[2];
        int32_t cR = m_cur.data[1]  + m_cur.data[3];
        int32_t l = pL + (int32_t)((cL - pL) * f);
        int32_t r = pR + (int32_t)((cR - pR) * f);
        if (m_gain != 1.0f) { l = (int32_t)(l * m_gain); r = (int32_t)(r * m_gain); }
        blockL->data[i] = clamp16(l);
        blockR->data[i] = clamp16(r);
    }

    transmit(blockL, 0);
    transmit(blockR, 1);
    release(blockL);
    release(blockR);
}
