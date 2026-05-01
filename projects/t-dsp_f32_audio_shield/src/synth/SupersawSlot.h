// SupersawSlot.h — ISynthSlot adapter for the JP-8000-style supersaw lead
// in lib/TDspSupersaw.
//
// Slot 6 in the new T-DSP synth-slot architecture. The engine itself
// (SupersawSink) is reused verbatim from the legacy
// projects/t-dsp_tac5212_audio_shield_adaptor wiring — it owns the
// 5-osc detuned saw stack, monophonic note-stack, portamento glide,
// state-variable filter, and ADSR envelope. This adapter exposes the
// slot surface (id / displayName / midiSink / setActive / panic) plus
// the per-slot housekeeping the OSC contract expects (volume, on,
// listenChannel — the latter delegates to SupersawSink::setMidiChannel).
//
// Audio path (mono int16 → F32 sub-mixer slot 6, dual-mono fan-out —
// same shape Dexed/MPE use):
//
//   g_supersawO1..O5 -> g_supersawMixAB (osc1..osc4)
//                    -> g_supersawMixFinal (mixAB + osc5)
//                    -> g_supersawFilt (state-variable LP)
//                    -> g_supersawEnv (ADSR)
//                    -> g_supersawToF32 (I16->F32)
//                    -> g_supersawGain (per-slot fader)
//                    -> g_synthSumLB[2] + g_synthSumRB[2]
//
// The audio nodes live at file scope in main.cpp; this adapter just
// steers their state.

#pragma once

#include <stdint.h>

#include <MidiSink.h>
#include <SupersawSink.h>

#include "SynthSlot.h"

class AudioEffectGain_F32;

namespace tdsp_synth {

class SupersawSlot : public ISynthSlot {
public:
    // Pointers to the engine sink + per-slot mono gain stage. Pointers
    // must outlive this object (they're file-scope statics in main.cpp).
    struct AudioContext {
        SupersawSink        *sink;
        AudioEffectGain_F32 *gain;
    };

    explicit SupersawSlot(const AudioContext &ctx);

    // ISynthSlot
    const char*     id()          const override { return "supersaw"; }
    const char*     displayName() const override { return "Supersaw"; }
    tdsp::MidiSink* midiSink()          override { return _ctx.sink; }
    void            setActive(bool active) override;
    void            panic() override;

    // Initialise the engine to the legacy-default state (matches the
    // legacy /synth/supersaw/* boot values from t-dsp_tac5212_audio_shield_adaptor).
    // Call once from setup() before the first MIDI event arrives.
    void begin();

    // Per-slot fader (X32 form 0..1) and on/off — same convention as
    // MultisampleSlot/MpeSlot. Switching slots away panics held notes;
    // toggling on/off / volume does NOT (a quick mute-tap doesn't kill
    // a sustained note).
    void setVolume(float v);
    void setOn(bool on);
    bool on() const { return _on; }
    float volume() const { return _volume; }

    // 0 = omni; 1..16 = single channel; >16 clamped to omni. Delegates
    // to SupersawSink::setMidiChannel under the hood.
    void    setListenChannel(uint8_t ch);
    uint8_t listenChannel() const;

    // Direct access for OSC handlers that drive the engine knobs
    // (detune / mix / cutoff / resonance / ADSR / portamento). Returning
    // a pointer rather than wrapping every accessor keeps this adapter
    // small — the OSC handler talks to the sink directly.
    SupersawSink* sink() { return _ctx.sink; }

private:
    AudioContext _ctx;
    bool   _active = false;
    bool   _on     = true;
    float  _volume = 0.6f;   // matches legacy g_supersawVolume cold-boot

    void applyGain();
};

}  // namespace tdsp_synth
