#include "OscDispatcher.h"

#include "MixerModel.h"
#include "SignalGraphBinding.h"
#include "CodecPanel.h"
#include "MeterEngine.h"

#include <OSCMessage.h>
#include <OSCBundle.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

namespace tdsp {

OscDispatcher::OscDispatcher() = default;

// ----- Address parsing helpers -----

int OscDispatcher::parseChannelAddress(const char *address, const char **outSuffix) {
    // Expects "/ch/NN/..." — returns NN and sets outSuffix to the char
    // after "/ch/NN". Returns 0 if the address doesn't match the pattern.
    if (!address) return 0;
    if (strncmp(address, "/ch/", 4) != 0) return 0;
    const char *p = address + 4;
    // Parse up to 3 digits for the channel number (supports 01..255).
    int n = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9' && digits < 3) {
        n = n * 10 + (*p - '0');
        ++p;
        ++digits;
    }
    if (digits == 0) return 0;
    if (outSuffix) *outSuffix = p;
    return n;
}

// ----- Route entry point -----

void OscDispatcher::route(OSCMessage &msg, OSCBundle &reply) {
    if (msg.hasError() || !_model) return;

    // CNMAT/OSC's OSCMessage::getAddress(char*, int, int) copies the
    // address into a buffer at the given offset. We want the full address
    // — use getAddress(buf, 0). The Teensy-compatible API is:
    //   int getAddress(char *buffer, int offset, int len)
    // but easier to call msg.getAddress(buf, 0, len).
    //
    // Working around API weirdness: CNMAT/OSC has multiple getAddress
    // overloads and some branches use `getAddress(buffer, offset)` without
    // length. Be defensive.
    char address[128];
    int addrLen = msg.getAddress(address, 0, sizeof(address) - 1);
    if (addrLen < 0) addrLen = 0;
    address[addrLen] = '\0';

    // -- /codec/<model>/... → registered codec panel --
    if (strncmp(address, "/codec/", 7) == 0 && _codecPanel) {
        const char *after = address + 7;  // points at <model>/...
        const char *model = _codecPanel->modelName();
        size_t modelLen = strlen(model);
        if (strncmp(after, model, modelLen) == 0 &&
            (after[modelLen] == '/' || after[modelLen] == '\0')) {
            // addrOffset = position after "/codec/<model>" in the address.
            int addrOffset = 7 + (int)modelLen;
            _codecPanel->route(msg, addrOffset, reply);
            return;
        }
    }

    // -- /ch/NN/... --
    const char *chSuffix = nullptr;
    int chNum = parseChannelAddress(address, &chSuffix);
    if (chNum > 0 && chSuffix) {
        if (strcmp(chSuffix, "/mix/fader") == 0) {
            handleChannelFader(chNum, msg, reply);
            return;
        }
        if (strcmp(chSuffix, "/mix/on") == 0) {
            handleChannelOn(chNum, msg, reply);
            return;
        }
        if (strcmp(chSuffix, "/mix/solo") == 0) {
            handleChannelSolo(chNum, msg, reply);
            return;
        }
        if (strcmp(chSuffix, "/config/name") == 0) {
            handleChannelName(chNum, msg, reply);
            return;
        }
        if (strcmp(chSuffix, "/config/link") == 0) {
            handleChannelLink(chNum, msg, reply);
            return;
        }
        if (strcmp(chSuffix, "/preamp/hpf/on") == 0) {
            handleChannelHpfOn(chNum, msg, reply);
            return;
        }
        if (strcmp(chSuffix, "/preamp/hpf/f") == 0) {
            handleChannelHpfFreq(chNum, msg, reply);
            return;
        }
    }

    // -- /main/st/... --
    if (strcmp(address, "/main/st/mix/fader") == 0) {
        handleMainFader(msg, reply);
        return;
    }
    if (strcmp(address, "/main/st/mix/on") == 0) {
        handleMainOn(msg, reply);
        return;
    }
    if (strcmp(address, "/main/st/hostvol/enable") == 0) {
        handleMainHostvolEnable(msg, reply);
        return;
    }

    // -- /sub (subscriptions — accepted but meters always stream for MVP) --
    if (strcmp(address, "/sub") == 0) {
        handleSub(msg, reply);
        return;
    }

    // -- /info (read-only device identity) --
    if (strcmp(address, "/info") == 0) {
        handleInfo(msg, reply);
        return;
    }

    // Unknown address — silently drop. The web_dev_surface's console pane
    // shows decode errors and unknowns via its own path; we don't echo
    // anything back here.
}

