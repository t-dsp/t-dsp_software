#include "Adc6140Panel.h"

#include <Arduino.h>
#include <OSCMessage.h>
#include <OSCBundle.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

using tlv320adc6140::Result;
using tlv320adc6140::InputType;
using tlv320adc6140::InputSource;
using tlv320adc6140::Coupling;
using tlv320adc6140::Impedance;
using tlv320adc6140::MicBias;
using tlv320adc6140::FullScale;
using tlv320adc6140::HpfCutoff;
using tlv320adc6140::DecimationFilter;
using tlv320adc6140::ChannelSumMode;
using tlv320adc6140::DreAgcMode;

// ----- Small helpers (same shape as Tac5212Panel) -------------------------

static int32_t argAsInt(OSCMessage &msg, int i) {
    if (msg.isInt(i))   return msg.getInt(i);
    if (msg.isFloat(i)) return (int32_t)msg.getFloat(i);
    return 0;
}

static float argAsFloat(OSCMessage &msg, int i) {
    if (msg.isFloat(i)) return msg.getFloat(i);
    if (msg.isInt(i))   return (float)msg.getInt(i);
    return 0.0f;
}

static int argAsString(OSCMessage &msg, int i, char *buf, int bufSize) {
    if (!msg.isString(i) || !buf || bufSize <= 0) return 0;
    int len = msg.getString(i, buf, bufSize);
    if (len >= bufSize) len = bufSize - 1;
    buf[len] = '\0';
    return len;
}

static void echoEnum  (OSCBundle &r, const char *a, const char *v) { OSCMessage m(a); m.add(v); r.add(m); }
static void echoInt   (OSCBundle &r, const char *a, int32_t   v)   { OSCMessage m(a); m.add(v); r.add(m); }
static void echoFloat (OSCBundle &r, const char *a, float     v)   { OSCMessage m(a); m.add(v); r.add(m); }

static void echoError(OSCBundle &reply, const char *addr, const char *msg) {
    OSCMessage m("/error");
    char buf[160];
    snprintf(buf, sizeof(buf), "%s %s", addr, msg ? msg : "(unknown)");
    m.add(buf);
    reply.add(m);
}

// Report Result back to caller as a /error message if non-Ok.
static void reportResult(OSCBundle &reply, const char *addr, const Result &r) {
    if (r.isOk()) return;
    echoError(reply, addr, r.message ? r.message : "error");
}

// ----- route() ------------------------------------------------------------

void Adc6140Panel::route(OSCMessage &msg, int addrOffset, OSCBundle &reply) {
    if (msg.hasError()) return;

    char fullAddress[128];
    int fullLen = msg.getAddress(fullAddress, 0, sizeof(fullAddress) - 1);
    if (fullLen < 0) fullLen = 0;
    fullAddress[fullLen] = '\0';
    const char *sub = fullAddress + addrOffset;

    // Per-channel: /ch/N/<leaf>
    if (strncmp(sub, "/ch/", 4) == 0) {
        const char *p = sub + 4;
        if (*p < '1' || *p > '4') { echoError(reply, fullAddress, "channel must be 1..4"); return; }
        int n = *p - '0';
        p++;
        if (*p != '/') { echoError(reply, fullAddress, "malformed path"); return; }
        p++;

        if (strcmp(p, "gain")      == 0) { handleChannelGain     (n, msg, reply); return; }
        if (strcmp(p, "dvol")      == 0) { handleChannelDvol     (n, msg, reply); return; }
        if (strcmp(p, "impedance") == 0) { handleChannelImpedance(n, msg, reply); return; }
        if (strcmp(p, "coupling")  == 0) { handleChannelCoupling (n, msg, reply); return; }
        if (strcmp(p, "intype")    == 0) { handleChannelInType   (n, msg, reply); return; }
        if (strcmp(p, "source")    == 0) { handleChannelSource   (n, msg, reply); return; }
        if (strcmp(p, "dre")       == 0) { handleChannelDre      (n, msg, reply); return; }
        if (strcmp(p, "gaincal")   == 0) { handleChannelGainCal  (n, msg, reply); return; }

        echoError(reply, fullAddress, "unknown channel leaf");
        return;
    }

    if (strcmp(sub, "/micbias")       == 0) { handleMicbias    (msg, reply); return; }
    if (strcmp(sub, "/fullscale")     == 0) { handleFullscale  (msg, reply); return; }
    if (strcmp(sub, "/hpf")           == 0) { handleHpf        (msg, reply); return; }
    if (strcmp(sub, "/decimfilt")     == 0) { handleDecimFilt  (msg, reply); return; }
    if (strcmp(sub, "/chsum")         == 0) { handleChSum      (msg, reply); return; }
    if (strcmp(sub, "/mode")          == 0) { handleMode       (msg, reply); return; }
    if (strcmp(sub, "/dre/level")     == 0) { handleDreLevel   (msg, reply); return; }
    if (strcmp(sub, "/dre/maxgain")   == 0) { handleDreMaxGain (msg, reply); return; }
    if (strcmp(sub, "/agc/target")    == 0) { handleAgcTarget  (msg, reply); return; }
    if (strcmp(sub, "/agc/maxgain")   == 0) { handleAgcMaxGain (msg, reply); return; }
    if (strcmp(sub, "/info")          == 0) { handleInfo       (msg, reply); return; }
    if (strcmp(sub, "/status")        == 0) { handleStatus     (msg, reply); return; }
    if (strcmp(sub, "/reg/set")       == 0) { handleRegSet     (msg, reply); return; }
    if (strcmp(sub, "/reg/get")       == 0) { handleRegGet     (msg, reply); return; }

    echoError(reply, fullAddress, "unknown leaf");
}

