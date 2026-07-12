// AudioSynthYmfmOPM.cpp — see header. Wraps ymfm::ym2151 as a Teensy audio source.

#include "AudioSynthYmfmOPM.h"
#include "OpmPitch.h"    // encodeOpmPitch() + kOpmNote — shared, host-testable

// AudioNoInterrupts()/AudioInterrupts() live in Audio.h (not AudioStream.h); they
// just gate the audio-update software IRQ so a register write can't be preempted
// mid-sequence. Define them here (guarded) to keep this library from pulling in
// the whole of Audio.h. IRQ_SOFTWARE / NVIC_* come from the Teensy core.
#ifndef AudioNoInterrupts
#define AudioNoInterrupts() (NVIC_DISABLE_IRQ(IRQ_SOFTWARE))
#define AudioInterrupts()   (NVIC_ENABLE_IRQ(IRQ_SOFTWARE))
#endif

using namespace tdsp::ymfmopm;

static inline int16_t clamp16(int32_t v) {
    if (v > 32767)  return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

AudioSynthYmfmOPM::AudioSynthYmfmOPM()
    : AudioStream(0, nullptr), m_chip(m_intf) {
    for (int i = 0; i < kNumChannels; i++) {
        m_note[i] = -1; m_age[i] = 0; m_srcChan[i] = 0; m_velTl[i] = 0;
    }
}

void AudioSynthYmfmOPM::begin() {
    m_chip.reset();

    // Native chip output rate is clock/64; resample ratio = how many chip samples
    // to advance per one AUDIO_SAMPLE_RATE output sample (>1 here => downsampling).
    uint32_t nativeRate = m_chip.sample_rate(kClockHz);
    m_ratio = (float)nativeRate / (float)AUDIO_SAMPLE_RATE_EXACT;
    m_pos = 0.0f;

    // Prime the resampler taps so the first update() has two valid samples.
    m_cur.clear();
    m_chip.generate(&m_cur, 1);
    m_prev = m_cur;

    // Load a default patch on every channel so any note sounds immediately.
    setVoice(kAdditiveOrgan);
}

// Write one OpmVoice's per-channel + per-operator registers for channel `ch`.
// Operator slot s (0..3) lives at register base + s*8 + ch.
void AudioSynthYmfmOPM::applyVoiceToChannel(int ch, const OpmVoice &v) {
    // 0x20+ch: pan L+R on (0xC0), feedback (bits 3-5), algorithm (bits 0-2).
    writeReg(0x20 + ch, 0xC0 | ((v.feedback & 7) << 3) | (v.alg & 7));
    for (int s = 0; s < 4; s++) {
        const OpmOp &o = v.op[s];
        uint8_t off = (uint8_t)(s * 8 + ch);
        writeReg(0x40 + off, ((o.dt1 & 7) << 4) | (o.mul & 15));       // DT1 | MUL
        writeReg(0x60 + off, o.tl & 0x7f);                            // TL
        writeReg(0x80 + off, ((o.ks & 3) << 6) | (o.ar & 31));        // KS | AR
        writeReg(0xA0 + off, ((o.ams & 1) << 7) | (o.d1r & 31));      // AM-en | D1R
        writeReg(0xC0 + off, ((o.dt2 & 3) << 6) | (o.d2r & 31));      // DT2 | D2R
        writeReg(0xE0 + off, ((o.sl & 15) << 4) | (o.rr & 15));       // SL | RR
    }
}

void AudioSynthYmfmOPM::setVoice(const OpmVoice &voice) {
    m_voiceStore = voice;        // keep our own copy (caller's may be a temporary)
    m_haveVoice = true;
    AudioNoInterrupts();
    for (int ch = 0; ch < kNumChannels; ch++) applyVoiceToChannel(ch, m_voiceStore);
    AudioInterrupts();
}

// Pick a channel for a new note: first free one, else steal the oldest-sounding.
int AudioSynthYmfmOPM::allocChannel(uint8_t note) {
    for (int ch = 0; ch < kNumChannels; ch++)
        if (m_note[ch] < 0) { m_note[ch] = note; m_age[ch] = ++m_ageCounter; return ch; }
    int oldest = 0;
    for (int ch = 1; ch < kNumChannels; ch++)
        if (m_age[ch] < m_age[oldest]) oldest = ch;
    m_note[oldest] = note;
    m_age[oldest] = ++m_ageCounter;
    return oldest;
}

// Sum of this note's channel bend and (in MPE) the master channel's bend, which
// applies to every note. Non-MPE path: srcCh 0, masterChan 0 -> just m_bend[0].
float AudioSynthYmfmOPM::effectiveBend(uint8_t srcCh) const {
    float b = m_bend[srcCh <= 16 ? srcCh : 0];
    if (m_masterChan && srcCh != m_masterChan) b += m_bend[m_masterChan];
    return b;
}

// Write KC/KF for one sounding OPM channel from its base note + effective bend.
void AudioSynthYmfmOPM::applyPitch(int ch) {
    if (m_note[ch] < 0) return;
    uint8_t kc, kf;
    encodeOpmPitch((float)m_note[ch] + effectiveBend(m_srcChan[ch]), kc, kf);
    writeReg(0x28 + ch, kc & 0x7f);           // key code (block + note)
    writeReg(0x30 + ch, (uint8_t)(kf << 2));  // key fraction (6 bits in [7:2])
}

// Write the four operator TLs for one channel from the voice, plus velocity, plus
// per-channel expression: pressure loudens the carriers (C1/C2 = slots 2,3),
// timbre (CC74) brightens the modulators (M1/M2 = slots 0,1). The carrier/
// modulator split is the VOPM naming heuristic — exact for the common algorithms;
// on the all-carrier alg 7 "timbre" simply trims two of the four carriers.
void AudioSynthYmfmOPM::applyExpression(int ch) {
    uint8_t sc = m_srcChan[ch] <= 16 ? m_srcChan[ch] : 0;
    int pressAtt  = (int)(m_press[sc]  * kPressDepth);   // subtracted from carriers -> louder
    int timbreAtt = (int)(m_timbre[sc] * kTimbreDepth);  // subtracted from modulators -> brighter
    for (int s = 0; s < 4; s++) {
        int tl = (int)m_voiceStore.op[s].tl + (int)m_velTl[ch];
        tl -= (s >= 2) ? pressAtt : timbreAtt;           // slots 2,3 carriers; 0,1 modulators
        if (tl < 0) tl = 0; else if (tl > 127) tl = 127;
        writeReg(0x60 + (uint8_t)(s * 8 + ch), (uint8_t)tl);
    }
}

void AudioSynthYmfmOPM::noteOnCh(uint8_t srcCh, uint8_t note, uint8_t vel) {
    if (vel == 0) { noteOffCh(srcCh, note); return; }
    if (!m_haveVoice) return;
    AudioNoInterrupts();
    int ch = allocChannel(note);
    m_srcChan[ch] = srcCh;
    m_velTl[ch] = (uint8_t)(((127 - vel) * kVelDepth) / 127);   // low velocity attenuates
    writeReg(0x08, ch);            // key OFF first (clean envelope retrigger)
    applyPitch(ch);                // KC/KF from note + bend
    applyExpression(ch);           // per-op TL from voice + velocity + pressure/timbre
    writeReg(0x08, 0x78 | ch);     // key ON, all four operators
    AudioInterrupts();
}

void AudioSynthYmfmOPM::noteOffCh(uint8_t srcCh, uint8_t note) {
    AudioNoInterrupts();
    for (int ch = 0; ch < kNumChannels; ch++)
        if (m_note[ch] == (int8_t)note && m_srcChan[ch] == srcCh) {
            writeReg(0x08, ch);    // key OFF (all operators)
            m_note[ch] = -1;
        }
    AudioInterrupts();
}

void AudioSynthYmfmOPM::allNotesOff() {
    AudioNoInterrupts();
    for (int ch = 0; ch < kNumChannels; ch++) { writeReg(0x08, ch); m_note[ch] = -1; }
    AudioInterrupts();
}

void AudioSynthYmfmOPM::allNotesOffCh(uint8_t srcCh) {
    AudioNoInterrupts();
    for (int ch = 0; ch < kNumChannels; ch++)
        if (m_srcChan[ch] == srcCh && m_note[ch] >= 0) { writeReg(0x08, ch); m_note[ch] = -1; }
    AudioInterrupts();
}

// --- per-channel expression: restate the affected sounding channels ----------
void AudioSynthYmfmOPM::pitchBend(uint8_t srcCh, float semitones) {
    if (srcCh > 16) return;
    m_bend[srcCh] = semitones;
    bool master = (m_masterChan && srcCh == m_masterChan);   // master bend -> all notes
    AudioNoInterrupts();
    for (int ch = 0; ch < kNumChannels; ch++)
        if (m_note[ch] >= 0 && (master || m_srcChan[ch] == srcCh)) applyPitch(ch);
    AudioInterrupts();
}

void AudioSynthYmfmOPM::pressure(uint8_t srcCh, float value) {
    if (srcCh > 16) return;
    m_press[srcCh] = value;
    bool master = (m_masterChan && srcCh == m_masterChan);
    AudioNoInterrupts();
    for (int ch = 0; ch < kNumChannels; ch++)
        if (m_note[ch] >= 0 && (master || m_srcChan[ch] == srcCh)) applyExpression(ch);
    AudioInterrupts();
}

void AudioSynthYmfmOPM::timbre(uint8_t srcCh, float value) {
    if (srcCh > 16) return;
    m_timbre[srcCh] = value;
    bool master = (m_masterChan && srcCh == m_masterChan);
    AudioNoInterrupts();
    for (int ch = 0; ch < kNumChannels; ch++)
        if (m_note[ch] >= 0 && (master || m_srcChan[ch] == srcCh)) applyExpression(ch);
    AudioInterrupts();
}

void AudioSynthYmfmOPM::update(void) {
    // Idle gate: when nothing is held, keep rendering for kIdleHoldBlocks (so
    // release tails finish), then emit nothing — transmitting no block reads as
    // silence downstream and skips the chip entirely, so a silent bank is free.
    if (activeVoices() == 0) {
        if (m_idleBlocks >= kIdleHoldBlocks) return;    // fully idle: no output, no work
        m_idleBlocks++;
    } else {
        m_idleBlocks = 0;
    }

    audio_block_t *blockL = allocate();
    audio_block_t *blockR = allocate();
    if (!blockL || !blockR) {                     // out of audio blocks: bail cleanly
        if (blockL) release(blockL);
        if (blockR) release(blockR);
        return;
    }

    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
        // Advance the fractional read position; consume whole chip samples as the
        // position crosses each integer, leaving m_pos in [0,1) between prev/cur.
        m_pos += m_ratio;
        while (m_pos >= 1.0f) {
            m_prev = m_cur;
            m_chip.generate(&m_cur, 1);
            m_pos -= 1.0f;
        }
        float f = m_pos;
        int32_t l = m_prev.data[0] + (int32_t)((m_cur.data[0] - m_prev.data[0]) * f);
        int32_t r = m_prev.data[1] + (int32_t)((m_cur.data[1] - m_prev.data[1]) * f);
        if (m_gain != 1.0f) { l = (int32_t)(l * m_gain); r = (int32_t)(r * m_gain); }
        blockL->data[i] = clamp16(l);
        blockR->data[i] = clamp16(r);
    }

    transmit(blockL, 0);
    transmit(blockR, 1);
    release(blockL);
    release(blockR);
}
