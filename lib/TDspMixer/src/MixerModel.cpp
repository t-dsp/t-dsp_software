#include "MixerModel.h"

#include <string.h>
#include <stdio.h>

namespace tdsp {

namespace {
// Default channel names for the small mixer MVP. Matches the decisions in
// ~/.claude/memory/decisions_mvp_v1_scope.md. Users can override via
// /ch/NN/config/name.
static const char *kDefaultNames[kChannelCount + 1] = {
    "",        // [0] sentinel, unused
    "USB L",
    "USB R",
    "Line L",
    "Line R",
    "Mic L",
    "Mic R",
    "XLR 1",   // 7  TLV320ADC6140 CH1 (external XLR preamp)
    "XLR 2",   // 8
    "XLR 3",   // 9
    "XLR 4",   // 10
};

// Default stereo-link flags. Odd channels of each pair are linked with
// their even neighbor: (1,2), (3,4), (5,6). The XLR channels default
// unlinked because they're individual mono mic inputs (SM58 etc.), not
// natural L/R pairs.
static bool defaultLink(int n) {
    return (n == 1) || (n == 3) || (n == 5);
}

// Default per-channel USB record send. Line L/R (3,4), Mic L/R (5,6) and
// XLR 1..4 (7..10) recorded at unity by default; USB playback L/R (1,2)
// explicitly excluded to avoid feedback when a DAW monitors input.
static bool defaultRecSend(int n) {
    return (n >= 3 && n <= kChannelCount);
}
}  // namespace

MixerModel::MixerModel() {
    reset();
}

void MixerModel::reset() {
    for (int n = 1; n <= kChannelCount; ++n) {
        Channel &ch = _channels[n];
        ch.fader     = 1.0f;
        // Mic L/R (5,6) start muted — onboard PDM mics are usually
        // unwanted at boot (feedback into headphones/monitor).
        // XLR 1..4 (7..10) also start muted because floating XLR inputs
        // (no mic plugged in) at 24 dB analog preamp gain dump amplified
        // EMI / hum into the main bus. User unmutes per channel as mics
        // get plugged in.
        ch.on        = !(n == 5 || n == 6 || (n >= 7 && n <= 10));
        ch.solo      = false;
        ch.link      = defaultLink(n);
        ch.hpfOn     = false;
        ch.hpfFreqHz = 80.0f;
        ch.recSend   = defaultRecSend(n);
        strncpy(ch.name, kDefaultNames[n], kChannelNameMax - 1);
        ch.name[kChannelNameMax - 1] = '\0';
    }
    _main.faderL         = 0.75f;
    _main.faderR         = 0.75f;
    _main.link           = true;
    _main.on             = true;
    _main.hostvolEnable  = true;
    _main.hostvolValue   = 1.0f;
    _main.loopEnable     = false;
}

Channel &MixerModel::channel(int n) {
    if (n < 1 || n > kChannelCount) return _channels[0];
    return _channels[n];
}

const Channel &MixerModel::channel(int n) const {
    if (n < 1 || n > kChannelCount) return _channels[0];
    return _channels[n];
}

// ----- Linked-partner helper -----
//
// Returns the partner channel index if `n` is part of a linked pair, else 0.
// The convention: odd channel N carries link=true, and the partner is N+1.
// The even channel also needs to respond to writes to itself, so if N is
// even, check whether N-1 carries link=true.
static int linkedPartner(const MixerModel &model, int n) {
    if (n < 1 || n > kChannelCount) return 0;
    // Odd side: partner is n+1 if link flag is set and n+1 is in range.
    if ((n & 1) == 1) {
        if (n + 1 <= kChannelCount && model.channel(n).link) return n + 1;
        return 0;
    }
    // Even side: check the odd predecessor.
    if (n - 1 >= 1 && model.channel(n - 1).link) return n - 1;
    return 0;
}

// ----- Setters -----
// Each setter returns true if the model changed. Linking propagates to the
// partner channel *without* returning a separate flag; the caller treats a
// linked write as a single atomic edit.

bool MixerModel::setChannelFader(int n, float value) {
    if (n < 1 || n > kChannelCount) return false;
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    Channel &ch = _channels[n];
    bool changed = (ch.fader != value);
    ch.fader = value;
    int partner = linkedPartner(*this, n);
    if (partner) {
        Channel &pch = _channels[partner];
        if (pch.fader != value) { pch.fader = value; changed = true; }
    }
    return changed;
}

bool MixerModel::setChannelOn(int n, bool on) {
    if (n < 1 || n > kChannelCount) return false;
    Channel &ch = _channels[n];
    bool changed = (ch.on != on);
    ch.on = on;
    int partner = linkedPartner(*this, n);
    if (partner) {
        Channel &pch = _channels[partner];
        if (pch.on != on) { pch.on = on; changed = true; }
    }
    return changed;
}

bool MixerModel::setChannelSolo(int n, bool solo) {
    if (n < 1 || n > kChannelCount) return false;
    Channel &ch = _channels[n];
    bool changed = (ch.solo != solo);
    ch.solo = solo;
    int partner = linkedPartner(*this, n);
    if (partner) {
        Channel &pch = _channels[partner];
        if (pch.solo != solo) { pch.solo = solo; changed = true; }
    }
    return changed;
}

bool MixerModel::setChannelHpfOn(int n, bool on) {
    if (n < 1 || n > kChannelCount) return false;
    Channel &ch = _channels[n];
    bool changed = (ch.hpfOn != on);
    ch.hpfOn = on;
    int partner = linkedPartner(*this, n);
    if (partner) {
        Channel &pch = _channels[partner];
        if (pch.hpfOn != on) { pch.hpfOn = on; changed = true; }
    }
    return changed;
}

bool MixerModel::setChannelHpfFreq(int n, float hz) {
    if (n < 1 || n > kChannelCount) return false;
    if (hz < 10.0f) hz = 10.0f;
    if (hz > 500.0f) hz = 500.0f;
    Channel &ch = _channels[n];
    bool changed = (ch.hpfFreqHz != hz);
    ch.hpfFreqHz = hz;
    int partner = linkedPartner(*this, n);
    if (partner) {
        Channel &pch = _channels[partner];
        if (pch.hpfFreqHz != hz) { pch.hpfFreqHz = hz; changed = true; }
    }
    return changed;
}

bool MixerModel::setChannelName(int n, const char *name) {
    if (n < 1 || n > kChannelCount || !name) return false;
    Channel &ch = _channels[n];
    bool changed = (strncmp(ch.name, name, kChannelNameMax - 1) != 0);
    strncpy(ch.name, name, kChannelNameMax - 1);
    ch.name[kChannelNameMax - 1] = '\0';
    // Names are intentionally NOT propagated across linked pairs — each
    // channel has its own name (e.g. "USB L" stays "USB L", "USB R" stays
    // "USB R" even when they're linked for fader/mute/solo).
    return changed;
}

bool MixerModel::setChannelLink(int n, bool linked) {
    if (n < 1 || n > kChannelCount) return false;
    // Only the ODD channel of a pair carries the link flag. If called on
    // an even channel, reject silently (no-op).
    if ((n & 1) == 0) return false;
    Channel &ch = _channels[n];
    bool changed = (ch.link != linked);
    ch.link = linked;
    return changed;
}

bool MixerModel::setMainFaderL(float value) {
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    bool changed = (_main.faderL != value);
    _main.faderL = value;
    if (_main.link && _main.faderR != value) {
        _main.faderR = value;
        changed = true;
    }
    return changed;
}

bool MixerModel::setMainFaderR(float value) {
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    bool changed = (_main.faderR != value);
    _main.faderR = value;
    if (_main.link && _main.faderL != value) {
        _main.faderL = value;
        changed = true;
    }
    return changed;
}

bool MixerModel::setMainLink(bool linked) {
    bool changed = (_main.link != linked);
    _main.link = linked;
    // When link is turned ON, snap R to L so both sides start in sync.
    // (X32 convention: odd/L side is canonical when linking.)
    if (linked && _main.faderR != _main.faderL) {
        _main.faderR = _main.faderL;
        changed = true;
    }
    return changed;
}

bool MixerModel::setMainOn(bool on) {
    bool changed = (_main.on != on);
    _main.on = on;
    return changed;
}

bool MixerModel::setMainHostvolEnable(bool enable) {
    bool changed = (_main.hostvolEnable != enable);
    _main.hostvolEnable = enable;
    return changed;
}

bool MixerModel::setMainHostvolValue(float value) {
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    bool changed = (_main.hostvolValue != value);
    _main.hostvolValue = value;
    return changed;
}

bool MixerModel::setMainLoopEnable(bool enable) {
    bool changed = (_main.loopEnable != enable);
    _main.loopEnable = enable;
    return changed;
}

bool MixerModel::setChannelRecSend(int n, bool send) {
    if (n < 1 || n > kChannelCount) return false;
    Channel &ch = _channels[n];
    bool changed = (ch.recSend != send);
    ch.recSend = send;
    int partner = linkedPartner(*this, n);
    if (partner) {
        Channel &pch = _channels[partner];
        if (pch.recSend != send) { pch.recSend = send; changed = true; }
    }
    return changed;
}

// ----- Effective gain computation -----

bool MixerModel::anySoloActive() const {
    for (int n = 1; n <= kChannelCount; ++n) {
        if (_channels[n].solo) return true;
    }
    return false;
}

float MixerModel::effectiveChannelGain(int n) const {
    if (n < 1 || n > kChannelCount) return 0.0f;
    const Channel &ch = _channels[n];
    if (!ch.on) return 0.0f;
    if (anySoloActive() && !ch.solo) return 0.0f;
    return ch.fader;
}

float MixerModel::effectiveMainFaderGainL() const {
    if (!_main.on) return 0.0f;
    return _main.faderL;
}

float MixerModel::effectiveMainFaderGainR() const {
    if (!_main.on) return 0.0f;
    return _main.faderR;
}

float MixerModel::effectiveHostvolGain() const {
    return _main.hostvolEnable ? _main.hostvolValue : 1.0f;
}

}  // namespace tdsp
