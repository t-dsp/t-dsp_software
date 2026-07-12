// AudioSynthYmfmOPL3.h — a Yamaha YMF262 (OPL3) as a Teensy Audio Library source,
// wrapped as a self-contained General-MIDI synth using the DMXOPL bank.
//
//   GM MIDI (note/program/drum)  --(register writes)-->  ymfm::ymf262
//     --generate() @ chip-native rate (clock/288 ~= 49.7 kHz)--> linear resample
//     --> stereo int16 AudioStream (2 outputs: 0=L, 1=R)
//
// Unlike the OPM wrapper (mono-timbral, one patch on all channels), OPL3 is
// natively multitimbral: this class owns an 18-channel (2-op) voice allocator, a
// per-MIDI-channel program map, drum handling on channel 10, and the parsed
// DMXOPL bank. The caller speaks only GM MIDI (see Opl3Sink / SynthBackendOpl3).
//
// Structure mirrors AudioSynthYmfmOPM: the chip runs at its true 14.318 MHz clock
// so pitch/timbre match hardware; update() retimes the chip's four OPL3 outputs to
// the Teensy graph rate with a fractional-position linear interpolator. Register
// writes happen from the caller's thread (loop), guarded by AudioNoInterrupts().

#pragma once

#include <Arduino.h>
#include <AudioStream.h>
#include "OplBank.h"
#include "ymfm/ymfm_opl.h"

class AudioSynthYmfmOPL3 : public AudioStream {
public:
    // OPL3 master clock (NTSC 4x colorburst). Native output rate = clock/288 =
    // 49716 Hz; keeping the true clock keeps pitch/envelope timing authentic and
    // update() retimes to AUDIO_SAMPLE_RATE.
    static constexpr uint32_t kClockHz     = 14318181;
    static constexpr int      kNumChannels = 18;      // OPL3 two-op channels
    static constexpr float    kNativeRate  = 49716.0f; // clock/288, for F-num math

    // Idle gate (see OPM wrapper): keep rendering release tails for this many
    // blocks after the last note, then emit nothing (a silent bank costs ~zero).
    static constexpr uint32_t kIdleHoldBlocks = 500;

    // Default output gain. OPL3's summed output is conservative; a 2x lift gives a
    // usable level with headroom (dense chords only graze the int16 clamp).
    static constexpr float kDefaultGain = 2.0f;

    AudioSynthYmfmOPL3();

    // Call once from setup() (after AudioMemory). Resets the chip, enables OPL3
    // mode, computes the resample ratio, and parses the baked DMXOPL bank.
    void begin();

    // --- GM MIDI (call from loop / MIDI handlers, not the ISR) -----------------
    // channel is 1..16 (GM). Channel 10 is the drum channel: `note` selects the
    // percussion instrument; program on ch10 is ignored.
    void noteOn (uint8_t channel, uint8_t note, uint8_t velocity);
    void noteOff(uint8_t channel, uint8_t note);
    void programChange(uint8_t channel, uint8_t program);   // melodic patch 0..127
    void pitchBend(uint8_t channel, float semitones);       // per-channel, pre-scaled
    void controlChange(uint8_t channel, uint8_t cc, uint8_t v); // 120/123 -> notes off
    void allNotesOff();

    void setGain(float g) { m_gain = g; }
    int  activeVoices() const {
        int n = 0;
        for (int i = 0; i < kNumChannels; i++) if (m_note[i] >= 0) n++;
        return n;
    }

    // --- bank ------------------------------------------------------------------
    // WOPL parsing is not implemented yet (returns -1). OP2 replaces the bank.
    int loadBankWopl(const uint8_t *data, size_t len);
    int loadBankOp2 (const uint8_t *data, size_t len);

    // --- catalog (for the app instrument picker) -------------------------------
    int         numMelodic() const { return tdsp::ymfmopl::kNumMelodic; }
    const char *melodicName(int gm) const;

    virtual void update(void) override;

private:
    // Low-level OPL3 register write (0x000-0x1FF). Bank 0 via ports 0/1, bank 1
    // (reg >= 0x100) via ports 2/3. Must be batched under AudioNoInterrupts().
    inline void writeReg(uint16_t reg, uint8_t val) {
        if (reg < 0x100) { m_chip.write(0, (uint8_t)(reg & 0xFF)); m_chip.write(1, val); }
        else             { m_chip.write(2, (uint8_t)(reg & 0xFF)); m_chip.write(3, val); }
    }

    // Per-OPL-channel operator/register offset helpers.
    static uint16_t chanBank(int c) { return (c < 9) ? 0x000 : 0x100; }
    static int      chanLocal(int c) { return c % 9; }
    static uint8_t  modOff(int c);   // modulator operator offset
    static uint8_t  carOff(int c);   // carrier operator offset

    void loadPatch(int c, const tdsp::ymfmopl::OplPatch &p, uint8_t carTL);
    void applyPitch(int c);          // write 0xA0/0xB0 from base note + bend, keep key state
    void keyOff(int c);
    int  allocChannel(uint8_t midiCh, uint8_t note);
    static void computeFB(float noteF, uint32_t &fnum, int &block);
    static uint8_t carrierTL(const tdsp::ymfmopl::OplPatch &p, uint8_t velocity);

    ymfm::ymfm_interface       m_intf;
    ymfm::ymf262               m_chip;
    ymfm::ymf262::output_data  m_prev, m_cur;   // resampler taps (chip-rate samples)

    float    m_ratio = 1.0f;
    float    m_pos   = 0.0f;
    float    m_gain  = kDefaultGain;
    uint32_t m_idleBlocks = kIdleHoldBlocks;

    // per-OPL-channel voice-allocation state
    int8_t   m_note[kNumChannels];    // MIDI note sounding, -1 = free
    uint8_t  m_chan[kNumChannels];    // owning MIDI channel (1..16)
    uint32_t m_age[kNumChannels];     // note-on order (oldest-note stealing)
    uint32_t m_ageCounter = 0;
    float    m_baseNote[kNumChannels];// effective note float (played note + offsets), pre-bend
    uint8_t  m_b0[kNumChannels];      // last 0xB0 value (block|fnum-hi|keyon)
    bool     m_keyOn[kNumChannels];

    // per-MIDI-channel state (index 1..16)
    uint8_t  m_program[17];           // current GM program per channel
    float    m_bend[17] = {0};        // pitch bend, semitones
    bool     m_sustain[17] = {false};

    // parsed bank (DMXOPL by default). Points at a DMAMEM-resident array (defined
    // in the .cpp) to keep the ~11 KB out of RAM1; survives without SD.
    tdsp::ymfmopl::OplInstrument *m_inst = nullptr;
    int  m_numInst = 0;
    bool m_ready   = false;
};
