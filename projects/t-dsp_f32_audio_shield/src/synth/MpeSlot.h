// MpeSlot.h — ISynthSlot adapter for the MPE-aware virtual-analog
// engine in lib/TDspMPE.
//
// Slot 3 in the new T-DSP synth-slot architecture. The engine itself
// (MpeVaSink) is reused verbatim from the legacy
// projects/t-dsp_tac5212_audio_shield_adaptor wiring — it owns voice
// allocation, MPE channel-per-note routing, per-voice CC#74 → cutoff,
// and the LFO. This adapter just exposes the slot surface
// (id / displayName / midiSink / setActive / panic) plus the
// per-slot housekeeping the OSC contract expects (volume, on,
// listenChannel/masterChannel).
//
// MPE recap: with master channel set to 1..16, notes on that channel
// are global (mod wheel + sustain only); notes on the other 1..16
// channels each own a voice and have per-channel pitch bend, pressure,
// and CC#74 expression. Master channel = 0 disables MPE filtering, so
// a plain non-MPE controller plays the engine on any channel.
//
// Audio path (mono int16 → F32 sub-mixer slot 3, dual-mono fan-out —
// same shape Dexed uses):
//
//   g_mpeOsc[0..3] -> g_mpeFilter[0..3] -> g_mpeEnv[0..3]
//                                       -> g_mpeMix (4-into-1 mono)
//                                       -> g_mpeToF32 (I16->F32)
//                                       -> g_mpeGain (per-slot fader)
//                                       -> g_synthSumLA[3] + g_synthSumRA[3]
//
// As with the other slots, the audio nodes live at file scope in
// main.cpp; this adapter just steers their state.

#pragma once

#include <stdint.h>

#include <MidiSink.h>
#include <MpeVaSink.h>

#include "SynthSlot.h"

class AudioEffectGain_F32;

namespace tdsp_synth {

class MpeSlot : public ISynthSlot {
public:
    // Pointers to the engine sink + per-slot mono gain stage. Pointers
    // must outlive this object (they're file-scope statics in main.cpp).
    struct AudioContext {
        MpeVaSink           *sink;
        AudioEffectGain_F32 *gain;
    };

    explicit MpeSlot(const AudioContext &ctx);

    // ISynthSlot
    const char*     id()          const override { return "mpe"; }
    const char*     displayName() const override { return "MPE VA"; }
    tdsp::MidiSink* midiSink()          override { return _ctx.sink; }
    void            setActive(bool active) override;
    void            panic() override;

    // Initialise the engine to a defined silent state. Call once from
    // setup() before the first MIDI event arrives.
    void begin();

    // Per-slot fader (X32 form 0..1) and on/off — same convention as
    // MultisampleSlot. Switching slots away panics held notes;
    // toggling on/off / volume does NOT (a quick mute-tap doesn't kill
    // a sustained chord).
    void setVolume(float v);
    void setOn(bool on);
    bool on() const { return _on; }
    float volume() const { return _volume; }

    // MPE master channel (matches MpeVaSink semantics):
    //   0     — no master, every 1..16 allocates voices (non-MPE mode)
    //   1..16 — master channel; notes there are ignored, only global
    //           CCs (mod wheel / sustain) are honored
    // The OSC dispatcher in main.cpp uses `/synth/mpe/midi/master` for
    // this; existing dev-surface code already references it as
    // state.mpe.masterChannel.
    void setMasterChannel(uint8_t ch);
    uint8_t masterChannel() const;

    // Convenience accessor for the snapshot block. The slot's
    // "listen channel" in the OSC contract is actually the MPE master
    // channel; provided under both names so the snapshot generator
    // can reuse the same template across slots.
    uint8_t listenChannel() const { return masterChannel(); }
    void    setListenChannel(uint8_t ch) { setMasterChannel(ch); }

    // Direct access for OSC handlers that drive the engine's
    // attack / release / waveform / filter / LFO knobs. Returning a
    // pointer rather than wrapping every accessor keeps this adapter
    // small — the legacy wiring already assumes the OSC handler talks
    // to the sink directly.
    MpeVaSink* sink() { return _ctx.sink; }

private:
    AudioContext _ctx;
    bool   _active = false;
    bool   _on     = true;
    float  _volume = 0.7f;

    void applyGain();
};

}  // namespace tdsp_synth
