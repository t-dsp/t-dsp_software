// MixerModel — plain-data mixer state for the small mixer MVP.
//
// The mixer model is the source of truth for channel/main state. OSC
// handlers mutate this model; SignalGraphBinding reads the model and pushes
// values into the audio graph. Clients echo model changes back to
// subscribers.
//
// Scope for MVP v1 (stripped small mixer): fader, mute (on), solo (solo-
// in-place), name, and per-channel HPF (single biquad, configurable on/off
// and cutoff frequency). No buses, no sends, no EQ bands, no pan, no
// scenes. Everything that's cut is captured in
// ~/.claude/memory/decisions_mvp_v1_scope.md — re-reading that before
// adding fields is mandatory.
//
// X32 conventions followed:
//   - fader is normalized 0..1 on the wire, not dB
//   - "on" means unmuted (on=true is the normal state, on=false is muted)
//   - stereo linking: odd channel of a linked pair carries link=true;
//     writes to either channel of the pair propagate to both

#pragma once

#include <stdint.h>

namespace tdsp {

// ----- Compile-time configuration -----

// Number of input channels for the small mixer. 6 = USB L/R, Line L/R,
// Mic L/R (onboard stereo PDM mic). Each channel is 1-indexed in OSC
// (/ch/01, /ch/02, ...) but 0-indexed in this array.
constexpr int kChannelCount = 6;

// Maximum length of a channel name including null terminator.
constexpr int kChannelNameMax = 16;

// ----- Data types -----

struct Channel {
    float   fader      = 1.0f;       // 0.0..1.0 normalized (X32 convention)
    bool    on         = true;       // false = muted
    bool    solo       = false;      // X32-style solo-in-place
    bool    link       = false;      // odd channel: linked with next even
    // Per-channel HPF (single biquad band).
    bool    hpfOn      = false;
    float   hpfFreqHz  = 80.0f;      // default cutoff
    // Per-channel USB record send. When true, this channel's pre-fader
    // signal is summed into the USB capture mixer (what the host records).
    // Defaults are per-source in reset(): Line/Mic ON, USB playback OFF
    // (feedback risk if a DAW monitors the input). Overridden to 0 in the
    // binding when Main.loopEnable is true.
    bool    recSend    = false;
    // User-settable label. Defaults set in reset() below.
    char    name[kChannelNameMax] = {0};
};

struct Main {
    // Stereo main faders. `link` ganged by default — writes to either
    // side propagate to the other. Unlink to run L/R independently (e.g.,
    // for balance trim or mono-sum debugging).
    float   faderL     = 0.75f;
    float   faderR     = 0.75f;
    bool    link       = true;
    bool    on         = true;
    // hostvol: pre-main attenuator fed by usbIn.volume() via USB Audio
    // Class feature unit. Lives AFTER the main fader stage in the audio
    // graph (so the master meters show the post-fader / pre-hostvol
    // level). When enabled, gain multiplier = hostvolValue. When
    // disabled, gain multiplier = 1.0 (bypass).
    bool    hostvolEnable = true;
    float   hostvolValue  = 1.0f;    // last observed usbIn.volume() scaled
    // Loopback: when true, the post-fader / pre-hostvol main mix is
    // tapped into USB capture, and per-channel recSend is forced off.
    // Lets you record whatever is in the headphones (for demo videos,
    // screen recording). Default false preserves prior behaviour.
    bool    loopEnable    = false;
};

// ----- MixerModel -----

class MixerModel {
public:
    MixerModel();

    // Reset to defaults (1-index channel names, all on, faders at 1.0,
    // main at 0.75, no solos, no links).
    void reset();

    // Channel accessors — 1-indexed to match OSC paths. ch(0) is illegal;
    // ch(1)..ch(kChannelCount) are valid. Returns a sentinel for out-of-
    // range indices so callers never crash.
    Channel       &channel(int n);
    const Channel &channel(int n) const;

    Main       &main()       { return _main; }
    const Main &main() const { return _main; }

    // Stereo-linking-aware setters. If channel N is part of a linked pair,
    // the write is propagated to the partner channel too (X32 "same
    // channel" semantics per roadmap open question 7).
    //
    // Return true if the write changed the model; false if it was a no-op
    // (same value already there). Callers use this to decide whether to
    // trigger a binding update and echo.
    bool setChannelFader(int n, float value);
    bool setChannelOn(int n, bool on);
    bool setChannelSolo(int n, bool solo);
    bool setChannelHpfOn(int n, bool on);
    bool setChannelHpfFreq(int n, float hz);
    bool setChannelName(int n, const char *name);
    bool setChannelLink(int n, bool linked);

    // Writes to either L or R propagate to the other when link=true.
    // Each returns true if the model changed.
    bool setMainFaderL(float value);
    bool setMainFaderR(float value);
    bool setMainLink(bool linked);
    bool setMainOn(bool on);
    bool setMainHostvolEnable(bool enable);
    bool setMainHostvolValue(float value);
    bool setMainLoopEnable(bool enable);
    bool setChannelRecSend(int n, bool send);

    // Solo-in-place computation: returns true if ANY channel is soloed.
    // The binding uses this to decide mute-on-non-soloed behavior.
    bool anySoloActive() const;

    // Compute the *effective* gain for a channel, taking into account:
    //   - channel.fader
    //   - channel.on (muted if false)
    //   - solo-in-place (muted if any channel is soloed AND this one isn't)
    // This is the value the binding pushes into the audio object.
    float effectiveChannelGain(int n) const;

    // Effective gain for the L main fader stage (pre-hostvol). This is
    // what SignalGraphBinding pushes to the L main amplifier.
    //   on ? faderL : 0
    float effectiveMainFaderGainL() const;

    // Effective gain for the R main fader stage (pre-hostvol).
    float effectiveMainFaderGainR() const;

    // Effective gain for the hostvol attenuator stage, independent of
    // L/R (stereo-linked). Pushed to BOTH hostvol amplifiers.
    //   hostvolEnable ? hostvolValue : 1.0f
    float effectiveHostvolGain() const;

private:
    Channel _channels[kChannelCount + 1];  // 1-indexed; [0] is a sentinel
    Main    _main;
};

}  // namespace tdsp
