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

void SignalGraphBinding::setMainHostvol(AudioAmplifier *hostvolL, AudioAmplifier *hostvolR) {
    _hostvolAmpL = hostvolL;
    _hostvolAmpR = hostvolR;
}

void SignalGraphBinding::setChannelRecAmp(int n, AudioAmplifier *amp) {
    if (n < 1 || n > kChannelCount) return;
    _channels[n].recAmp = amp;
}

void SignalGraphBinding::setChannelStereoMirror(int n, AudioMixer4 *mixerR, int slotR) {
    if (n < 1 || n > kChannelCount) return;
    _channels[n].mixerR = mixerR;
    _channels[n].slotR  = slotR;
}

void SignalGraphBinding::setMainLoop(AudioAmplifier *loopL, AudioAmplifier *loopR) {
    _loopAmpL = loopL;
    _loopAmpR = loopR;
}

void SignalGraphBinding::applyAll() {
    if (!_model) return;
    for (int n = 1; n <= kChannelCount; ++n) {
        applyChannel(n);
    }
    applyMain();
    applyMainLoop();  // pushes rec amps + loop tap
}

void SignalGraphBinding::applyChannel(int n) {
    if (!_model) return;
    if (n < 1 || n > kChannelCount) return;
    ChannelBinding &b = _channels[n];
    if (!b.mixer) return;

    // Mono mirror (legacy singleton): muted channel forced to 0, source
    // channel mirrored to a secondary slot.
    if (_mirrorActive && n == _mirrorMuteCh) {
        b.mixer->gain(b.slot, 0.0f);
        if (b.mixerR) b.mixerR->gain(b.slotR, 0.0f);
        applyChannelHpf(n);
        return;
    }
    const float gain = _model->effectiveChannelGain(n);
    b.mixer->gain(b.slot, gain);
    if (b.mixerR) b.mixerR->gain(b.slotR, gain);
    if (_mirrorActive && n == _mirrorSrcCh && _mirrorMixer) {
        _mirrorMixer->gain(_mirrorSlot, gain);
    }
    // HPF coefficient update is a separate apply step because it's
    // expensive; apply it only when HPF params actually changed.
    applyChannelHpf(n);
}

void SignalGraphBinding::applyAllChannelGains() {
    if (!_model) return;
    for (int n = 1; n <= kChannelCount; ++n) {
        ChannelBinding &b = _channels[n];
        if (!b.mixer) continue;
        if (_mirrorActive && n == _mirrorMuteCh) {
            b.mixer->gain(b.slot, 0.0f);
            if (b.mixerR) b.mixerR->gain(b.slotR, 0.0f);
            continue;
        }
        const float gain = _model->effectiveChannelGain(n);
        b.mixer->gain(b.slot, gain);
        if (b.mixerR) b.mixerR->gain(b.slotR, gain);
        if (_mirrorActive && n == _mirrorSrcCh && _mirrorMixer) {
            _mirrorMixer->gain(_mirrorSlot, gain);
        }
    }
}

void SignalGraphBinding::applyMain() {
    if (!_model) return;
    // Fader stage: per-side gain, killed by mute.
    if (_mainAmpL) _mainAmpL->gain(_model->effectiveMainFaderGainL());
    if (_mainAmpR) _mainAmpR->gain(_model->effectiveMainFaderGainR());
    // Hostvol stage: shared L/R, bypass when disabled.
    const float hv = _model->effectiveHostvolGain();
    if (_hostvolAmpL) _hostvolAmpL->gain(hv);
    if (_hostvolAmpR) _hostvolAmpR->gain(hv);
}

void SignalGraphBinding::applyChannelRec(int n) {
    if (!_model) return;
    if (n < 1 || n > kChannelCount) return;
    AudioAmplifier *amp = _channels[n].recAmp;
    if (!amp) return;
    // Loop is the master override: when engaged, all per-channel rec
    // sends are forced to 0 so sources already in the main mix don't
    // double-count in USB capture.
    const bool loopOn = _model->main().loopEnable;
    const bool send   = _model->channel(n).recSend;
    amp->gain((loopOn || !send) ? 0.0f : 1.0f);
}

void SignalGraphBinding::applyMainLoop() {
    if (!_model) return;
    const bool loopOn = _model->main().loopEnable;
    const float g = loopOn ? 1.0f : 0.0f;
    if (_loopAmpL) _loopAmpL->gain(g);
    if (_loopAmpR) _loopAmpR->gain(g);
    // Loop override affects every channel's rec gain — refresh them all.
    for (int n = 1; n <= kChannelCount; ++n) {
        applyChannelRec(n);
    }
}

void SignalGraphBinding::setMonoMirror(int srcCh, int muteCh,
                                       AudioMixer4 *targetMixer, int targetSlot) {
    _mirrorActive = true;
    _mirrorSrcCh  = srcCh;
    _mirrorMuteCh = muteCh;
    _mirrorMixer  = targetMixer;
    _mirrorSlot   = targetSlot;
}

void SignalGraphBinding::clearMonoMirror() {
    _mirrorActive = false;
    _mirrorSrcCh  = 0;
    _mirrorMuteCh = 0;
    _mirrorMixer  = nullptr;
    _mirrorSlot   = 0;
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
