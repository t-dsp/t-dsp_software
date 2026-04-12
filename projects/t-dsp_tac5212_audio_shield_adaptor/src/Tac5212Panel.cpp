#include "Tac5212Panel.h"

#include <Arduino.h>
#include <OSCMessage.h>
#include <OSCBundle.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// ----- Small helpers -----

// Pull an integer arg defensively: accepts both i (int32) and f (float)
// since some clients send ints as floats by mistake.
static int32_t argAsInt(OSCMessage &msg, int i) {
    if (msg.isInt(i)) return msg.getInt(i);
    if (msg.isFloat(i)) return (int32_t)msg.getFloat(i);
    return 0;
}

// Pull a string arg into a stack buffer. Returns the length.
static int argAsString(OSCMessage &msg, int i, char *buf, int bufSize) {
    if (!msg.isString(i) || !buf || bufSize <= 0) return 0;
    int len = msg.getString(i, buf, bufSize);
    if (len >= bufSize) len = bufSize - 1;
    buf[len] = '\0';
    return len;
}

// Check whether the current msg looks like a "read" (no args) as opposed
// to a "write" (one or more args). For our panel, leaves like /info /status
// /reset /wake with-no-args are reads; /wake with an int arg is a write.
static bool msgIsRead(OSCMessage &msg) {
    return msg.size() == 0;
}

static void echoEnumReply(OSCBundle &reply, const char *addr, const char *value) {
    OSCMessage m(addr);
    m.add(value);
    reply.add(m);
}

static void echoIntReply(OSCBundle &reply, const char *addr, int32_t value) {
    OSCMessage m(addr);
    m.add(value);
    reply.add(m);
}

__attribute__((unused))
static void echoFloatReply(OSCBundle &reply, const char *addr, float value) {
    OSCMessage m(addr);
    m.add(value);
    reply.add(m);
}

static void echoErrorReply(OSCBundle &reply, const char *addr, const char *msg) {
    // Conventional format: /error s "<original-addr> <reason>".
    OSCMessage m("/error");
    char buf[160];
    snprintf(buf, sizeof(buf), "%s %s", addr, msg ? msg : "(unknown)");
    m.add(buf);
    reply.add(m);
}

// ----- Tac5212Panel::route -----

void Tac5212Panel::route(OSCMessage &msg, int addrOffset, OSCBundle &reply) {
    if (msg.hasError()) return;

    char fullAddress[128];
    int fullLen = msg.getAddress(fullAddress, 0, sizeof(fullAddress) - 1);
    if (fullLen < 0) fullLen = 0;
    fullAddress[fullLen] = '\0';
    const char *sub = fullAddress + addrOffset;

    // Parse /adc/N/<leaf>
    if (strncmp(sub, "/adc/", 5) == 0) {
        const char *p = sub + 5;
        if (*p >= '0' && *p <= '9') {
            int n = 0;
            while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); ++p; }
            if (*p == '/') {
                const char *leaf = p;
                if (strcmp(leaf, "/mode")      == 0) { handleAdcMode(n, msg, reply); return; }
                if (strcmp(leaf, "/impedance") == 0) { handleAdcImpedance(n, msg, reply); return; }
                if (strcmp(leaf, "/fullscale") == 0) { handleAdcFullscale(n, msg, reply); return; }
                if (strcmp(leaf, "/coupling")  == 0) { handleAdcCoupling(n, msg, reply); return; }
                if (strcmp(leaf, "/bw")        == 0) { handleAdcBw(n, msg, reply); return; }
            }
        } else if (strcmp(sub, "/adc/hpf") == 0) {
            handleAdcHpf(msg, reply); return;
        }
    }

    if (strcmp(sub, "/vref/fscale") == 0) { handleVrefFscale(msg, reply); return; }

    if (strcmp(sub, "/micbias/enable") == 0) { handleMicbiasEnable(msg, reply); return; }
    if (strcmp(sub, "/micbias/level")  == 0) { handleMicbiasLevel(msg, reply); return; }

    // /out/N/mode
    if (strncmp(sub, "/out/", 5) == 0) {
        const char *p = sub + 5;
        if (*p >= '0' && *p <= '9') {
            int n = 0;
            while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); ++p; }
            if (strcmp(p, "/mode") == 0) { handleOutMode(n, msg, reply); return; }
        }
    }

    if (strcmp(sub, "/pdm/enable") == 0) { handlePdmEnable(msg, reply); return; }
    if (strcmp(sub, "/pdm/source") == 0) { handlePdmSource(msg, reply); return; }

    if (strcmp(sub, "/reset")  == 0) { handleReset(msg, reply); return; }
    if (strcmp(sub, "/wake")   == 0) { handleWake(msg, reply); return; }
    if (strcmp(sub, "/info")   == 0) { handleInfo(msg, reply); return; }
    if (strcmp(sub, "/status") == 0) { handleStatus(msg, reply); return; }

    if (strcmp(sub, "/reg/set") == 0) { handleRegSet(msg, reply); return; }
    if (strcmp(sub, "/reg/get") == 0) { handleRegGet(msg, reply); return; }

    // Unknown subpath — silent drop. The serial console pane in the web
    // client will show it in the raw OSC log.
}