// ----- Per-leaf handlers -----
//
// Pattern for each handler:
//   1. Validate arg type.
//   2. Mutate model via its setter; returns true if changed.
//   3. Call binding apply.
//   4. Append echo to reply bundle.

void OscDispatcher::handleChannelFader(int n, OSCMessage &msg, OSCBundle &reply) {
    if (!msg.isFloat(0)) return;
    float v = msg.getFloat(0);
    bool changed = _model->setChannelFader(n, v);
    if (_binding) {
        _binding->applyChannel(n);
        // Linked partner may also need applying.
        int partner = ((n & 1) == 1 && n < kChannelCount &&
                       _model->channel(n).link) ? n + 1 : 0;
        if (partner == 0 && (n & 1) == 0 && n > 1 &&
            _model->channel(n - 1).link) partner = n - 1;
        if (partner) _binding->applyChannel(partner);
    }
    (void)changed;  // always echo, even for no-op writes, for idempotence
    char addr[32];
    snprintf(addr, sizeof(addr), "/ch/%02d/mix/fader", n);
    OSCMessage echo(addr);
    echo.add(_model->channel(n).fader);
    reply.add(echo);
}

void OscDispatcher::handleChannelOn(int n, OSCMessage &msg, OSCBundle &reply) {
    if (!msg.isInt(0)) return;
    bool on = (msg.getInt(0) != 0);
    _model->setChannelOn(n, on);
    if (_binding) {
        _binding->applyChannel(n);
        int partner = ((n & 1) == 1 && n < kChannelCount &&
                       _model->channel(n).link) ? n + 1 : 0;
        if (partner == 0 && (n & 1) == 0 && n > 1 &&
            _model->channel(n - 1).link) partner = n - 1;
        if (partner) _binding->applyChannel(partner);
    }
    char addr[32];
    snprintf(addr, sizeof(addr), "/ch/%02d/mix/on", n);
    OSCMessage echo(addr);
    echo.add((int32_t)(_model->channel(n).on ? 1 : 0));
    reply.add(echo);
}

void OscDispatcher::handleChannelSolo(int n, OSCMessage &msg, OSCBundle &reply) {
    if (!msg.isInt(0)) return;
    bool solo = (msg.getInt(0) != 0);
    _model->setChannelSolo(n, solo);
    // Solo changes affect *every* channel's effective gain via SIP,
    // so refresh all gains rather than just this channel.
    if (_binding) _binding->applyAllChannelGains();
    char addr[32];
    snprintf(addr, sizeof(addr), "/ch/%02d/mix/solo", n);
    OSCMessage echo(addr);
    echo.add((int32_t)(_model->channel(n).solo ? 1 : 0));
    reply.add(echo);
}

void OscDispatcher::handleChannelName(int n, OSCMessage &msg, OSCBundle &reply) {
    if (!msg.isString(0)) return;
    char name[kChannelNameMax];
    int len = msg.getString(0, name, sizeof(name));
    name[(len < (int)sizeof(name)) ? len : (int)sizeof(name) - 1] = '\0';
    _model->setChannelName(n, name);
    char addr[32];
    snprintf(addr, sizeof(addr), "/ch/%02d/config/name", n);
    OSCMessage echo(addr);
    echo.add(_model->channel(n).name);
    reply.add(echo);
}

void OscDispatcher::handleChannelLink(int n, OSCMessage &msg, OSCBundle &reply) {
    if (!msg.isInt(0)) return;
    bool linked = (msg.getInt(0) != 0);
    _model->setChannelLink(n, linked);
    char addr[32];
    snprintf(addr, sizeof(addr), "/ch/%02d/config/link", n);
    OSCMessage echo(addr);
    echo.add((int32_t)(_model->channel(n).link ? 1 : 0));
    reply.add(echo);
}

void OscDispatcher::handleChannelHpfOn(int n, OSCMessage &msg, OSCBundle &reply) {
    if (!msg.isInt(0)) return;
    bool on = (msg.getInt(0) != 0);
    _model->setChannelHpfOn(n, on);
    if (_binding) _binding->applyChannelHpf(n);
    char addr[40];
    snprintf(addr, sizeof(addr), "/ch/%02d/preamp/hpf/on", n);
    OSCMessage echo(addr);
    echo.add((int32_t)(_model->channel(n).hpfOn ? 1 : 0));
    reply.add(echo);
}

