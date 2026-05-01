// ChipSlot.h — ISynthSlot adapter for the NES/Gameboy chiptune engine
// in lib/TDspChip.
//
// Slot 7 in the new T-DSP synth-slot architecture. The engine itself
// (ChipSink) is reused verbatim from the legacy
// projects/t-dsp_tac5212_audio_shield_adaptor wiring — it owns the
// monophonic last-note voice, the 2-pulse + triangle + noise mixing,
// the per-step arpeggiator, and the ADSR envelope. This adapter just
// exposes the slot surface (id / displayName / midiSink / setActive /
// panic) plus the per-slot housekeeping the OSC contract expects
// (volume, on, listenChannel).
//
// Audio path (mono int16 → F32 sub-mixer slot 7, dual-mono fan-out —
// same shape Dexed and MPE use):
//
//   chipPulse1 / chipPulse2 / chipTriangle / chipNoise
//     -> chipMix (4-into-1 mono)
//     -> chipEnv (ADSR)
//     -> chipToF32 (I16->F32)
//     -> chipGain (per-slot fader)
//     -> g_synthSumLB[3] + g_synthSumRB[3]
//
// As with the other slots, the audio nodes live at file scope in
// main.cpp; this adapter just steers their state.

#pragma once

#include <stdint.h>

#include <ChipSink.h>
#include <MidiSink.h>

#include "SynthSlot.h"

class AudioEffectGain_F32;

namespace tdsp_synth {

class ChipSlot : public ISynthSlot {
public:
    // Pointers to the engine sink + per-slot mono gain stage. Pointers
    // must outlive this object (they're file-scope statics in main.cpp).
    struct AudioContext {
        ChipSink            *sink;
        AudioEffectGain_F32 *gain;
    };

    explicit ChipSlot(const AudioContext &ctx);

    // ISynthSlot
    const char*     id()          const override { return "chip"; }
    const char*     displayName() const override { return "Chip"; }
    tdsp::MidiSink* midiSink()          override { return _ctx.sink; }
    void            setActive(bool active) override;
    void            panic() override;

    // Initialise the engine to a defined silent state and push initial
    // gain. Call once from setup() before the first MIDI event arrives.
    void begin();

    // Per-slot fader (X32 form 0..1) and on/off — same convention as
    // MultisampleSlot / MpeSlot. Switching slots away panics held notes;
    // toggling on/off / volume does NOT.
    void setVolume(float v);
    void setOn(bool on);
    bool on() const { return _on; }
    float volume() const { return _volume; }

    // 0 = omni, 1..16 = single channel. Mirrors ChipSink::setMidiChannel
    // semantics; included here so the OSC handler can stay slot-agnostic.
    void setListenChannel(uint8_t ch);
    uint8_t listenChannel() const;

    // Direct access for OSC handlers that drive the engine's
    // pulse / triangle / noise / arp / ADSR knobs. Same shape as
    // MpeSlot::sink() — keeps this adapter small.
    ChipSink* sink() { return _ctx.sink; }

private:
    AudioContext _ctx;
    bool   _active = false;
    bool   _on     = true;
    float  _volume = 0.6f;

    void applyGain();
};

}  // namespace tdsp_synth
