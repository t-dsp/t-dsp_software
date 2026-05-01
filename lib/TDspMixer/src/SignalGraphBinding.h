// SignalGraphBinding — maps a MixerModel onto concrete Teensy Audio
// Library objects.
//
// MVP v1 uses stock I16 audio objects (AudioMixer4, AudioAmplifier,
// AudioFilterBiquad). A future migration to F32 would touch this file
// but leave MixerModel and OscDispatcher unchanged. See
// ~/.claude/memory/decisions_mvp_v1_scope.md for the MVP vs F32
// decision.
//
// Usage: the example project creates the concrete audio objects (mixers,
// amplifiers, HPF biquads) in main.cpp, then registers them with the
// binding via the setChannel*() / setMain*() methods during setup().
// Whenever the model changes (via OSC handlers), call apply() or
// applyChannel(n) to push the new state into the audio graph.
//
// The binding assumes a specific audio graph topology — the one used by
// projects/t-dsp_tac5212_audio_shield_adaptor/. Alternate topologies
// (e.g., different channel counts, different routing) need a different
// binding subclass OR a configuration step that registers different slots.

#pragma once

#include "MixerModel.h"

#include <stdint.h>

class AudioMixer4;
class AudioAmplifier;
class AudioFilterBiquad;

namespace tdsp {

class SignalGraphBinding {
public:
    SignalGraphBinding();

    void setModel(MixerModel *model) { _model = model; }

    // Register the objects that carry channel state. Called during setup()
    // once per channel. `mixer` is the mixer that receives this channel's
    // signal (main L or main R), and `slot` is which input slot.
    // `hpf` is the per-channel HPF biquad (may be nullptr if HPF is not
    // wired for this channel). `sampleRate` is used for HPF coefficient
    // calculation.
    void setChannel(int n,
                    AudioMixer4 *mixer, int slot,
                    AudioFilterBiquad *hpf,
                    float sampleRateHz = 44100.0f);

    // Register the main bus FADER amplifier stages (one per L/R). These
    // receive `on ? faderL/faderR : 0`. The meter taps in the project
    // audio graph sit on these amplifiers' outputs, so meters read post-
    // fader / pre-hostvol (what you'd see on an X32 main LR meter).
    void setMain(AudioAmplifier *ampL, AudioAmplifier *ampR);

    // Register the main bus HOSTVOL amplifier stages (one per L/R).
    // These sit DOWNSTREAM of the fader amps in the audio graph and
    // receive `hostvolEnable ? hostvolValue : 1.0f`. The DAC output
    // and capture taps come from these amplifiers' outputs.
    void setMainHostvol(AudioAmplifier *hostvolL, AudioAmplifier *hostvolR);

    // Register a per-channel USB record-send amplifier. Sits between the
    // source and the capture mixer; gain is 1.0 when Channel.recSend is
    // true AND Main.loopEnable is false, else 0.0. May be nullptr for
    // channels that don't have a rec amp wired.
    void setChannelRecAmp(int n, AudioAmplifier *amp);

    // Register a right-side mirror mixer slot for a channel. Used when a
    // mono input (e.g. XLR mic) needs to feed BOTH the main L and main R
    // buses at the same fader gain. Unlike setMonoMirror (which is a
    // singleton), this is stored per-channel and scales to many channels.
    // Call after setChannel(); pass the same channel index.
    void setChannelStereoMirror(int n, AudioMixer4 *mixerR, int slotR);

    // Register the main bus LOOP tap amplifiers (one per L/R). These
    // sit on the post-mainAmp / pre-hostvolAmp signal and feed the
    // capture mixer. Gain is 1.0 when Main.loopEnable is true, else 0.0.
    // When loop is enabled the per-channel rec amps are forced to 0 so
    // sources that also sit in the main mix don't double-count in USB out.
    void setMainLoop(AudioAmplifier *loopL, AudioAmplifier *loopR);

    // Push all model state into the registered audio objects. Call this
    // after a batch of model changes or once during setup() to initialize
    // the audio graph.
    void applyAll();

    // Push a single channel's state. Use this when only one channel
    // changed for efficiency.
    void applyChannel(int n);

    // Push the main state (fader, on, hostvol).
    void applyMain();

    // Compute and apply the HPF biquad coefficient for a channel. Called
    // internally by applyChannel() and externally when HPF params change.
    void applyChannelHpf(int n);

    // When any channel's solo flag changes, all other channels' effective
    // gain may change (solo-in-place). Call this to refresh all channels'
    // gains at once without rebuilding coefficients.
    void applyAllChannelGains();

    // Push a single channel's record-send state (recSend + loop override).
    void applyChannelRec(int n);

    // Push the main loop-tap state and refresh every channel's rec gain.
    // Call whenever Main.loopEnable flips, since the override rule affects
    // every channel's effective rec gain.
    void applyMainLoop();

    // Mono mirror: when active, applyChannel(srcCh) also sets
    // targetMixer->gain(targetSlot, effectiveChannelGain(srcCh)), and
    // applyChannel(muteCh) forces gain to 0 regardless of the model.
    // Used for line-input mono/differential mode where CH1's signal
    // cross-feeds to both L and R, and CH2 is silenced.
    void setMonoMirror(int srcCh, int muteCh,
                       AudioMixer4 *targetMixer, int targetSlot);
    void clearMonoMirror();

private:
    MixerModel *_model = nullptr;

    struct ChannelBinding {
        AudioMixer4       *mixer       = nullptr;
        int                slot        = 0;
        AudioFilterBiquad *hpf         = nullptr;
        float              sampleRate  = 44100.0f;
        AudioAmplifier    *recAmp      = nullptr;
        // Optional right-side mirror. When set, applyChannel() writes the
        // effective gain to `mixerR.gain(slotR)` in addition to the primary
        // mixer. Used for mono-to-stereo fanout (e.g. XLR mic into both L+R).
        AudioMixer4       *mixerR      = nullptr;
        int                slotR       = 0;
    };
    ChannelBinding _channels[kChannelCount + 1];  // 1-indexed; [0] unused

    AudioAmplifier *_mainAmpL    = nullptr;
    AudioAmplifier *_mainAmpR    = nullptr;
    AudioAmplifier *_hostvolAmpL = nullptr;
    AudioAmplifier *_hostvolAmpR = nullptr;
    AudioAmplifier *_loopAmpL    = nullptr;
    AudioAmplifier *_loopAmpR    = nullptr;

    // Mono mirror state
    bool        _mirrorActive    = false;
    int         _mirrorSrcCh     = 0;
    int         _mirrorMuteCh    = 0;
    AudioMixer4 *_mirrorMixer    = nullptr;
    int         _mirrorSlot      = 0;
};

}  // namespace tdsp