void OscDispatcher::handleChannelHpfFreq(int n, OSCMessage &msg, OSCBundle &reply) {
    if (!msg.isFloat(0)) return;
    float hz = msg.getFloat(0);
    _model->setChannelHpfFreq(n, hz);
    if (_binding) _binding->applyChannelHpf(n);
    char addr[40];
    snprintf(addr, sizeof(addr), "/ch/%02d/preamp/hpf/f", n);
    OSCMessage echo(addr);
    echo.add(_model->channel(n).hpfFreqHz);
    reply.add(echo);
}

void OscDispatcher::handleMainFader(OSCMessage &msg, OSCBundle &reply) {
    if (!msg.isFloat(0)) return;
    float v = msg.getFloat(0);
    _model->setMainFader(v);
    if (_binding) _binding->applyMain();
    OSCMessage echo("/main/st/mix/fader");
    echo.add(_model->main().fader);
    reply.add(echo);
}

void OscDispatcher::handleMainOn(OSCMessage &msg, OSCBundle &reply) {
    if (!msg.isInt(0)) return;
    bool on = (msg.getInt(0) != 0);
    _model->setMainOn(on);
    if (_binding) _binding->applyMain();
    OSCMessage echo("/main/st/mix/on");
    echo.add((int32_t)(_model->main().on ? 1 : 0));
    reply.add(echo);
}

void OscDispatcher::handleMainHostvolEnable(OSCMessage &msg, OSCBundle &reply) {
    if (!msg.isInt(0)) return;
    bool enable = (msg.getInt(0) != 0);
    _model->setMainHostvolEnable(enable);
    if (_binding) _binding->applyMain();
    OSCMessage echo("/main/st/hostvol/enable");
    echo.add((int32_t)(_model->main().hostvolEnable ? 1 : 0));
    reply.add(echo);
}

void OscDispatcher::handleSub(OSCMessage &msg, OSCBundle &reply) {
    // Pattern the web_dev_surface client sends (see dispatcher.ts):
    //   /sub sis "addSub"      interval_ms   "/meters/input"
    //   /sub ss  "unsubscribe"               "/meters/input"
    //
    // For MVP we only care about meter subscriptions — one global enable
    // bit on MeterEngine. First string arg is the verb; the last string
    // arg is the target address pattern. Anything that isn't addSub or
    // unsubscribe is silently dropped.
    (void)reply;
    if (msg.size() < 1 || !msg.isString(0)) return;

    char verb[16];
    int verbLen = msg.getString(0, verb, sizeof(verb));
    verb[(verbLen < (int)sizeof(verb)) ? verbLen : (int)sizeof(verb) - 1] = '\0';

    // Find the last string arg (the target pattern). For addSub it's at
    // index 2 (after the int interval); for unsubscribe it's at index 1.
    const int lastArg = msg.size() - 1;
    if (lastArg < 1 || !msg.isString(lastArg)) return;

    char target[64];
    int targetLen = msg.getString(lastArg, target, sizeof(target));
    target[(targetLen < (int)sizeof(target)) ? targetLen : (int)sizeof(target) - 1] = '\0';

    const bool isMeterTarget = (strncmp(target, "/meters/", 8) == 0);

    if (isMeterTarget && _meterEngine) {
        if (strcmp(verb, "addSub") == 0) {
            _meterEngine->setEnabled(true);
        } else if (strcmp(verb, "unsubscribe") == 0) {
            _meterEngine->setEnabled(false);
        }
    }
}

void OscDispatcher::handleInfo(OSCMessage &msg, OSCBundle &reply) {
    (void)msg;
    OSCMessage info("/info");
    info.add("t-dsp small mixer");
    info.add((int32_t)kChannelCount);
    info.add("mvp_v1");
    reply.add(info);
}

// ----- Broadcast helpers (non-OSC-originated state changes) -----

void OscDispatcher::broadcastMainFader(OSCBundle &reply) {
    if (!_model) return;
    OSCMessage echo("/main/st/mix/fader");
    echo.add(_model->main().fader);
    reply.add(echo);
}

