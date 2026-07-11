// AudioSynthYmfmOPM.cpp — see header. Wraps ymfm::ym2151 as a Teensy audio source.

#include "AudioSynthYmfmOPM.h"

// AudioNoInterrupts()/AudioInterrupts() live in Audio.h (not AudioStream.h); they
// just gate the audio-update software IRQ so a register write can't be preempted
// mid-sequence. Define them here (guarded) to keep this library from pulling in
// the whole of Audio.h. IRQ_SOFTWARE / NVIC_* come from the Teensy core.
#ifndef AudioNoInterrupts
#define AudioNoInterrupts() (NVIC_DISABLE_IRQ(IRQ_SOFTWARE))
#define AudioInterrupts()   (NVIC_ENABLE_IRQ(IRQ_SOFTWARE))
#endif

using namespace tdsp::ymfmopm;

// MIDI semitone (0=C .. 11=B) -> OPM key-code low nibble. The OPM divides each
// octave into 12 of 16 codes, skipping 3/7/11/15, so the sequence low->high is
// 0,1,2, 4,5,6, 8,9,10, 12,13,14. Octave goes in bits 4-6 of the key code.
// (Global concert-pitch alignment is a one-constant trim; monotonic + ~A440 is
// what milestone-1 needs, and this table delivers that.)
static const uint8_t kOpmNote[12] = { 0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14 };

static inline int16_t clamp16(int32_t v) {
    if (v > 32767)  return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

AudioSynthYmfmOPM::AudioSynthYmfmOPM()
    : AudioStream(0, nullptr), m_chip(m_intf) {
    for (int i = 0; i < kNumChannels; i++) { m_note[i] = -1; m_age[i] = 0; }
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

void AudioSynthYmfmOPM::noteOn(uint8_t note, uint8_t vel) {
    if (vel == 0) { noteOff(note); return; }
    if (!m_haveVoice) return;

    int oct = (int)note / 12 - 1;                 // MIDI 60 -> OPM octave 4
    if (oct < 0) oct = 0; else if (oct > 7) oct = 7;
    uint8_t kc = (uint8_t)((oct << 4) | kOpmNote[note % 12]);

    // Velocity -> total-level attenuation added to every operator. For the default
    // additive (alg 7) patch all four operators are carriers, so this is a clean
    // loudness map; for modulator-carrier patches it also softens brightness at low
    // velocity, which is musically reasonable. (A per-algorithm carrier mask would
    // make FM patches respond in loudness only — a later refinement.)
    uint8_t tlAdd = (uint8_t)(((127 - vel) * 40) / 127);

    AudioNoInterrupts();
    int ch = allocChannel(note);
    writeReg(0x08, ch);                           // key OFF (retrigger cleanly)
    writeReg(0x28 + ch, kc & 0x7f);               // key code (block/note)
    writeReg(0x30 + ch, 0x00);                    // key fraction (no fine detune)
    for (int s = 0; s < 4; s++) {                 // apply velocity to operator TLs
        int tl = (int)m_voiceStore.op[s].tl + tlAdd;
        if (tl > 127) tl = 127;
        writeReg(0x60 + (uint8_t)(s * 8 + ch), (uint8_t)tl);
    }
    writeReg(0x08, 0x78 | ch);                    // key ON, all four operators
    AudioInterrupts();
}

void AudioSynthYmfmOPM::noteOff(uint8_t note) {
    AudioNoInterrupts();
    for (int ch = 0; ch < kNumChannels; ch++) {
        if (m_note[ch] == (int8_t)note) {
            writeReg(0x08, ch);                   // key OFF (all operators)
            m_note[ch] = -1;
        }
    }
    AudioInterrupts();
}

void AudioSynthYmfmOPM::allNotesOff() {
    AudioNoInterrupts();
    for (int ch = 0; ch < kNumChannels; ch++) {
        writeReg(0x08, ch);
        m_note[ch] = -1;
    }
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
