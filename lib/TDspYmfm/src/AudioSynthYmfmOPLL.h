// AudioSynthYmfmOPLL.h — a Yamaha YM2413 (OPLL) as a Teensy Audio Library source.
//
//   GM MIDI (note/program/drum)  --(register writes)-->  ymfm::ym2413
//     --generate() @ chip-native rate (clock/72 ~= 49.7 kHz)--> linear resample
//     --> stereo int16 AudioStream (2 outputs: 0=L, 1=R; OPLL is mono, folded to both)
//
// This is the "PSS-140 sound": OPLL has NO user-programmable operator params for its
// melodic patches — you pick one of 15 instruments baked into the chip's ROM (the
// ym2413::s_default_instruments table) by writing a 4-bit instrument number, plus a
// 4-bit volume, per channel. So there is no external bank to parse (contrast the OPL3
// wrapper's DMXOPL bank); programChange() maps a GM program to one of those 15.
//
// Structure mirrors AudioSynthYmfmOPL3: the chip runs at its true 3.579545 MHz clock
// so pitch/timbre match hardware; update() retimes the chip output to the Teensy graph
// rate with a fractional-position linear interpolator. Register writes happen from the
// caller's thread (loop), batched under AudioNoInterrupts().
//
// Percussion (MIDI channel 10) uses OPLL's built-in 5-piece rhythm section (BD/SD/
// TOM/TC/HH). Enabling rhythm mode steals melodic channels 6-8 (authentic OPLL
// behaviour), so melodic polyphony drops from 9 to 6 while drums are active.

#pragma once

#include <Arduino.h>
#include <AudioStream.h>
#include "ymfm/ymfm_opl.h"

class AudioSynthYmfmOPLL : public AudioStream {
public:
    // OPLL master clock (3.579545 MHz, NTSC colorburst). Native output rate =
    // clock/72 = 49716 Hz — same as OPL3, so the F-number/resample math matches.
    static constexpr uint32_t kClockHz    = 3579545;
    static constexpr int      kNumChannels = 9;        // OPLL two-op channels
    static constexpr float    kNativeRate  = 49716.0f; // clock/72, for F-num math
    static constexpr int      kNumInstruments = 15;    // built-in melodic patches (1..15)

    // See OPM/OPL3 wrappers: keep rendering release tails for this many blocks after
    // the last note, then emit nothing (a silent chip costs ~zero).
    static constexpr uint32_t kIdleHoldBlocks = 500;

    // OPLL's 9-bit DAC output is quiet; a healthy default lift.
    static constexpr float kDefaultGain = 3.0f;

    AudioSynthYmfmOPLL();

    // Call once from setup() (after AudioMemory). Resets the chip and computes the
    // resample ratio. Instrument ROM is the ym2413 default, so no bank load needed.
    void begin();

    // --- GM MIDI (call from loop / MIDI handlers, not the ISR) -----------------
    // channel is 1..16 (GM). Channel 10 is the drum channel (OPLL rhythm section).
    void noteOn (uint8_t channel, uint8_t note, uint8_t velocity);
    void noteOff(uint8_t channel, uint8_t note);
    void programChange(uint8_t channel, uint8_t program);   // melodic patch 0..127
    void pitchBend(uint8_t channel, float semitones);       // per-channel, pre-scaled

    // --- direct patch control (bypasses the GM->ROM map) -----------------------
    // Load an 8-byte OPLL user-voice patch (registers $00..$07) into instrument
    // slot 0. There is ONE user-voice bank shared by all channels, so all channels
    // playing instrument 0 share the last patch loaded here (mono-timbral user voice).
    void setUserVoice(const uint8_t patch[8]);
    // Force a channel's instrument: 0 = the user voice, 1..15 = a built-in ROM
    // instrument, -1 = clear (revert to the GM program map). A ProgramChange on the
    // channel also clears the override. Used by the app picker to audition a specific
    // ROM or PSS-140 voice on every channel.
    void setInstrumentOverride(uint8_t channel, int inst);
    void controlChange(uint8_t channel, uint8_t cc, uint8_t v); // 120/123 -> notes off
    void allNotesOff();

    void setGain(float g) { m_gain = g; }
    int  activeVoices() const {
        int n = 0;
        for (int i = 0; i < kNumChannels; i++) if (m_note[i] >= 0) n++;
        return n + (m_rhythmBits ? 1 : 0);
    }

    // --- catalog (for the app instrument picker) -------------------------------
    // OPLL exposes exactly 15 melodic instruments; GM programs map onto them.
    int         numInstruments() const { return kNumInstruments; }
    const char *instrumentName(int opllInst) const;   // 1..15
    // The OPLL instrument number (1..15) a GM program maps to.
    int         gmToInstrument(uint8_t program) const;

    virtual void update(void) override;

private:
    // Low-level OPLL register write: address port then data port.
    inline void writeReg(uint8_t reg, uint8_t val) {
        m_chip.write(0, reg);
        m_chip.write(1, val);
    }

    // MIDI note (float, incl. bend) -> OPLL 9-bit F-number + 3-bit block.
    static void computeFB(float noteF, uint32_t &fnum, int &block);

    void applyPitch(int c);              // write 0x1x/0x2x from base note + bend, keep key state
    void keyOff(int c);
    int  allocChannel(uint8_t midiCh, uint8_t note);
    static uint8_t volNibble(uint8_t velocity);  // MIDI velocity -> 4-bit attenuation

    // percussion (rhythm section)
    void enableRhythm();
    void disableRhythm();
    void drumOn(uint8_t note, uint8_t velocity);
    void drumOff(uint8_t note);
    static uint8_t drumBit(uint8_t note);        // GM note -> rhythm key bit (0 = none)

    ymfm::ymfm_interface       m_intf;
    ymfm::ym2413               m_chip;
    ymfm::ym2413::output_data  m_prev, m_cur;    // resampler taps (chip-rate samples)

    float    m_ratio = 1.0f;
    float    m_pos   = 0.0f;
    float    m_gain  = kDefaultGain;
    uint32_t m_idleBlocks = kIdleHoldBlocks;

    // per-OPLL-channel voice-allocation state
    int8_t   m_note[kNumChannels];    // MIDI note sounding, -1 = free
    uint8_t  m_chan[kNumChannels];    // owning MIDI channel (1..16)
    uint32_t m_age[kNumChannels];     // note-on order (oldest-note stealing)
    uint32_t m_ageCounter = 0;
    float    m_baseNote[kNumChannels];// played note float, pre-bend
    uint8_t  m_reg2x[kNumChannels];   // last 0x2x value (sustain|keyon|block|fnum-hi)
    uint8_t  m_inst[kNumChannels];    // OPLL instrument (1..15) loaded on this voice
    bool     m_keyOn[kNumChannels];

    // per-MIDI-channel state (index 1..16)
    uint8_t  m_program[17];           // current GM program per channel
    float    m_bend[17]    = {0};     // pitch bend, semitones
    int8_t   m_override[17];          // forced instrument (0..15), -1 = use GM map

    // rhythm section
    uint8_t  m_rhythmBits = 0;        // currently-latched drum key bits (0x0E low 5)
    bool     m_rhythmMode = false;    // whether rhythm mode is enabled (steals ch6-8)

    bool     m_ready = false;
};