// ----- snapshot() ---------------------------------------------------------

void Adc6140Panel::snapshot(OSCBundle &reply) {
    // Read back everything we can from the chip so a freshly-connected
    // client's UI reflects actual state. Keep this minimal for now — the
    // client should already echo its writes, so the snapshot is mostly
    // useful for the first paint.
    tlv320adc6140::Status s = _codec.readStatus();
    char addr[64];
    snprintf(addr, sizeof(addr), "/codec/adc6140/status");
    OSCMessage m(addr);
    m.add((int32_t)s.devSts0);
    m.add((int32_t)s.devSts1);
    m.add((int32_t)s.asiSts);
    reply.add(m);
}

// ----- per-channel handlers -----------------------------------------------

void Adc6140Panel::handleChannelGain(int n, OSCMessage &msg, OSCBundle &reply) {
    char addr[64]; snprintf(addr, sizeof(addr), "/codec/adc6140/ch/%d/gain", n);
    if (msg.size() == 0) { echoFloat(reply, addr, 0.0f); return; }
    int db = (int)argAsFloat(msg, 0);
    if (db < 0) db = 0; if (db > 42) db = 42;
    Result r = _codec.channel(n).setGainDb((uint8_t)db);
    reportResult(reply, addr, r);
    echoFloat(reply, addr, (float)db);
}

void Adc6140Panel::handleChannelDvol(int n, OSCMessage &msg, OSCBundle &reply) {
    char addr[64]; snprintf(addr, sizeof(addr), "/codec/adc6140/ch/%d/dvol", n);
    if (msg.size() == 0) { echoFloat(reply, addr, 0.0f); return; }
    float db = argAsFloat(msg, 0);
    Result r = _codec.channel(n).setDvolDb(db);
    reportResult(reply, addr, r);
    echoFloat(reply, addr, db);
}

void Adc6140Panel::handleChannelImpedance(int n, OSCMessage &msg, OSCBundle &reply) {
    char addr[64]; snprintf(addr, sizeof(addr), "/codec/adc6140/ch/%d/impedance", n);
    if (msg.size() == 0) { echoEnum(reply, addr, "10k"); return; }
    char s[16] = {0};
    argAsString(msg, 0, s, sizeof(s));
    Impedance z;
    if      (strcmp(s, "2.5k") == 0) z = Impedance::K2_5;
    else if (strcmp(s, "10k")  == 0) z = Impedance::K10;
    else if (strcmp(s, "20k")  == 0) z = Impedance::K20;
    else { echoError(reply, addr, "expected 2.5k|10k|20k"); return; }
    Result r = _codec.channel(n).setImpedance(z);
    reportResult(reply, addr, r);
    echoEnum(reply, addr, s);
}

