// TDspLinn — LinnStrument control over the Teensy's USB-host port.
//
// The LinnStrument exposes EVERY panel setting as an NRPN (CC 99/98 = parameter MSB/LSB, CC 6/38 =
// value MSB/LSB; per-split parameters 0-99 are the LEFT split, +100 the RIGHT split, globals 200+),
// answers a read request (NRPN 299 = "send me parameter N") with the same four CCs, and lights pads
// with CC 20 (column) / CC 21 (row) / CC 22 (colour). Reference: the device firmware's receivedNrpn()
// in rogerlinndesign/linnstrument-firmware ls_midi.ino; T-DSP's planning/linnstrument-panel/PLAN.md.
//
// This class owns the host-side MIDIDevice: it detects attach/detach (and whether the device is a
// LinnStrument, by product string), writes NRPNs, assembles the device's NRPN replies out of the host
// CC stream (CONSUMING those four CCs so they never reach the synths as data-entry noise), keeps a
// shadow table of every value, paces a full re-read (~200 queries), and pushes changes on the control
// broadcast as `@LINN=` (connection JSON), `@LINN.V=<param>,<value>` and `@LINN.SYNC=<done>/<total>`.
//
// FLOW CONTROL (hardware lesson, 2026-10-01): USBHost_t36's MIDIDeviceBase::write_packed() SPINS
// FOREVER once its two 64-byte transmit buffers are both in flight and the device never completes the
// transfers (a LinnStrument that isn't servicing USB MIDI in — e.g. set to its MIDI jacks — hung the
// whole box). So nothing here writes blindly: every outgoing "op" is ONE burst (<= 16 MIDI messages, so
// one transfer) that ENDS WITH A READ of some parameter, the device's reply returns the credit, at most
// two ops are ever in flight, and if the device stops answering we stop writing until it re-attaches.
#pragma once
#include <Arduino.h>
#include <USBHost_t36.h>

namespace tdsp {

class LinnCtl {
public:
    static constexpr int kMaxParam = 300;      // 0..299 (299 = query)
    enum Resp : int8_t { RespUnknown = -1, RespNo = 0, RespYes = 1 };

    explicit LinnCtl(MIDIDevice &dev) : dev_(dev) { for (auto &v : vals_) v = -1; memset(pend_, 0, sizeof(pend_)); }

    void begin(Print *push) { push_ = push; }

    // ---- detection ----------------------------------------------------------------------------
    bool        connected() const { return connected_; }
    bool        isLinn()    const { return connected_ && isLinn_; }
    Resp        responsive() const { return resp_; }
    const char *name()      const { return name_; }
    uint16_t    vid()       const { return vid_; }
    uint16_t    pid()       const { return pid_; }

    // ---- settings the app owns (not persisted) ------------------------------------------------
    int  cols        = 25;      // 25 = full LinnStrument, 16 = LinnStrument 128 (split point / pad painting)
    bool followMode  = false;   // mirror T-DSP's MIDI mode (MPE handshake) onto the device
    bool followTempo = false;   // push the master clock BPM to NRPN 238

