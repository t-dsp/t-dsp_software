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

    // Idle gate: once no notes are held, keep generating for this many audio
    // blocks (to let envelope release tails ring out), then stop calling the chip
    // and emit silence. This makes an *instantiated but silent* bank cost ~nothing
    // — essential when several banks run at once (see YmfmOpmMulti). ~500 blocks
    // ≈ 1.5 s at 128-sample blocks, comfortably longer than any preset's release.
    static constexpr uint32_t kIdleHoldBlocks = 500;

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
    // Non-MPE (mono-timbral) note API: notes carry no source channel, so a global
    // bend/pressure/timbre (source channel 0) applies to all — unchanged behaviour.
    void noteOn(uint8_t note, uint8_t vel) { noteOnCh(0, note, vel); }
    void noteOff(uint8_t note)             { noteOffCh(0, note); }
    void allNotesOff();
    void setVoice(const tdsp::ymfmopm::OpmVoice &voice);  // apply patch to all channels
    void setGain(float g) { m_gain = g; }                 // output trim (default kDefaultGain)

    // --- MPE / per-note expression -------------------------------------------
    // Each note is tagged with the MIDI channel (1..16) that owns it; expression
    // messages on that channel steer only that note's OPM channel — OPM's per-
    // channel key code/fraction makes true per-note pitch bend native. Semitones
    // are already pitch-bend-range-scaled by the caller (the MidiSink convention);
    // pressure/timbre are 0..1. Set the MPE master channel with setMasterChannel:
    // 1..16 = a member-channel note steers only itself while the master channel's
    // bend applies to ALL notes; 0 = no master (plain keyboard, every channel
    // allocates and behaves like the global path).
    void setMasterChannel(uint8_t ch) { m_masterChan = (ch <= 16) ? ch : 0; }
    uint8_t masterChannel() const { return m_masterChan; }

    void noteOnMpe (uint8_t srcCh, uint8_t note, uint8_t vel) { noteOnCh(srcCh, note, vel); }
    void noteOffMpe(uint8_t srcCh, uint8_t note)              { noteOffCh(srcCh, note); }
    void pitchBend (uint8_t srcCh, float semitones);   // per-channel (or master = all)
    void pressure  (uint8_t srcCh, float value);       // 0..1 -> loudness (carriers)
    void timbre    (uint8_t srcCh, float value);       // 0..1 -> brightness (modulators)
    void allNotesOffCh(uint8_t srcCh);

    // Number of channels currently holding a note (0..kNumChannels). Cheap scan;
    // used for the idle gate and for multi-bank telemetry.
    int activeVoices() const {
        int n = 0;
        for (int i = 0; i < kNumChannels; i++) if (m_note[i] >= 0) n++;
        return n;
    }

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

    // channel-tagged note on/off (source channel 0 = non-MPE / global)
    void  noteOnCh(uint8_t srcCh, uint8_t note, uint8_t vel);
    void  noteOffCh(uint8_t srcCh, uint8_t note);
    void  applyPitch(int ch);          // write KC/KF for one OPM channel (note + bend)
    void  applyExpression(int ch);     // write per-op TL (voice + velocity + pressure/timbre)
    float effectiveBend(uint8_t srcCh) const;

    // TL depths (register units; each ~0.75 dB): how far velocity/pressure/timbre
    // can move an operator's total level.
    static constexpr int kVelDepth    = 40;   // low velocity attenuates
    static constexpr int kPressDepth  = 40;   // pressure brightens/loudens carriers
    static constexpr int kTimbreDepth = 30;   // CC74 brightens modulators

    ymfm::ymfm_interface m_intf;       // default no-op interface (timers unused for a MIDI synth)
    ymfm::ym2151         m_chip;       // the emulated OPM
    ymfm::ym2151::output_data m_prev, m_cur;   // resampler taps (chip-rate samples)

    float m_ratio = 1.0f;   // chip native rate / AUDIO_SAMPLE_RATE (samples to advance per output)
    float m_pos   = 0.0f;   // fractional resampler position
    float m_gain  = kDefaultGain;   // linear output gain applied before clamp
    uint32_t m_idleBlocks = kIdleHoldBlocks;  // blocks since last active note (starts idle)

    // per-OPM-channel voice-allocation state
    int8_t   m_note[kNumChannels];     // MIDI note sounding on each channel, -1 = free
    uint32_t m_age[kNumChannels];      // note-on order (for oldest-note stealing)
    uint32_t m_ageCounter = 0;
    uint8_t  m_srcChan[kNumChannels];  // MIDI channel that owns each note (0 = global/non-MPE)
    uint8_t  m_velTl[kNumChannels];    // velocity TL offset captured at note-on

    // per-MIDI-channel MPE expression state (index 1..16; index 0 = global/non-MPE)
    float    m_bend[17]   = {0};       // pitch bend, semitones
    float    m_press[17]  = {0};       // channel pressure, 0..1
    float    m_timbre[17] = {0};       // CC74 timbre, 0..1
    uint8_t  m_masterChan = 0;         // MPE master channel (0 = none)

    // The engine keeps its OWN copy of the active voice so callers may pass a
    // temporary (e.g. a voice just parsed from an SD .opm file) without keeping
    // it alive. noteOn() reads operator TLs from here for velocity scaling.
    tdsp::ymfmopm::OpmVoice m_voiceStore;
    bool                    m_haveVoice = false;
};