// ----- ADC handlers -----

void Tac5212Panel::handleAdcMode(int n, OSCMessage &msg, OSCBundle &reply) {
    char val[32];
    if (argAsString(msg, 0, val, sizeof(val)) == 0) return;
    tac5212::AdcMode mode;
    if      (strcmp(val, "single_ended_inp") == 0) mode = tac5212::AdcMode::SingleEndedInp;
    else if (strcmp(val, "single_ended_inm") == 0) mode = tac5212::AdcMode::SingleEndedInm;
    else if (strcmp(val, "differential")      == 0) mode = tac5212::AdcMode::Differential;
    else {
        char addr[48];
        snprintf(addr, sizeof(addr), "/codec/tac5212/adc/%d/mode", n);
        echoErrorReply(reply, addr, "unknown mode");
        return;
    }
    tac5212::Result r = _codec.adc(n).setMode(mode);
    char addr[48];
    snprintf(addr, sizeof(addr), "/codec/tac5212/adc/%d/mode", n);
    if (r.isError()) {
        echoErrorReply(reply, addr, r.message ? r.message : "setMode failed");
    } else {
        echoEnumReply(reply, addr, val);
    }
}

void Tac5212Panel::handleAdcImpedance(int n, OSCMessage &msg, OSCBundle &reply) {
    char val[16];
    if (argAsString(msg, 0, val, sizeof(val)) == 0) return;
    tac5212::AdcImpedance imp;
    if      (strcmp(val, "5k")  == 0) imp = tac5212::AdcImpedance::K5;
    else if (strcmp(val, "10k") == 0) imp = tac5212::AdcImpedance::K10;
    else if (strcmp(val, "40k") == 0) imp = tac5212::AdcImpedance::K40;
    else {
        char addr[56];
        snprintf(addr, sizeof(addr), "/codec/tac5212/adc/%d/impedance", n);
        echoErrorReply(reply, addr, "unknown impedance");
        return;
    }
    tac5212::Result r = _codec.adc(n).setImpedance(imp);
    char addr[56];
    snprintf(addr, sizeof(addr), "/codec/tac5212/adc/%d/impedance", n);
    if (r.isError()) echoErrorReply(reply, addr, r.message ? r.message : "setImpedance failed");
    else             echoEnumReply(reply, addr, val);
}

void Tac5212Panel::handleAdcFullscale(int n, OSCMessage &msg, OSCBundle &reply) {
    char val[16];
    if (argAsString(msg, 0, val, sizeof(val)) == 0) return;
    tac5212::AdcFullscale fs;
    if      (strcmp(val, "2vrms") == 0) fs = tac5212::AdcFullscale::V2rms;
    else if (strcmp(val, "4vrms") == 0) fs = tac5212::AdcFullscale::V4rms;
    else {
        char addr[56];
        snprintf(addr, sizeof(addr), "/codec/tac5212/adc/%d/fullscale", n);
        echoErrorReply(reply, addr, "unknown fullscale");
        return;
    }
    tac5212::Result r = _codec.adc(n).setFullscale(fs);
    char addr[56];
    snprintf(addr, sizeof(addr), "/codec/tac5212/adc/%d/fullscale", n);
    if (r.isError()) echoErrorReply(reply, addr, r.message ? r.message : "setFullscale failed");
    else             echoEnumReply(reply, addr, val);
}

