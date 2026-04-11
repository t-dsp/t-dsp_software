#include "SignalGraphBinding.h"

#include <Audio.h>  // stock I16 audio library: AudioMixer4, AudioAmplifier, AudioFilterBiquad

namespace tdsp {

SignalGraphBinding::SignalGraphBinding() {
    // Default-construct the channel bindings array.
    for (int n = 0; n <= kChannelCount; ++n) {
        _channels[n] = ChannelBinding{};
    }
}

void SignalGraphBinding::setChannel(int n,
                                    AudioMixer4 *mixer, int slot,
                                    AudioFilterBiquad *hpf,
                                    float sampleRateHz) {
    if (n < 1 || n > kChannelCount) return;
    _channels[n].mixer      = mixer;
    _channels[n].slot       = slot;
    _channels[n].hpf        = hpf;
    _channels[n].sampleRate = sampleRateHz;
}

void SignalGraphBinding::setMain(AudioAmplifier *ampL, AudioAmplifier *ampR) {
    _mainAmpL = ampL;
    _mainAmpR = ampR;
}

void SignalGraphBinding::applyAll() {
    if (!_model) return;
    for (int n = 1; n <= kChannelCount; ++n) {
        applyChannel(n);
    }
    applyMain();
}

void SignalGraphBinding::applyChannel(int n) {
    if (!_model) return;
    if (n < 1 || n > kChannelCount) return;
    ChannelBinding &b = _channels[n];
    if (!b.mixer) return;
    const float gain = _model->effectiveChannelGain(n);
    b.mixer->gain(b.slot, gain);
    // HPF coefficient update is a separate apply step because it's
    // expensive; apply it only when HPF params actually changed.
    applyChannelHpf(n);
}

void SignalGraphBinding::applyAllChannelGains() {
    if (!_model) return;
    for (int n = 1; n <= kChannelCount; ++n) {
        ChannelBinding &b = _channels[n];
        if (!b.mixer) continue;
        b.mixer->gain(b.slot, _model->effectiveChannelGain(n));
    }
}

void SignalGraphBinding::applyMain() {
    if (!_model) return;
    const float gain = _model->effectiveMainGain();
    if (_mainAmpL) _mainAmpL->gain(gain);
    if (_mainAmpR) _mainAmpR->gain(gain);
}

void SignalGraphBinding::applyChannelHpf(int n) {
    if (!_model) return;
    if (n < 1 || n > kChannelCount) return;
    ChannelBinding &b = _channels[n];
    if (!b.hpf) return;
    const Channel &ch = _model->channel(n);
    if (ch.hpfOn) {
        // Stock AudioFilterBiquad has setHighpass(stage, frequency, q).
        // Use stage 0 and a Q of 0.707 (Butterworth response).
        b.hpf->setHighpass(0, ch.hpfFreqHz, 0.707f);
    } else {
        // "Disable" by setting a very low cutoff that effectively passes
        // everything. The stock AudioFilterBiquad doesn't have a bypass
        // mode, so we use a 1 Hz high-pass as the "off" state — any
        // audible signal passes through unchanged.
        b.hpf->setHighpass(0, 1.0f, 0.707f);
    }
}

}  // namespace tdsp