    // ---- control (all queued; see FLOW CONTROL) -----------------------------------------------
    void set(int param, int value) { if (param >= 0 && param < 299) push(Op{OpSet, (int16_t)param, (int16_t)value, 0}); }
    void query(int param)          { if (param >= 0 && param < 299) push(Op{OpQuery, (int16_t)param, 0, 0}); }
    void syncAll() {
        if (!isLinn()) return;
        for (int p = 0;   p <= 66;  p++) enqueue(p);     // LEFT split
        for (int p = 100; p <= 166; p++) enqueue(p);     // RIGHT split
        for (int p = 200; p <= 270; p++) enqueue(p);     // globals
        syncTotal_ = pendCount_; syncDone_ = 0;
        pushSync();
    }
    void preset(int n) { if (n < 0) n = 0; if (n > 5) n = 5; set(243, n); resyncAt_ = millis() + 600; }
    void light(int col, int row, int colour) { push(Op{OpLight, (int16_t)col, (int16_t)row, (int16_t)colour}); }
    void clearLights() { push(Op{OpClear, 0, 0, 0}); }                 // CC 24 = clear the custom LED pattern
    void paintAll(int colour) { paintColour_ = colour & 0x7f; paintCursor_ = 0; paintTotal_ = cols * 8; }   // drained 2 cells per op
    // MPE handshake: make both splits talk exactly the dialect T-DSP's MidiRouter expects (channel per
    // note from main channel 1, member bend range = T-DSP's, Z = channel pressure, Y = CC74) — or the
    // GM-friendly inverse (one channel, bend 2).
    void applyMpe(bool mpe, int bendRange) {
        if (!isLinn()) return;
        for (int side = 0; side <= 100; side += 100) {
            set(side + 0,  mpe ? 1 : 0);          // MIDI mode: 1 = channel per note, 0 = one channel
            set(side + 1,  1);                    // main channel 1 (MPE master)
            set(side + 19, mpe ? bendRange : 2);  // bend range (semitones)
            set(side + 20, 1);                    // send X (pitch)
            set(side + 24, 1);                    // send Y (timbre)
            set(side + 39, 2);                    // Y expression = CC74
            set(side + 27, 1);                    // send Z (loudness)
            set(side + 28, 1);                    // Z expression = channel pressure
        }
    }
    void setTempo(float bpm) { tempo_ = bpm; }

    // ---- host CC stream: assemble NRPN replies; returns true when the CC was consumed --------------
    bool onHostCC(uint8_t ch, uint8_t cc, uint8_t val) {
        if (!isLinn_ || ch < 1 || ch > 16) return false;
        Asm &a = asm_[ch];
        switch (cc) {
            case 99: a.pMsb = val; a.vMsb = -1; return true;
            case 98: a.pLsb = val; a.vMsb = -1; return true;
            case 6:  if (a.pMsb < 0 || a.pLsb < 0) return false; a.vMsb = val; return true;
            case 38: {
                if (a.pMsb < 0 || a.pLsb < 0 || a.vMsb < 0) return false;
                const int p = (a.pMsb << 7) | a.pLsb, v = (a.vMsb << 7) | val;
                a.vMsb = -1;
                onReply(p, v);
                return true;
            }
            default: return false;
        }
    }