void Adc6140Panel::handleChannelCoupling(int n, OSCMessage &msg, OSCBundle &reply) {
    char addr[64]; snprintf(addr, sizeof(addr), "/codec/adc6140/ch/%d/coupling", n);
    if (msg.size() == 0) { echoEnum(reply, addr, "ac"); return; }
    char s[8] = {0};
    argAsString(msg, 0, s, sizeof(s));
    Coupling c;
    if      (strcmp(s, "ac") == 0) c = Coupling::Ac;
    else if (strcmp(s, "dc") == 0) c = Coupling::Dc;
    else { echoError(reply, addr, "expected ac|dc"); return; }
    Result r = _codec.channel(n).setCoupling(c);
    reportResult(reply, addr, r);
    echoEnum(reply, addr, s);
}

void Adc6140Panel::handleChannelInType(int n, OSCMessage &msg, OSCBundle &reply) {
    char addr[64]; snprintf(addr, sizeof(addr), "/codec/adc6140/ch/%d/intype", n);
    if (msg.size() == 0) { echoEnum(reply, addr, "mic"); return; }
    char s[8] = {0};
    argAsString(msg, 0, s, sizeof(s));
    InputType t;
    if      (strcmp(s, "mic")  == 0) t = InputType::Microphone;
    else if (strcmp(s, "line") == 0) t = InputType::Line;
    else { echoError(reply, addr, "expected mic|line"); return; }
    Result r = _codec.channel(n).setType(t);
    reportResult(reply, addr, r);
    echoEnum(reply, addr, s);
}

void Adc6140Panel::handleChannelSource(int n, OSCMessage &msg, OSCBundle &reply) {
    char addr[64]; snprintf(addr, sizeof(addr), "/codec/adc6140/ch/%d/source", n);
    if (msg.size() == 0) { echoEnum(reply, addr, "differential"); return; }
    char s[24] = {0};
    argAsString(msg, 0, s, sizeof(s));
    InputSource src;
    if      (strcmp(s, "differential")  == 0) src = InputSource::Differential;
    else if (strcmp(s, "single_ended")  == 0) src = InputSource::SingleEnded;
    else { echoError(reply, addr, "expected differential|single_ended"); return; }
    Result r = _codec.channel(n).setSource(src);
    reportResult(reply, addr, r);
    echoEnum(reply, addr, s);
}

void Adc6140Panel::handleChannelDre(int n, OSCMessage &msg, OSCBundle &reply) {
    char addr[64]; snprintf(addr, sizeof(addr), "/codec/adc6140/ch/%d/dre", n);
    if (msg.size() == 0) { echoInt(reply, addr, 0); return; }
    int v = argAsInt(msg, 0);
    Result r = _codec.channel(n).setDreEnable(v != 0);
    reportResult(reply, addr, r);
    echoInt(reply, addr, v ? 1 : 0);
}

void Adc6140Panel::handleChannelGainCal(int n, OSCMessage &msg, OSCBundle &reply) {
    char addr[64]; snprintf(addr, sizeof(addr), "/codec/adc6140/ch/%d/gaincal", n);
    if (msg.size() == 0) { echoFloat(reply, addr, 0.0f); return; }
    float db = argAsFloat(msg, 0);
    Result r = _codec.channel(n).setGainCalDb(db);
    reportResult(reply, addr, r);
    echoFloat(reply, addr, db);
}

// ----- chip-global handlers -----------------------------------------------

