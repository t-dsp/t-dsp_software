// AcidSlot.h — ISynthSlot adapter for the TB-303-style monophonic
// bass engine in lib/TDspAcid.
//
// Slot 5 in the new T-DSP synth-slot architecture. The engine itself
// (AcidSink) is reused verbatim from the legacy
// projects/t-dsp_tac5212_audio_shield_adaptor wiring — it owns
// last-note priority, the slide-only-when-overlapping rule, accent,
// and the software-driven filter envelope. This adapter exposes the
// slot surface (id / displayName / midiSink / setActive / panic) plus
// the per-slot housekeeping the OSC contract expects (volume, on,
// listenChannel) and forwards loop()-driven ticks to the engine.
//
// Audio path (mono int16 → F32 sub-mixer slot 5, dual-mono fan-out):
//
//   g_acidOsc -> g_acidFilter -> g_acidAmpEnv
//             -> g_acidToF32 (I16->F32)
//             -> g_acidGain  (per-slot fader)
//             -> g_synthSumLB[1] + g_synthSumRB[1]
//
// As with the other slots, the audio nodes live at file scope in
// main.cpp; this adapter just steers their state.

#pragma once

#include <stdint.h>

#include <AcidSink.h>
#include <MidiSink.h>

#include "SynthSlot.h"

class AudioEffectGain_F32;

namespace tdsp_synth {

class AcidSlot : public ISynthSlot {
public:
    // Pointers to the engine sink + per-slot mono gain stage. Pointers
    // must outlive this object (they're file-scope statics in main.cpp).
    struct AudioContext {
        AcidSink            *sink;
        AudioEffectGain_F32 *gain;
    };

    explicit AcidSlot(const AudioContext &ctx);

    // ISynthSlot
    const char*     id()          const override { return "acid"; }
    const char*     displayName() const override { return "Acid"; }
    tdsp::MidiSink* midiSink()          override { return _ctx.sink; }
    void            setActive(bool active) override;
    void            panic() override;

    // Initialise the engine to a defined silent state. Call once from
    // setup() before the first MIDI event arrives.
    void begin();

    // Drives the engine's software-running filter envelope and slide
    // glide. AcidSink::tick is a cheap no-op when the voice is idle
    // and there's no pitch glide in flight.
    void tick(uint32_t nowMs);

    // Per-slot fader (X32 form 0..1) and on/off — same convention as
    // MpeSlot / MultisampleSlot. Switching slots away panics held
    // notes; toggling on/off / volume does NOT.
    void  setVolume(float v);
    void  setOn(bool on);
    bool  on() const { return _on; }
    float volume() const { return _volume; }

    // 0 = omni; 1..16 = single channel; >16 clamped to omni.
    void    setListenChannel(uint8_t ch);
    uint8_t listenChannel() const;

    // Direct access for OSC handlers that drive engine knobs
    // (waveform, tuning, cutoff, resonance, env mod / decay, accent,
    // slide). Returning a pointer rather than wrapping every accessor
    // keeps this adapter small — the legacy wiring already assumes
    // the OSC handler talks to the sink directly.
    AcidSink* sink() { return _ctx.sink; }

private:
    AudioContext _ctx;
    bool   _active = false;
    bool   _on     = true;
    float  _volume = 0.7f;

    void applyGain();
};

}  // namespace tdsp_synth