void Tac5212Panel::handleAdcCoupling(int n, OSCMessage &msg, OSCBundle &reply) {
    char val[24];
    if (argAsString(msg, 0, val, sizeof(val)) == 0) return;
    tac5212::AdcCoupling c;
    if      (strcmp(val, "ac")               == 0) c = tac5212::AdcCoupling::Ac;
    else if (strcmp(val, "dc_low")           == 0) c = tac5212::AdcCoupling::DcLow;
    else if (strcmp(val, "dc_rail_to_rail")  == 0) c = tac5212::AdcCoupling::DcRailToRail;
    else {
        char addr[56];
        snprintf(addr, sizeof(addr), "/codec/tac5212/adc/%d/coupling", n);
        echoErrorReply(reply, addr, "unknown coupling");
        return;
    }
    tac5212::Result r = _codec.adc(n).setCoupling(c);
    char addr[56];
    snprintf(addr, sizeof(addr), "/codec/tac5212/adc/%d/coupling", n);
    if (r.isError()) echoErrorReply(reply, addr, r.message ? r.message : "setCoupling failed");
    else             echoEnumReply(reply, addr, val);
}

void Tac5212Panel::handleAdcBw(int n, OSCMessage &msg, OSCBundle &reply) {
    char val[16];
    if (argAsString(msg, 0, val, sizeof(val)) == 0) return;
    tac5212::AdcBw bw;
    if      (strcmp(val, "24khz") == 0) bw = tac5212::AdcBw::Audio24k;
    else if (strcmp(val, "96khz") == 0) bw = tac5212::AdcBw::Wide96k;
    else {
        char addr[56];
        snprintf(addr, sizeof(addr), "/codec/tac5212/adc/%d/bw", n);
        echoErrorReply(reply, addr, "unknown bw");
        return;
    }
    tac5212::Result r = _codec.adc(n).setBw(bw);
    char addr[56];
    snprintf(addr, sizeof(addr), "/codec/tac5212/adc/%d/bw", n);
    if (r.isError()) echoErrorReply(reply, addr, r.message ? r.message : "setBw failed");
    else             echoEnumReply(reply, addr, val);
}

void Tac5212Panel::handleAdcHpf(OSCMessage &msg, OSCBundle &reply) {
    int32_t v = argAsInt(msg, 0);
    tac5212::Result r = _codec.setAdcHpf(v != 0);
    const char *addr = "/codec/tac5212/adc/hpf";
    if (r.isError()) echoErrorReply(reply, addr, r.message ? r.message : "setAdcHpf failed");
    else             echoIntReply(reply, addr, v != 0 ? 1 : 0);
}

// ----- VREF -----

void Tac5212Panel::handleVrefFscale(OSCMessage &msg, OSCBundle &reply) {
    char val[16];
    if (argAsString(msg, 0, val, sizeof(val)) == 0) return;
    tac5212::VrefFscale scale;
    if      (strcmp(val, "2.75v")  == 0) scale = tac5212::VrefFscale::V2p75;
    else if (strcmp(val, "2.5v")   == 0) scale = tac5212::VrefFscale::V2p5;
    else if (strcmp(val, "1.375v") == 0) scale = tac5212::VrefFscale::V1p375;
    else {
        echoErrorReply(reply, "/codec/tac5212/vref/fscale", "unknown scale");
        return;
    }
    tac5212::Result r = _codec.setVrefFscale(scale);
    if (r.isError()) echoErrorReply(reply, "/codec/tac5212/vref/fscale", r.message ? r.message : "setVrefFscale failed");
    else             echoEnumReply(reply, "/codec/tac5212/vref/fscale", val);
}

// ----- MICBIAS -----

void Tac5212Panel::handleMicbiasEnable(OSCMessage &msg, OSCBundle &reply) {
    int32_t v = argAsInt(msg, 0);
    tac5212::Result r = _codec.setMicbiasEnable(v != 0);
    if (r.isError()) {
        echoErrorReply(reply, "/codec/tac5212/micbias/enable", r.message ? r.message : "setMicbiasEnable failed");
    } else {
        echoIntReply(reply, "/codec/tac5212/micbias/enable", v != 0 ? 1 : 0);
        if (r.isWarning() && r.message) {
            // Separate warning message so the client can surface it.
            OSCMessage warn("/warning");
            warn.add(r.message);
            reply.add(warn);
        }
    }
}