void Adc6140Panel::handleMicbias(OSCMessage &msg, OSCBundle &reply) {
    const char *addr = "/codec/adc6140/micbias";
    if (msg.size() == 0) { echoEnum(reply, addr, "off"); return; }
    char s[20] = {0};
    argAsString(msg, 0, s, sizeof(s));
    MicBias m;
    if      (strcmp(s, "off")           == 0) m = MicBias::Off;
    else if (strcmp(s, "vref")          == 0) m = MicBias::Vref;
    else if (strcmp(s, "vref_boosted")  == 0) m = MicBias::VrefBoosted;
    else if (strcmp(s, "avdd")          == 0) m = MicBias::Avdd;
    else { echoError(reply, addr, "expected off|vref|vref_boosted|avdd"); return; }
    reportResult(reply, addr, _codec.setMicBias(m));
    echoEnum(reply, addr, s);
}

void Adc6140Panel::handleFullscale(OSCMessage &msg, OSCBundle &reply) {
    const char *addr = "/codec/adc6140/fullscale";
    if (msg.size() == 0) { echoEnum(reply, addr, "2v75"); return; }
    char s[16] = {0};
    argAsString(msg, 0, s, sizeof(s));
    FullScale fs;
    if      (strcmp(s, "2v75")  == 0) fs = FullScale::V2Rms275;
    else if (strcmp(s, "2v5")   == 0) fs = FullScale::V1Rms8250;
    else if (strcmp(s, "1v375") == 0) fs = FullScale::V1Rms1375;
    else { echoError(reply, addr, "expected 2v75|2v5|1v375"); return; }
    reportResult(reply, addr, _codec.setFullScale(fs));
    echoEnum(reply, addr, s);
}

void Adc6140Panel::handleHpf(OSCMessage &msg, OSCBundle &reply) {
    const char *addr = "/codec/adc6140/hpf";
    if (msg.size() == 0) { echoEnum(reply, addr, "12hz"); return; }
    char s[20] = {0};
    argAsString(msg, 0, s, sizeof(s));
    HpfCutoff h;
    if      (strcmp(s, "programmable") == 0) h = HpfCutoff::Programmable;
    else if (strcmp(s, "12hz")         == 0) h = HpfCutoff::Cutoff12Hz;
    else if (strcmp(s, "96hz")         == 0) h = HpfCutoff::Cutoff96Hz;
    else if (strcmp(s, "384hz")        == 0) h = HpfCutoff::Cutoff384Hz;
    else { echoError(reply, addr, "expected programmable|12hz|96hz|384hz"); return; }
    reportResult(reply, addr, _codec.setHpf(h));
    echoEnum(reply, addr, s);
}

void Adc6140Panel::handleDecimFilt(OSCMessage &msg, OSCBundle &reply) {
    const char *addr = "/codec/adc6140/decimfilt";
    if (msg.size() == 0) { echoEnum(reply, addr, "linear"); return; }
    char s[24] = {0};
    argAsString(msg, 0, s, sizeof(s));
    DecimationFilter d;
    if      (strcmp(s, "linear")            == 0) d = DecimationFilter::LinearPhase;
    else if (strcmp(s, "low_latency")       == 0) d = DecimationFilter::LowLatency;
    else if (strcmp(s, "ultra_low_latency") == 0) d = DecimationFilter::UltraLowLatency;
    else { echoError(reply, addr, "expected linear|low_latency|ultra_low_latency"); return; }
    reportResult(reply, addr, _codec.setDecimationFilter(d));
    echoEnum(reply, addr, s);
}

void Adc6140Panel::handleChSum(OSCMessage &msg, OSCBundle &reply) {
    const char *addr = "/codec/adc6140/chsum";
    if (msg.size() == 0) { echoEnum(reply, addr, "off"); return; }
    char s[12] = {0};
    argAsString(msg, 0, s, sizeof(s));
    ChannelSumMode c;
    if      (strcmp(s, "off")   == 0) c = ChannelSumMode::Off;
    else if (strcmp(s, "pairs") == 0) c = ChannelSumMode::Pairs;
    else if (strcmp(s, "quad")  == 0) c = ChannelSumMode::Quad;
    else { echoError(reply, addr, "expected off|pairs|quad"); return; }
    reportResult(reply, addr, _codec.setChannelSumMode(c));
    echoEnum(reply, addr, s);
}

