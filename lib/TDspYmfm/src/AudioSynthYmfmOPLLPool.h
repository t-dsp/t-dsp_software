// AudioSynthYmfmOPLLPool.h — a POOL of Yamaha YM2413 (OPLL) chips, one note per chip,
// so the OPLL can do TRUE 3-axis MPE (per-note bend + pressure + timbre).
//
// A single YM2413 has one shared user-voice register bank, so per-note timbre is
// impossible on it. This class runs N independent ymfm::ym2413 instances (one note
// each, on the chip's channel 0 user voice), so CC#74 can modulate each note's
// modulator level (brightness) independently. Bend -> that chip's F-number, pressure
// -> that chip's volume nibble, timbre -> that chip's modulator Total Level ($02).
//
// All chips share the 49716 Hz native rate, so their per-sample outputs are SUMMED and
// the sum runs through ONE linear resampler to the Teensy graph rate (cheaper than N
// resamplers). Mono out, transmitted to both L and R. Idle chips aren't generated.
//
// Note ALLOCATION (which slot plays which note) is done by OpllPoolSink; this class
// exposes only slot-addressed control. See the OPLL single-chip engine for the shared
// register/F-number conventions.
#pragma once

#include <Arduino.h>
#include <AudioStream.h>
#include "ymfm/ymfm_opl.h"

class AudioSynthYmfmOPLLPool : public AudioStream {
public:
    static constexpr int      kMaxSlots  = 8;          // pool ceiling (RAM/CPU cap)
    static constexpr uint32_t kClockHz   = 3579545;    // OPLL master clock
    static constexpr float    kNativeRate = 49716.0f;  // clock/72
    static constexpr uint16_t kReleaseBlocks = 300;    // keep rendering a slot this long after note-off
    static constexpr float    kDefaultGain = 2.0f;     // N summed notes -> less headroom than 1 chip

    AudioSynthYmfmOPLLPool();

    void begin();
    void setNumSlots(int n) { m_n = (n < 1) ? 1 : (n > kMaxSlots ? kMaxSlots : n); }
    int  numSlots() const { return m_n; }

    // --- slot-addressed control (driven by OpllPoolSink; call from loop, not ISR) ----
    // Load `patch` (8 user-voice bytes) into the slot's chip and key the note on.
    void slotNoteOn(int slot, uint8_t note, uint8_t vel, const uint8_t patch[8]);
    void slotNoteOff(int slot);
    void slotBend(int slot, float semitones);   // per-note pitch bend (X)
    void slotPressure(int slot, float amount);  // per-note volume swell 0..1 (Z)
    void slotTimbre(int slot, float amount);    // per-note brightness 0..1 (Y, CC#74)
    void allOff();

    void setGain(float g) { m_gain = g; }
    int  activeSlots() const {
        int n = 0; for (int s = 0; s < m_n; s++) if (m_active[s] || m_idle[s]) n++; return n;
    }

    virtual void update(void) override;

private:
    // Each slot = one whole OPLL chip with its own interface (so its user voice is
    // independent). Declared intf-before-chip so the chip binds to its own interface.
    struct Slot {
        ymfm::ymfm_interface intf;
        ymfm::ym2413         chip;
        Slot() : chip(intf) {}
    };
    Slot m_slot[kMaxSlots];

    inline void writeReg(int s, uint8_t reg, uint8_t val) {
        m_slot[s].chip.write(0, reg); m_slot[s].chip.write(1, val);
    }
    static void computeFB(float noteF, uint32_t &fnum, int &block);
    uint8_t volNibble(int s) const;   // velocity + pressure -> 4-bit attenuation
    uint8_t modTL(int s) const;       // patch modulator TL modulated by timbre (brightness)
    void applyPitch(int s);
    void applyVolume(int s);
    void applyTimbre(int s);

    // per-slot state (channel 0 of each chip)
    bool     m_active[kMaxSlots];
    uint8_t  m_note[kMaxSlots];
    uint8_t  m_vel[kMaxSlots];
    float    m_baseNote[kMaxSlots];
    float    m_bend[kMaxSlots];
    float    m_pressure[kMaxSlots];
    float    m_timbre[kMaxSlots];
    uint8_t  m_patch[kMaxSlots][8];
    uint8_t  m_reg2x[kMaxSlots];
    bool     m_keyOn[kMaxSlots];
    uint16_t m_idle[kMaxSlots];       // release-tail render countdown (update blocks)

    int      m_n = 6;                 // active pool size
    float    m_ratio = 1.0f;
    float    m_pos = 0.0f;
    float    m_gain = kDefaultGain;
    int32_t  m_prevSum = 0, m_curSum = 0;   // summed-native resampler taps
    bool     m_ready = false;
};
