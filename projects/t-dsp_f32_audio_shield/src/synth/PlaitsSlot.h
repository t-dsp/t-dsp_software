// PlaitsSlot.h — ISynthSlot adapter for the Plaits-inspired macro
// oscillator engine in lib/TDspPlaits.
//
// Slot 2 in the new T-DSP synth-slot architecture. The engine itself
// (PlaitsSink) is reused via the AudioContext pattern — the audio nodes
// live at file scope in main.cpp, this adapter just exposes the slot
// surface (id / displayName / midiSink / setActive / panic) plus the
// per-slot housekeeping the OSC contract expects (volume, on,
// listenChannel/masterChannel).
//
// Plaits is MPE-aware (channel-per-voice routing for pitch bend /
// pressure / CC#74) but defaults to master=0 (omni / non-MPE) so the
// dev surface's on-screen keyboard plays it without configuration.
//
// Audio path (mono int16 → F32 sub-mixer slot 2, dual-mono fan-out —
// same shape MPE/Dexed use):
//
//   plaitsOsc1[i] / plaitsOsc2[i] -> plaitsVoiceMix[i] (osc1@0, osc2@1)
//                                  -> plaitsFilter[i]
//                                  -> plaitsEnv[i]
//                                  -> plaitsMix (4-into-1 mono)
//                                  -> plaitsToF32 (I16->F32)
//                                  -> plaitsGain (per-slot fader)
//                                  -> g_synthSumLA[2] + g_synthSumRA[2]

#pragma once

#include <stdint.h>

#include <MidiSink.h>
#include <PlaitsSink.h>

#include "SynthSlot.h"

class AudioEffectGain_F32;

namespace tdsp_synth {

class PlaitsSlot : public ISynthSlot {
public:
    struct AudioContext {
        PlaitsSink          *sink;
        AudioEffectGain_F32 *gain;
    };

    explicit PlaitsSlot(const AudioContext &ctx);

    // ISynthSlot
    const char*     id()          const override { return "plaits"; }
    const char*     displayName() const override { return "Plaits"; }
    tdsp::MidiSink* midiSink()          override { return _ctx.sink; }
    void            setActive(bool active) override;
    void            panic() override;

    // Initialise gain to a defined silent state. Call once from setup()
    // before the first MIDI event arrives.
    void begin();

    // Per-slot fader (X32 form 0..1) and on/off — same convention as
    // MpeSlot. Switching slots away panics held notes; toggling on/off
    // / volume does NOT (a quick mute-tap doesn't kill a held tone).
    void setVolume(float v);
    void setOn(bool on);
    bool on() const { return _on; }
    float volume() const { return _volume; }

    // MPE master channel:
    //   0     — no master, every 1..16 allocates a voice (omni; matches
    //           the on-screen keyboard which sends ch 1)
    //   1..16 — MPE master; notes there are ignored, only mod wheel /
    //           sustain are honored
    void setMasterChannel(uint8_t ch);
    uint8_t masterChannel() const;

    // Slot OSC convention reuses the listenChannel name everywhere so
    // the snapshot generator can stay slot-agnostic — Plaits' "listen
    // channel" is just the master channel.
    uint8_t listenChannel() const { return masterChannel(); }
    void    setListenChannel(uint8_t ch) { setMasterChannel(ch); }

    // Direct access for OSC handlers that drive the engine's macros
    // (model / harmonics / timbre / morph / decay / resonance).
    PlaitsSink* sink() { return _ctx.sink; }

private:
    AudioContext _ctx;
    bool   _active = false;
    bool   _on     = true;
    float  _volume = 0.7f;

    void applyGain();
};

}  // namespace tdsp_synth