void Adc6140Panel::handleMode(OSCMessage &msg, OSCBundle &reply) {
    const char *addr = "/codec/adc6140/mode";
    if (msg.size() == 0) { echoEnum(reply, addr, "dre"); return; }
    char s[8] = {0};
    argAsString(msg, 0, s, sizeof(s));
    DreAgcMode m;
    if      (strcmp(s, "dre") == 0) m = DreAgcMode::Dre;
    else if (strcmp(s, "agc") == 0) m = DreAgcMode::Agc;
    else { echoError(reply, addr, "expected dre|agc"); return; }
    reportResult(reply, addr, _codec.setDreAgcMode(m));
    echoEnum(reply, addr, s);
}

void Adc6140Panel::handleDreLevel(OSCMessage &msg, OSCBundle &reply) {
    const char *addr = "/codec/adc6140/dre/level";
    if (msg.size() == 0) { echoInt(reply, addr, -54); return; }
    int v = argAsInt(msg, 0);
    reportResult(reply, addr, _codec.setDreLevel((int8_t)v));
    echoInt(reply, addr, v);
}

void Adc6140Panel::handleDreMaxGain(OSCMessage &msg, OSCBundle &reply) {
    const char *addr = "/codec/adc6140/dre/maxgain";
    if (msg.size() == 0) { echoInt(reply, addr, 24); return; }
    int v = argAsInt(msg, 0);
    reportResult(reply, addr, _codec.setDreMaxGain((uint8_t)v));
    echoInt(reply, addr, v);
}

void Adc6140Panel::handleAgcTarget(OSCMessage &msg, OSCBundle &reply) {
    const char *addr = "/codec/adc6140/agc/target";
    if (msg.size() == 0) { echoInt(reply, addr, -34); return; }
    int v = argAsInt(msg, 0);
    reportResult(reply, addr, _codec.setAgcTargetLevel((int8_t)v));
    echoInt(reply, addr, v);
}

void Adc6140Panel::handleAgcMaxGain(OSCMessage &msg, OSCBundle &reply) {
    const char *addr = "/codec/adc6140/agc/maxgain";
    if (msg.size() == 0) { echoInt(reply, addr, 24); return; }
    int v = argAsInt(msg, 0);
    reportResult(reply, addr, _codec.setAgcMaxGain((uint8_t)v));
    echoInt(reply, addr, v);
}

void Adc6140Panel::handleInfo(OSCMessage &msg, OSCBundle &reply) {
    (void)msg;
    tlv320adc6140::DeviceInfo di = _codec.info();
    OSCMessage m("/codec/adc6140/info");
    m.add(di.model);
    m.add((int32_t)di.i2cAddr);
    reply.add(m);
}

void Adc6140Panel::handleStatus(OSCMessage &msg, OSCBundle &reply) {
    (void)msg;
    tlv320adc6140::Status s = _codec.readStatus();
    OSCMessage m("/codec/adc6140/status");
    m.add((int32_t)s.devSts0);
    m.add((int32_t)s.devSts1);
    m.add((int32_t)s.asiSts);
    m.add(s.asiClockValid ? 1 : 0);
    reply.add(m);
}

void Adc6140Panel::handleRegSet(OSCMessage &msg, OSCBundle &reply) {
    const char *addr = "/codec/adc6140/reg/set";
    if (msg.size() < 2) { echoError(reply, addr, "need reg value"); return; }
    int reg = argAsInt(msg, 0);
    int val = argAsInt(msg, 1);
    reportResult(reply, addr, _codec.writeRegister(0, (uint8_t)reg, (uint8_t)val));
    OSCMessage m(addr);
    m.add((int32_t)reg);
    m.add((int32_t)val);
    reply.add(m);
}

void Adc6140Panel::handleRegGet(OSCMessage &msg, OSCBundle &reply) {
    const char *addr = "/codec/adc6140/reg/get";
    if (msg.size() < 1) { echoError(reply, addr, "need reg"); return; }
    int reg = argAsInt(msg, 0);
    uint8_t v = _codec.readRegister(0, (uint8_t)reg);
    OSCMessage m(addr);
    m.add((int32_t)reg);
    m.add((int32_t)v);
    reply.add(m);
}