void Tac5212Panel::handleMicbiasLevel(OSCMessage &msg, OSCBundle &reply) {
    // The OSC tree uses absolute voltage strings, but the library uses
    // relative-to-VREF. Translate by reading current VREF implicitly: for
    // simplicity, pick the first matching combo. Reserved combos are
    // caught by library validation.
    char val[16];
    if (argAsString(msg, 0, val, sizeof(val)) == 0) return;
    // Simple mapping: if VREF is 2.75v, then:
    //   SameAsVref → 2.75v
    //   HalfVref   → 1.375v
    //   Avdd       → avdd
    // If VREF is 2.5v:
    //   SameAsVref → 2.5v
    //   HalfVref   → 1.25v
    //   Avdd       → avdd
    // We don't have a getter for VREF here, so we optimistically assume
    // 2.75v (the default) and let the library reject reserved combos.
    // For a full solution we'd read the register; MVP accepts this
    // simplification.
    tac5212::MicbiasLevel level;
    if      (strcmp(val, "2.75v")  == 0) level = tac5212::MicbiasLevel::SameAsVref;
    else if (strcmp(val, "2.5v")   == 0) level = tac5212::MicbiasLevel::SameAsVref;  // if VREF=2.5v
    else if (strcmp(val, "1.375v") == 0) level = tac5212::MicbiasLevel::HalfVref;    // if VREF=2.75v
    else if (strcmp(val, "1.25v")  == 0) level = tac5212::MicbiasLevel::HalfVref;    // if VREF=2.5v
    else if (strcmp(val, "avdd")   == 0) level = tac5212::MicbiasLevel::Avdd;
    else {
        echoErrorReply(reply, "/codec/tac5212/micbias/level", "unknown level");
        return;
    }
    tac5212::Result r = _codec.setMicbiasLevel(level);
    if (r.isError()) echoErrorReply(reply, "/codec/tac5212/micbias/level", r.message ? r.message : "setMicbiasLevel failed");
    else             echoEnumReply(reply, "/codec/tac5212/micbias/level", val);
}

// ----- Output mode -----

void Tac5212Panel::handleOutMode(int n, OSCMessage &msg, OSCBundle &reply) {
    char val[16];
    if (argAsString(msg, 0, val, sizeof(val)) == 0) return;
    tac5212::OutMode mode;
    if      (strcmp(val, "diff_line") == 0) mode = tac5212::OutMode::DiffLine;
    else if (strcmp(val, "se_line")   == 0) mode = tac5212::OutMode::SeLine;
    else if (strcmp(val, "hp_driver") == 0) mode = tac5212::OutMode::HpDriver;
    else if (strcmp(val, "receiver")  == 0) mode = tac5212::OutMode::FdReceiver;
    else {
        char addr[48];
        snprintf(addr, sizeof(addr), "/codec/tac5212/out/%d/mode", n);
        echoErrorReply(reply, addr, "unknown out mode");
        return;
    }
    tac5212::Result r = _codec.out(n).setMode(mode);
    char addr[48];
    snprintf(addr, sizeof(addr), "/codec/tac5212/out/%d/mode", n);
    if (r.isError()) echoErrorReply(reply, addr, r.message ? r.message : "setMode failed");
    else             echoEnumReply(reply, addr, val);
}

// ----- PDM (stubbed at library level) -----

void Tac5212Panel::handlePdmEnable(OSCMessage &msg, OSCBundle &reply) {
    int32_t v = argAsInt(msg, 0);
    tac5212::Result r = _codec.pdm().setEnable(v != 0);
    if (r.isError()) {
        echoErrorReply(reply, "/codec/tac5212/pdm/enable", r.message ? r.message : "pdm setEnable failed");
    } else {
        echoIntReply(reply, "/codec/tac5212/pdm/enable", v != 0 ? 1 : 0);
    }
}

void Tac5212Panel::handlePdmSource(OSCMessage &msg, OSCBundle &reply) {
    char val[8];
    if (argAsString(msg, 0, val, sizeof(val)) == 0) return;
    tac5212::PdmSource src;
    if      (strcmp(val, "gpi1") == 0) src = tac5212::PdmSource::Gpi1;
    else if (strcmp(val, "gpi2") == 0) src = tac5212::PdmSource::Gpi2;
    else {
        echoErrorReply(reply, "/codec/tac5212/pdm/source", "unknown pdm source");
        return;
    }
    tac5212::Result r = _codec.pdm().setSource(src);
    if (r.isError()) echoErrorReply(reply, "/codec/tac5212/pdm/source", r.message ? r.message : "pdm setSource failed");
    else             echoEnumReply(reply, "/codec/tac5212/pdm/source", val);
}

// ----- System / global -----

