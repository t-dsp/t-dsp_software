// AudioSynthYmfmOPLL.cpp — see header. Wraps ymfm::ym2413 (OPLL) as a Teensy audio
// source and adds a small GM MIDI synth layer (9-voice allocator, per-channel program
// mapped onto the 15 built-in instruments, channel-10 rhythm section).

#include "AudioSynthYmfmOPLL.h"
#include <math.h>

// Gate the audio-update software IRQ so a batch of register writes can't be torn by
// the render ISR. Guarded to avoid pulling in all of Audio.h (mirrors OPL3 wrapper).
#ifndef AudioNoInterrupts
#define AudioNoInterrupts() (NVIC_DISABLE_IRQ(IRQ_SOFTWARE))
#define AudioInterrupts()   (NVIC_ENABLE_IRQ(IRQ_SOFTWARE))
#endif

// The 15 OPLL melodic instruments, in ROM order (index 1..15). Names are the
// conventional YM2413 set; index 0 (the user/custom voice) is not exposed here.
static const char *const kInstNames[16] = {
    "User",                                        // 0 (custom voice, unused)
    "Violin", "Guitar", "Piano", "Flute",          // 1..4
    "Clarinet", "Oboe", "Trumpet", "Organ",        // 5..8
    "Horn", "Synthesizer", "Harpsichord",          // 9..11
    "Vibraphone", "Synth Bass", "Acoustic Bass",   // 12..14
    "Electric Guitar"                              // 15
};

// GM program (0..127) -> OPLL instrument (1..15). OPLL only has 15 timbres, so this
// is a coarse family map; tune to taste. Index 0 = user voice is intentionally avoided.
static const uint8_t kGmToInst[128] = {
    // 0-7 Pianos
    3,3,3,3,3,3,11,11,
    // 8-15 Chromatic percussion
    12,12,12,12,12,12,11,11,
    // 16-23 Organ
    8,8,8,8,8,8,8,8,
    // 24-31 Guitar
    2,2,2,15,15,15,15,15,
    // 32-39 Bass
    14,14,14,14,13,13,13,13,
    // 40-47 Strings
    1,1,1,1,1,1,1,12,
    // 48-55 Ensemble
    1,1,1,1,10,10,10,10,
    // 56-63 Brass
    7,7,7,7,7,7,7,7,
    // 64-71 Reed
    6,6,6,6,5,5,5,5,
    // 72-79 Pipe
    4,4,4,4,4,4,4,4,
    // 80-87 Synth lead
    10,10,10,10,10,10,10,10,
    // 88-95 Synth pad
    10,10,10,10,10,10,10,10,
    // 96-103 Synth effects
    10,10,10,10,10,10,10,10,
    // 104-111 Ethnic
    2,2,2,2,2,2,2,2,
    // 112-119 Percussive
    12,12,12,12,12,12,12,12,
    // 120-127 Sound effects
    10,10,10,10,10,10,10,10
};