void OscDispatcher::broadcastMainOn(OSCBundle &reply) {
    if (!_model) return;
    OSCMessage echo("/main/st/mix/on");
    echo.add((int32_t)(_model->main().on ? 1 : 0));
    reply.add(echo);
}

void OscDispatcher::broadcastMainHostvolValue(OSCBundle &reply) {
    if (!_model) return;
    OSCMessage echo("/main/st/hostvol/value");
    echo.add(_model->main().hostvolValue);
    reply.add(echo);
}

void OscDispatcher::broadcastChannelFader(int n, OSCBundle &reply) {
    if (!_model || n < 1 || n > kChannelCount) return;
    char addr[32];
    snprintf(addr, sizeof(addr), "/ch/%02d/mix/fader", n);
    OSCMessage echo(addr);
    echo.add(_model->channel(n).fader);
    reply.add(echo);
}

void OscDispatcher::broadcastChannelOn(int n, OSCBundle &reply) {
    if (!_model || n < 1 || n > kChannelCount) return;
    char addr[32];
    snprintf(addr, sizeof(addr), "/ch/%02d/mix/on", n);
    OSCMessage echo(addr);
    echo.add((int32_t)(_model->channel(n).on ? 1 : 0));
    reply.add(echo);
}

void OscDispatcher::broadcastChannelSolo(int n, OSCBundle &reply) {
    if (!_model || n < 1 || n > kChannelCount) return;
    char addr[32];
    snprintf(addr, sizeof(addr), "/ch/%02d/mix/solo", n);
    OSCMessage echo(addr);
    echo.add((int32_t)(_model->channel(n).solo ? 1 : 0));
    reply.add(echo);
}

void OscDispatcher::broadcastChannelName(int n, OSCBundle &reply) {
    if (!_model || n < 1 || n > kChannelCount) return;
    char addr[32];
    snprintf(addr, sizeof(addr), "/ch/%02d/config/name", n);
    OSCMessage echo(addr);
    echo.add(_model->channel(n).name);
    reply.add(echo);
}

// Pack peak/RMS float pairs into a big-endian float32 blob and add it
// to the reply bundle under `address`. Shared by broadcastMetersInput
// and broadcastMetersOutput so the packing logic lives in one place.
static void packMeterBlob(OSCBundle &reply, const char *address,
                          const float *peakRmsPairs, int pairCount) {
    if (!peakRmsPairs || pairCount <= 0) return;
    // Pack floats as big-endian float32 (X32 convention). CNMAT/OSC
    // handles endianness for float args but NOT for blob payloads — we
    // have to byte-swap manually.
    uint8_t blob[64];  // 8 pairs * 8 bytes each = 64 bytes max
    int byteCount = pairCount * 8;
    if (byteCount > (int)sizeof(blob)) byteCount = sizeof(blob);
    for (int i = 0; i < pairCount && i * 8 + 8 <= (int)sizeof(blob); ++i) {
        uint32_t peakBits, rmsBits;
        memcpy(&peakBits, &peakRmsPairs[i * 2 + 0], 4);
        memcpy(&rmsBits, &peakRmsPairs[i * 2 + 1], 4);
        blob[i * 8 + 0] = (uint8_t)(peakBits >> 24);
        blob[i * 8 + 1] = (uint8_t)(peakBits >> 16);
        blob[i * 8 + 2] = (uint8_t)(peakBits >> 8);
        blob[i * 8 + 3] = (uint8_t)(peakBits);
        blob[i * 8 + 4] = (uint8_t)(rmsBits >> 24);
        blob[i * 8 + 5] = (uint8_t)(rmsBits >> 16);
        blob[i * 8 + 6] = (uint8_t)(rmsBits >> 8);
        blob[i * 8 + 7] = (uint8_t)(rmsBits);
    }
    OSCMessage meters(address);
    meters.add(blob, byteCount);
    reply.add(meters);
}

void OscDispatcher::broadcastMetersInput(OSCBundle &reply,
                                         const float *peakRmsPairs,
                                         int pairCount) {
    packMeterBlob(reply, "/meters/input", peakRmsPairs, pairCount);
}

void OscDispatcher::broadcastMetersOutput(OSCBundle &reply,
                                          const float *peakRmsPairs,
                                          int pairCount) {
    packMeterBlob(reply, "/meters/output", peakRmsPairs, pairCount);
}

}  // namespace tdsp