    // ---- loop() -------------------------------------------------------------------------------
    void service(uint32_t now) {
        // attach / detach edges
        const bool c = (bool)dev_;
        if (c != connected_) {
            connected_ = c;
            if (c) {
                vid_ = dev_.idVendor(); pid_ = dev_.idProduct();
                const uint8_t *pn = dev_.product(); const uint8_t *mn = dev_.manufacturer();
                snprintf(name_, sizeof(name_), "%s", pn && pn[0] ? (const char *)pn : (mn && mn[0] ? (const char *)mn : "USB MIDI device"));
                char low[sizeof(name_)]; for (size_t i = 0; i < sizeof(low); i++) { low[i] = (char)tolower((unsigned char)name_[i]); if (!name_[i]) break; } low[sizeof(low) - 1] = 0;
                isLinn_ = strstr(low, "linnstrument") != nullptr;
                resetLink();
                Serial.printf("[linn] attached: %s (vid %04x pid %04x)%s\n", name_, vid_, pid_, isLinn_ ? " -> LinnStrument" : " (not a LinnStrument)");
                if (isLinn_) { probeAt_ = now + 800; probes_ = 0; }   // let it settle, then ask one question
            } else {
                Serial.printf("[linn] detached: %s\n", name_);
                isLinn_ = false; resetLink();
            }
            pushStatus();
        }
        if (!isLinn()) return;

        // first contact: a lone probe read; the reply proves the device consumes our data (then sync)
        if (probeAt_ && (int32_t)(now - probeAt_) >= 0) {
            probeAt_ = 0;
            if (resp_ != RespYes && probes_ < 2 && inflight_ < 2) { probes_++; sendBurst(Op{OpQuery, 234, 0, 0}, now); if (probes_ < 2) probeAt_ = now + 2500; }
        }
        // reply timeout: the device went quiet with data in flight -> stop writing (see FLOW CONTROL)
        if (inflight_ > 0 && (int32_t)(now - lastSendAt_) >= 1500) {
            inflight_ = 0;
            if (resp_ != RespNo) { resp_ = RespNo; qHead_ = qTail_ = 0; pendCount_ = 0; memset(pend_, 0, sizeof(pend_)); paintTotal_ = 0;
                                   Serial.println("[linn] LinnStrument is not answering USB MIDI (Power/MIDI set to the jacks? asleep?) -> control paused"); pushStatus(); }
        }
        if (resp_ != RespYes) return;   // nothing goes out until the device has answered once

        if (resyncAt_ && (int32_t)(now - resyncAt_) >= 0) { resyncAt_ = 0; syncAll(); }
        if (followTempo && tempo_ > 0 && (int32_t)(now - lastTempoAt_) >= 500) {
            const int b = (int)(tempo_ + 0.5f);
            if (b != lastTempoSent_ && b >= 1 && b <= 360) { lastTempoSent_ = b; lastTempoAt_ = now; set(238, b); }
        }
        // drain: queued ops first, then pad painting, then the sync's pending reads
        if (inflight_ < 2 && (int32_t)(now - lastSendAt_) >= 3) {
            if (qHead_ != qTail_) { Op op = q_[qHead_]; qHead_ = (qHead_ + 1) % kQ; sendBurst(op, now); }
            else if (paintCursor_ < paintTotal_) { sendBurst(Op{OpPaint, 0, 0, 0}, now); }
            else if (pendCount_ > 0) {
                for (int i = 0; i < kMaxParam; i++) {
                    const int p = (qCursor_ + i) % kMaxParam;
                    if (pend_[p >> 3] & (1 << (p & 7))) { pend_[p >> 3] &= ~(1 << (p & 7)); pendCount_--; qCursor_ = p + 1; sendBurst(Op{OpQuery, (int16_t)p, 0, 0}, now); break; }
                }
            }
        }
    }

    // ---- status / values -------------------------------------------------------------------------
    int  value(int p) const { return (p >= 0 && p < kMaxParam) ? vals_[p] : -1; }
    int  syncDone()  const { return syncDone_; }
    int  syncTotal() const { return syncTotal_; }
    // {"connected":1,"linn":1,"resp":1,"name":"LinnStrument","vid":…,"pid":…,"cols":25,"follow":0,"tempo":0,"synced":[done,total]} (+ "v":{"19":24,…})
    void statusJson(Print &o, bool withValues) const {
        o.printf("{\"connected\":%d,\"linn\":%d,\"resp\":%d,\"name\":\"", connected_ ? 1 : 0, isLinn() ? 1 : 0, (int)resp_);
        for (const char *p = name_; *p; p++) { if (*p == '"' || *p == '\\') o.write('\\'); if ((uint8_t)*p >= 32) o.write(*p); }
        o.printf("\",\"vid\":%u,\"pid\":%u,\"cols\":%d,\"follow\":%d,\"tempo\":%d,\"synced\":[%d,%d]",
                 (unsigned)vid_, (unsigned)pid_, cols, followMode ? 1 : 0, followTempo ? 1 : 0, syncDone_, syncTotal_);
        if (withValues) {
            o.print(",\"v\":{"); bool first = true;
            for (int p = 0; p < 299; p++) if (vals_[p] >= 0) { o.printf("%s\"%d\":%d", first ? "" : ",", p, vals_[p]); first = false; }
            o.print("}");
        }
        o.print("}");
    }
    void pushStatus() { if (push_) { push_->print("@LINN="); statusJson(*push_, false); push_->print("\n"); } }

private:
    enum OpKind : uint8_t { OpQuery, OpSet, OpLight, OpClear, OpPaint };
    struct Op  { OpKind kind; int16_t a, b, c; };
    struct Asm { int8_t pMsb = -1, pLsb = -1, vMsb = -1; };
    static constexpr int kQ = 96;