static inline int16_t clamp16(int32_t v) {
    if (v > 32767)  return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

AudioSynthYmfmOPLL::AudioSynthYmfmOPLL()
    : AudioStream(0, nullptr), m_chip(m_intf) {
    for (int c = 0; c < kNumChannels; c++) {
        m_note[c] = -1; m_chan[c] = 0; m_age[c] = 0;
        m_baseNote[c] = 0.0f; m_reg2x[c] = 0; m_inst[c] = 1; m_keyOn[c] = false;
    }
    for (int i = 0; i < 17; i++) { m_program[i] = 0; m_bend[i] = 0.0f; m_override[i] = -1; }
}

FLASHMEM void AudioSynthYmfmOPLL::begin() {
    m_chip.reset();

    AudioNoInterrupts();
    writeReg(0x0E, 0x00);   // rhythm off
    for (int c = 0; c < kNumChannels; c++) {
        writeReg(0x20 + c, 0x00);   // key off, block 0
        writeReg(0x30 + c, 0x0F);   // instrument 0, max attenuation (silent)
        m_note[c] = -1; m_chan[c] = 0; m_age[c] = 0;
        m_baseNote[c] = 0.0f; m_reg2x[c] = 0; m_inst[c] = 1; m_keyOn[c] = false;
    }
    m_rhythmBits = 0; m_rhythmMode = false;
    AudioInterrupts();

    // Resample ratio: chip native rate (clock/72) per one output sample.
    uint32_t nativeRate = m_chip.sample_rate(kClockHz);
    m_ratio = (float)nativeRate / (float)AUDIO_SAMPLE_RATE_EXACT;
    m_pos   = 0.0f;

    // Prime the resampler taps so the first update() has two valid samples.
    m_cur.clear();
    m_chip.generate(&m_cur, 1);
    m_prev = m_cur;

    for (int i = 1; i <= 16; i++) { m_program[i] = 0; m_bend[i] = 0.0f; m_override[i] = -1; }
    m_ready = true;
}

const char *AudioSynthYmfmOPLL::instrumentName(int opllInst) const {
    if (opllInst < 1 || opllInst > kNumInstruments) return "";
    return kInstNames[opllInst];
}

int AudioSynthYmfmOPLL::gmToInstrument(uint8_t program) const {
    return kGmToInst[program & 0x7F];
}

// MIDI note (float) -> OPLL 9-bit F-number (0..511) + 3-bit block. Picks the lowest
// block whose F-number fits, keeping maximum precision. OPLL's F-number multiplier is
// half OPL3's (9-bit vs 10-bit field): fnum = freq * 2^(19-block) / fsam.
void AudioSynthYmfmOPLL::computeFB(float noteF, uint32_t &fnum, int &block) {
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

// MIDI velocity -> 4-bit OPLL volume nibble (0 = loudest, 15 = silent).
uint8_t AudioSynthYmfmOPLL::volNibble(uint8_t velocity) {
    // Map 1..127 -> attenuation 0..~13, leaving 15 for effectively silent.
    int att = (127 - (int)velocity) * 13 / 127;
    if (att < 0) att = 0; if (att > 15) att = 15;
    return (uint8_t)att;
}

void AudioSynthYmfmOPLL::applyPitch(int c) {
    if (m_note[c] < 0) return;
    float nf = m_baseNote[c] + m_bend[m_chan[c] <= 16 ? m_chan[c] : 0];
    uint32_t fnum; int block;
    computeFB(nf, fnum, block);
    writeReg(0x10 + c, (uint8_t)(fnum & 0xFF));               // F-num low 8 bits
    uint8_t r2 = (uint8_t)(((fnum >> 8) & 0x01)              // F-num high bit
                           | ((block & 0x07) << 1)           // block
                           | (m_keyOn[c] ? 0x10 : 0x00));    // key on
    m_reg2x[c] = r2;
    writeReg(0x20 + c, r2);
}

void AudioSynthYmfmOPLL::keyOff(int c) {
    m_keyOn[c] = false;
    uint8_t r2 = (uint8_t)(m_reg2x[c] & ~0x10);
    m_reg2x[c] = r2;
    writeReg(0x20 + c, r2);
}

int AudioSynthYmfmOPLL::allocChannel(uint8_t midiCh, uint8_t note) {
    // While rhythm mode is active, channels 6-8 belong to the drum section.
    int limit = m_rhythmMode ? 6 : kNumChannels;
    for (int c = 0; c < limit; c++)
        if (m_note[c] < 0) {
            m_note[c] = (int8_t)note; m_chan[c] = midiCh; m_age[c] = ++m_ageCounter;
            return c;
        }
    int oldest = 0;
    for (int c = 1; c < limit; c++)
        if (m_age[c] < m_age[oldest]) oldest = c;
    keyOff(oldest);
    m_note[oldest] = (int8_t)note; m_chan[oldest] = midiCh; m_age[oldest] = ++m_ageCounter;
    return oldest;
}

void AudioSynthYmfmOPLL::noteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (!m_ready) return;
    if (velocity == 0) { noteOff(channel, note); return; }
    if (channel < 1 || channel > 16) return;

    if (channel == 10) { drumOn(note, velocity); return; }

    // A picker override (0 = user voice, 1..15 = ROM) wins; otherwise fall back to
    // the GM->ROM map so songs' per-channel program changes still pick a voice.
    int inst = (m_override[channel] >= 0) ? m_override[channel]
                                          : gmToInstrument(m_program[channel]);

    AudioNoInterrupts();
    int c = allocChannel(channel, note);
    m_inst[c]     = (uint8_t)inst;
    m_baseNote[c] = (float)note;
    m_keyOn[c]    = false;
    // 0x3x: instrument (high nibble) | volume (low nibble). Set before key-on.
    writeReg(0x30 + c, (uint8_t)((inst << 4) | volNibble(velocity)));
    applyPitch(c);                       // 0x1x/0x2x with key still off
    // Clock the chip once so it samples KON=0 before we raise KON=1 (see OPL3 wrapper:
    // a rapid off->on on the same voice otherwise misses the key edge and never
    // re-attacks). One forced sample restores the edge.
    { ymfm::ym2413::output_data tmp; m_chip.generate(&tmp, 1); }
    m_keyOn[c] = true;
    uint8_t r2 = (uint8_t)(m_reg2x[c] | 0x10);
    m_reg2x[c] = r2;
    writeReg(0x20 + c, r2);
    AudioInterrupts();
}

void AudioSynthYmfmOPLL::noteOff(uint8_t channel, uint8_t note) {
    if (channel < 1 || channel > 16) return;
    if (channel == 10) { drumOff(note); return; }
    AudioNoInterrupts();
    for (int c = 0; c < kNumChannels; c++)
        if (m_note[c] == (int8_t)note && m_chan[c] == channel) {
            keyOff(c);
            m_note[c] = -1;
        }
    AudioInterrupts();
}

void AudioSynthYmfmOPLL::programChange(uint8_t channel, uint8_t program) {
    if (channel < 1 || channel > 16) return;
    m_program[channel] = program & 0x7F;
    m_override[channel] = -1;   // a song's program change reverts this channel to the GM map
}

// Write an 8-byte user-voice patch into OPLL registers $00..$07 (instrument slot 0).
void AudioSynthYmfmOPLL::setUserVoice(const uint8_t patch[8]) {
    AudioNoInterrupts();
    for (uint8_t r = 0; r < 8; r++) writeReg(r, patch[r]);
    AudioInterrupts();
}

void AudioSynthYmfmOPLL::setInstrumentOverride(uint8_t channel, int inst) {
    if (channel < 1 || channel > 16) return;
    if (inst < -1) inst = -1; else if (inst > 15) inst = 15;
    m_override[channel] = (int8_t)inst;
}

void AudioSynthYmfmOPLL::pitchBend(uint8_t channel, float semitones) {
    if (channel < 1 || channel > 16) return;
    m_bend[channel] = semitones;
    AudioNoInterrupts();
    for (int c = 0; c < kNumChannels; c++)
        if (m_note[c] >= 0 && m_chan[c] == channel) applyPitch(c);
    AudioInterrupts();
}

void AudioSynthYmfmOPLL::controlChange(uint8_t channel, uint8_t cc, uint8_t v) {
    if (channel < 1 || channel > 16) return;
    switch (cc) {
        case 120:                        // all sound off
        case 123:                        // all notes off (this channel)
            AudioNoInterrupts();
            if (channel == 10) { m_rhythmBits = 0; writeReg(0x0E, m_rhythmMode ? 0x20 : 0x00); }
            for (int c = 0; c < kNumChannels; c++)
                if (m_note[c] >= 0 && m_chan[c] == channel) { keyOff(c); m_note[c] = -1; }
            AudioInterrupts();
            break;
        default:
            break;                       // mod wheel / volume / sustain: no-op for now
    }
}

void AudioSynthYmfmOPLL::allNotesOff() {
    AudioNoInterrupts();
    for (int c = 0; c < kNumChannels; c++) {
        if (m_note[c] >= 0) keyOff(c);
        m_note[c] = -1;
    }
    m_rhythmBits = 0;
    if (m_rhythmMode) writeReg(0x0E, 0x20);
    AudioInterrupts();
}

// --- rhythm section --------------------------------------------------------------
// OPLL's built-in 5-piece kit lives on channels 6-8. Enabling it requires the fixed
// per-datasheet frequency setup on those channels, then the drums are keyed by pulsing
// bits in 0x0E. Values below are the conventional YM2413 rhythm init; tune to taste.

void AudioSynthYmfmOPLL::enableRhythm() {
    if (m_rhythmMode) return;
    // Free any melodic voices squatting on ch6-8 before the drum section claims them.
    for (int c = 6; c < 9; c++) {
        if (m_note[c] >= 0) { keyOff(c); m_note[c] = -1; }
    }
    // Standard rhythm-channel frequency/block setup (YM2413 application manual).
    writeReg(0x16, 0x20); writeReg(0x26, 0x05);   // ch6: bass drum
    writeReg(0x17, 0x50); writeReg(0x27, 0x05);   // ch7: hi-hat / snare
    writeReg(0x18, 0xC0); writeReg(0x28, 0x01);   // ch8: tom / top cymbal
    m_rhythmBits = 0;
    writeReg(0x0E, 0x20);                          // rhythm enable, all drums off
    m_rhythmMode = true;
}

void AudioSynthYmfmOPLL::disableRhythm() {
    if (!m_rhythmMode) return;
    m_rhythmBits = 0;
    writeReg(0x0E, 0x00);
    m_rhythmMode = false;
}

// GM percussion note -> OPLL rhythm key bit within 0x0E (0 if unmapped).
uint8_t AudioSynthYmfmOPLL::drumBit(uint8_t note) {
    switch (note) {
        case 35: case 36:                       return 0x10;  // bass drum
        case 38: case 40:                       return 0x08;  // snare
        case 41: case 43: case 45:
        case 47: case 48: case 50:              return 0x04;  // toms
        case 42: case 44: case 46:              return 0x01;  // hi-hat
        case 49: case 51: case 52:
        case 55: case 57: case 59:              return 0x02;  // cymbals
        default:                                return 0x00;
    }
}

void AudioSynthYmfmOPLL::drumOn(uint8_t note, uint8_t velocity) {
    uint8_t bit = drumBit(note);
    if (!bit) return;
    AudioNoInterrupts();
    enableRhythm();
    // Drum "volume": BD uses ch6's 0x36 (both nibbles), SD/HH share ch7's 0x37,
    // TOM/TC share ch8's 0x38. Coarse velocity write; good enough for a scaffold.
    uint8_t v = volNibble(velocity);
    switch (bit) {
        case 0x10: writeReg(0x36, (uint8_t)((v << 4) | v)); break;
        case 0x08: case 0x01: writeReg(0x37, (uint8_t)((v << 4) | v)); break;
        case 0x04: case 0x02: writeReg(0x38, (uint8_t)((v << 4) | v)); break;
    }
    // Re-trigger: clear then set this drum's bit so ymfm sees a key edge.
    writeReg(0x0E, (uint8_t)(0x20 | (m_rhythmBits & ~bit)));
    { ymfm::ym2413::output_data tmp; m_chip.generate(&tmp, 1); }
    m_rhythmBits |= bit;
    writeReg(0x0E, (uint8_t)(0x20 | m_rhythmBits));
    AudioInterrupts();
}

void AudioSynthYmfmOPLL::drumOff(uint8_t note) {
    uint8_t bit = drumBit(note);
    if (!bit) return;
    AudioNoInterrupts();
    m_rhythmBits &= ~bit;
    if (m_rhythmMode) writeReg(0x0E, (uint8_t)(0x20 | m_rhythmBits));
    AudioInterrupts();
}

void AudioSynthYmfmOPLL::update(void) {
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
        // OPLL is mono: ymfm reports it on two outputs (melody / rhythm split); sum
        // them and feed both L and R.
        int32_t p = m_prev.data[0] + m_prev.data[1];
        int32_t c = m_cur.data[0]  + m_cur.data[1];
        int32_t s = p + (int32_t)((c - p) * f);
        if (m_gain != 1.0f) s = (int32_t)(s * m_gain);
        int16_t out = clamp16(s);
        blockL->data[i] = out;
        blockR->data[i] = out;
    }

    transmit(blockL, 0);
    transmit(blockR, 1);
    release(blockL);
    release(blockR);
}
