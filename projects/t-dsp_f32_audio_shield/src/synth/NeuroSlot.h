// NeuroSlot.h — ISynthSlot adapter for the reese / neuro bass engine in
// lib/TDspNeuro.
//
// Slot 4 in the new T-DSP synth-slot architecture. The engine itself
// (NeuroSink) is reused verbatim from the legacy
// projects/t-dsp_tac5212_audio_shield_adaptor wiring — it owns the
// 3-saw + sub-sine voice, monophonic note-stack with last-note priority,
// portamento glide, state-variable filter, ADSR envelope, and an LFO.
// This adapter exposes the slot surface (id / displayName / midiSink /
// setActive / panic) plus the per-slot housekeeping the OSC contract
// expects (volume, on, listenChannel — the latter delegates to
// NeuroSink::setMidiChannel).
//
// Stink chain (multiband destruction) lives in the legacy project but
// is intentionally out of scope for this slot rebuild. It can be re-added
// later either as a per-slot effect block or as a shared FX bus tap.
//
// Audio path (mono int16 → F32 sub-mixer slot 4, dual-mono fan-out —
// same shape Dexed/MPE/Supersaw use):
//
//   g_neuroOsc1/2/3 + g_neuroOscSub -> g_neuroVoiceMix (4-in)
//                                   -> g_neuroFilt (state-variable LP)
//                                   -> g_neuroEnv (ADSR)
//                                   -> g_neuroToF32 (I16->F32)
//                                   -> g_neuroGain (per-slot fader)
//                                   -> g_synthSumLB[0] + g_synthSumRB[0]
//
// The audio nodes live at file scope in main.cpp; this adapter just
// steers their state.

#pragma once

#include <stdint.h>

#include <MidiSink.h>
#include <NeuroSink.h>

#include "SynthSlot.h"

class AudioEffectGain_F32;

namespace tdsp_synth {

class NeuroSlot : public ISynthSlot {
public:
    // Pointers to the engine sink + per-slot mono gain stage. Pointers
    // must outlive this object (they're file-scope statics in main.cpp).
    struct AudioContext {
        NeuroSink           *sink;
        AudioEffectGain_F32 *gain;
    };

    explicit NeuroSlot(const AudioContext &ctx);

    // ISynthSlot
    const char*     id()          const override { return "neuro"; }
    const char*     displayName() const override { return "Neuro"; }
    tdsp::MidiSink* midiSink()          override { return _ctx.sink; }
    void            setActive(bool active) override;
    void            panic() override;

    // Initialise the engine to the legacy-default state (matches the
    // legacy /synth/neuro/* boot values from t-dsp_tac5212_audio_shield_adaptor).
    // Call once from setup() before the first MIDI event arrives.
    void begin();

    // Per-slot fader (X32 form 0..1) and on/off — same convention as
    // MultisampleSlot/MpeSlot/SupersawSlot. Switching slots away panics
    // held notes; toggling on/off / volume does NOT (a quick mute-tap
    // doesn't kill a sustained note).
    void setVolume(float v);
    void setOn(bool on);
    bool on() const { return _on; }
    float volume() const { return _volume; }

    // 0 = omni; 1..16 = single channel; >16 clamped to omni. Delegates
    // to NeuroSink::setMidiChannel under the hood.
    void    setListenChannel(uint8_t ch);
    uint8_t listenChannel() const;

    // Direct access for OSC handlers that drive the engine knobs
    // (detune / sub / osc3 / cutoff / resonance / ADSR / LFO / portamento).
    // Returning a pointer rather than wrapping every accessor keeps
    // this adapter small — the OSC handler talks to the sink directly.
    NeuroSink* sink() { return _ctx.sink; }

private:
    AudioContext _ctx;
    bool   _active = false;
    bool   _on     = true;
    float  _volume = 0.7f;   // matches legacy g_neuroVolume cold-boot

    void applyGain();
};

}  // namespace tdsp_synth
