// AudioSynthYmfmOPM.h — a Yamaha YM2151 (OPM) as a Teensy Audio Library source.
//
//   MIDI note/patch  --(register writes)-->  ymfm::ym2151 (chip emulation)
//     --generate() @ chip-native rate (clock/64)--> linear resample to 48 kHz
//     --> stereo int16 AudioStream (2 outputs: 0=L, 1=R)
//
// The chip runs at the real 3.579545 MHz clock, so its native output rate is
// ~55.93 kHz and pitch/timbre match hardware. update() resamples that to the
// Teensy graph rate with a fractional-position linear interpolator (a spike-grade
// downsampler — no anti-alias filter, which is inaudible for this material).
//
// ymfm is a REGISTER-LEVEL emulator with no note API, so this class adds the
// missing synth layer: an 8-voice allocator (with oldest-note stealing), a
// MIDI-note -> OPM key-code map, and OpmVoice patch application. Register writes
// happen from the caller's thread (loop), guarded by AudioNoInterrupts() so a
// write can't be torn by the audio update ISR.
//
// This is int16 by design: it drops into the same int16->F32 bridge the mix-kit
// already uses for Dexed (AudioConvert_I16toF32), or straight into an int16
// AudioOutputTDM as in the standalone spike.

#pragma once

#include <Arduino.h>
#include <AudioStream.h>
#include "OpmVoice.h"
#include "ymfm/ymfm_opm.h"

class AudioSynthYmfmOPM : public AudioStream {
public:
    // OPM master clock. The real YM2151 ran at 3.579545 MHz (NTSC colorburst);
    // native output rate is clock/64 = 55.93 kHz. Keeping the true clock keeps
    // pitch and LFO/envelope timing authentic; update() retimes to AUDIO's rate.
    static constexpr uint32_t kClockHz = 3579545;
    static constexpr int      kNumChannels = 8;   // OPM FM channels

    // Default output gain. The raw OPM sum is conservative (a single note peaks
    // ~0.045 full-scale, a 4-note chord ~0.17), so a 2x lift gives a usable level
    // while leaving headroom: dense high-velocity chords only graze the int16
    // clamp (a soft ceiling, not a wrap). Override any time with setGain().
    static constexpr float    kDefaultGain = 2.0f;

    AudioSynthYmfmOPM();

    // Must be called once from setup() (after AudioMemory). Resets the chip,
    // computes the resample ratio, and loads the default voice on all channels.
    void begin();

    // --- performance API (call from loop / MIDI handlers, not the ISR) ---------
    void noteOn(uint8_t note, uint8_t vel);
    void noteOff(uint8_t note);
    void allNotesOff();
    void setVoice(const tdsp::ymfmopm::OpmVoice &voice);  // apply patch to all channels
    void setGain(float g) { m_gain = g; }                 // output trim (default kDefaultGain)

    // Teensy Audio Library render callback (runs in the audio software ISR).
    virtual void update(void) override;

private:
    // low-level register write: address latch then data (ymfm two-step protocol)
    inline void writeReg(uint8_t reg, uint8_t val) {
        m_chip.write_address(reg);
        m_chip.write_data(val);
    }
    void applyVoiceToChannel(int ch, const tdsp::ymfmopm::OpmVoice &v);
    int  allocChannel(uint8_t note);   // pick a free channel or steal the oldest

    ymfm::ymfm_interface m_intf;       // default no-op interface (timers unused for a MIDI synth)
    ymfm::ym2151         m_chip;       // the emulated OPM
    ymfm::ym2151::output_data m_prev, m_cur;   // resampler taps (chip-rate samples)

    float m_ratio = 1.0f;   // chip native rate / AUDIO_SAMPLE_RATE (samples to advance per output)
    float m_pos   = 0.0f;   // fractional resampler position
    float m_gain  = kDefaultGain;   // linear output gain applied before clamp

    // per-channel voice-allocation state
    int8_t   m_note[kNumChannels];     // MIDI note sounding on each channel, -1 = free
    uint32_t m_age[kNumChannels];      // note-on order (for oldest-note stealing)
    uint32_t m_ageCounter = 0;

    const tdsp::ymfmopm::OpmVoice *m_voice = nullptr;
};