    void cc(uint8_t c, uint8_t v) { dev_.sendControlChange(c, v, 1); }
    void nrpnRaw(int p, int v) { cc(99, (p >> 7) & 0x7f); cc(98, p & 0x7f); cc(6, (v >> 7) & 0x7f); cc(38, v & 0x7f); }
    // ONE burst = one USB transfer (<= 16 MIDI messages), always ending in a read so a reply comes back.
    void sendBurst(const Op &op, uint32_t now) {
        int readBack = 234;   // a harmless global to read when the op itself has nothing to confirm
        switch (op.kind) {
            case OpQuery: readBack = op.a; break;
            case OpSet:   nrpnRaw(op.a, op.b); vals_[op.a] = op.b; readBack = op.a; break;      // optimistic; the read-back shows the clamped truth
            case OpLight: cc(20, op.a & 0x7f); cc(21, op.b & 0x7f); cc(22, op.c & 0x7f); break;
            case OpClear: cc(24, 1); break;
            case OpPaint:
                for (int k = 0; k < 2 && paintCursor_ < paintTotal_; k++, paintCursor_++) {
                    const int col = 1 + paintCursor_ / 8, row = paintCursor_ % 8;
                    cc(20, col); cc(21, row); cc(22, paintColour_);
                }
                break;
        }
        nrpnRaw(299, readBack);
        dev_.send_now();
        inflight_++; lastSendAt_ = now;
    }
    void onReply(int p, int v) {
        if (inflight_ > 0) inflight_--;
        const bool first = (resp_ != RespYes);
        resp_ = RespYes;
        if (p >= 0 && p < 299) {
            vals_[p] = (int16_t)v;
            if (push_) push_->printf("@LINN.V=%d,%d\n", p, v);
            if (syncTotal_ && syncDone_ < syncTotal_) { syncDone_++; if (syncDone_ == syncTotal_ || (syncDone_ % 20) == 0) pushSync(); }
        }
        if (first) { Serial.println("[linn] LinnStrument answers -> reading every setting"); pushStatus(); syncAll(); }
    }
    void push(const Op &op) {
        if (!isLinn() || resp_ == RespNo) return;
        const int next = (qTail_ + 1) % kQ;
        if (next == qHead_) { Serial.println("[linn] op queue full, dropped"); return; }
        q_[qTail_] = op; qTail_ = next;
    }
    void enqueue(int p) { if (p < 0 || p >= kMaxParam) return; if (!(pend_[p >> 3] & (1 << (p & 7)))) { pend_[p >> 3] |= (1 << (p & 7)); pendCount_++; } }
    void pushSync() { if (push_) push_->printf("@LINN.SYNC=%d/%d\n", syncDone_, syncTotal_); }
    void resetLink() {
        for (auto &v : vals_) v = -1;
        for (auto &a : asm_) a = Asm();
        memset(pend_, 0, sizeof(pend_)); pendCount_ = 0; qHead_ = qTail_ = 0; inflight_ = 0;
        syncTotal_ = syncDone_ = 0; resp_ = RespUnknown; probeAt_ = 0; probes_ = 0; resyncAt_ = 0; paintTotal_ = paintCursor_ = 0;
    }

    MIDIDevice &dev_;
    Print      *push_ = nullptr;
    bool        connected_ = false, isLinn_ = false;
    Resp        resp_ = RespUnknown;
    uint16_t    vid_ = 0, pid_ = 0;
    char        name_[48] = "";
    int16_t     vals_[kMaxParam];
    Asm         asm_[17];
    uint8_t     pend_[(kMaxParam + 7) / 8];
    int         pendCount_ = 0, qCursor_ = 0;
    Op          q_[kQ];
    int         qHead_ = 0, qTail_ = 0, inflight_ = 0, probes_ = 0;
    uint32_t    lastSendAt_ = 0, probeAt_ = 0, resyncAt_ = 0, lastTempoAt_ = 0;
    int         syncDone_ = 0, syncTotal_ = 0, lastTempoSent_ = -1;
    int         paintColour_ = 0, paintCursor_ = 0, paintTotal_ = 0;
    float       tempo_ = 0;
};

}  // namespace tdsp