void Tac5212Panel::handleReset(OSCMessage &msg, OSCBundle &reply) {
    (void)msg;
    tac5212::Result r = _codec.reset();
    OSCMessage m("/codec/tac5212/reset");
    m.add((int32_t)(r.isOk() ? 1 : 0));
    if (r.message) m.add(r.message);
    reply.add(m);
}

void Tac5212Panel::handleWake(OSCMessage &msg, OSCBundle &reply) {
    // /wake with no args is a read (return current wake state — but we
    // don't cache it, so respond with "1" optimistically). /wake with
    // int arg is a write.
    if (msgIsRead(msg)) {
        echoIntReply(reply, "/codec/tac5212/wake", 1);
        return;
    }
    int32_t v = argAsInt(msg, 0);
    tac5212::Result r = _codec.wake(v != 0);
    if (r.isError()) echoErrorReply(reply, "/codec/tac5212/wake", r.message ? r.message : "wake failed");
    else             echoIntReply(reply, "/codec/tac5212/wake", v != 0 ? 1 : 0);
}

void Tac5212Panel::handleInfo(OSCMessage &msg, OSCBundle &reply) {
    (void)msg;
    tac5212::DeviceInfo info = _codec.info();
    OSCMessage m("/codec/tac5212/info");
    m.add(info.model);
    m.add((int32_t)info.i2cAddr);
    m.add((int32_t)info.pageInUse);
    reply.add(m);
}

void Tac5212Panel::handleStatus(OSCMessage &msg, OSCBundle &reply) {
    (void)msg;
    tac5212::Status status = _codec.readStatus();
    OSCMessage m("/codec/tac5212/status");
    m.add((int32_t)status.devSts0);
    m.add((int32_t)status.devSts1);
    m.add((int32_t)(status.pllLocked ? 1 : 0));
    m.add((int32_t)(status.micBiasActive ? 1 : 0));
    m.add((int32_t)(status.faultActive ? 1 : 0));
    m.add((int32_t)(status.micBiasGpioOverride ? 1 : 0));
    reply.add(m);
}

// ----- Raw register access -----

void Tac5212Panel::handleRegSet(OSCMessage &msg, OSCBundle &reply) {
    // /reg/set ii reg value — page 0 assumed for MVP.
    int32_t reg   = argAsInt(msg, 0);
    int32_t value = argAsInt(msg, 1);
    tac5212::Result r = _codec.writeRegister(0, (uint8_t)reg, (uint8_t)value);
    if (r.isError()) {
        echoErrorReply(reply, "/codec/tac5212/reg/set", r.message ? r.message : "regSet failed");
    } else {
        OSCMessage m("/codec/tac5212/reg");
        m.add(reg);
        m.add(value);
        reply.add(m);
    }
}

void Tac5212Panel::handleRegGet(OSCMessage &msg, OSCBundle &reply) {
    int32_t reg = argAsInt(msg, 0);
    uint8_t value = _codec.readRegister(0, (uint8_t)reg);
    OSCMessage m("/codec/tac5212/reg");
    m.add(reg);
    m.add((int32_t)value);
    reply.add(m);
}

// ----- /snapshot contribution -----
//
// Reads back the current chip state and emits the same enum-string echoes
// a write would produce. The dev surface client routes /codec/tac5212/...
// echoes into its codec-panel UI signals, so a fresh client renders the
// real chip state instead of whatever defaults its <select> elements were
// initialized with. Currently covers the Output tab; other tabs join as
// lib-side getters become available.
//
// Quietly skips leaves whose getter returns Error (chip in an unexpected
// register combo, raw /reg/set tampering, etc.) — the client just keeps
// its placeholder until the next user action triggers an echo.

static const char *outModeToString(tac5212::OutMode m) {
    switch (m) {
        case tac5212::OutMode::DiffLine:   return "diff_line";
        case tac5212::OutMode::SeLine:     return "se_line";
        case tac5212::OutMode::HpDriver:   return "hp_driver";
        case tac5212::OutMode::FdReceiver: return "receiver";
    }
    return "diff_line";
}

void Tac5212Panel::snapshot(OSCBundle &reply) {
    for (int n = 1; n <= 2; ++n) {
        tac5212::OutMode mode;
        tac5212::Result r = _codec.out(n).getMode(mode);
        if (r.isError()) continue;
        char addr[48];
        snprintf(addr, sizeof(addr), "/codec/tac5212/out/%d/mode", n);
        echoEnumReply(reply, addr, outModeToString(mode));
    }
}
