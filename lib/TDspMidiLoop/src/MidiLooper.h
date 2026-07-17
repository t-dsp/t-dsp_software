// MidiLooper.h — beat-aware MIDI loop recorder + player for one voice.
//
// A MidiLooper IS a tdsp::MidiSink: register it DOWNSTREAM of the arp
// (arp.addDownstream(&looper)) so it captures the arp's *baked* note stream
// alongside the synth. Playback is emitted into a separate output sink (the
// synth directly), so the loop never re-enters the arp — no double-arp, no
// feedback. One MidiLooper per voice (voice 1 -> arp1/synthA, voice 2 -> arp2/synthB).
//
// (Named MidiLooper to avoid colliding with the audio-sample tdsp::Looper in
// lib/TDspLooper — that one loops audio blocks; this one loops MIDI events.)
//
// Recording model (matches the design):
//   armRecord()    -> Armed: waits for the first note. The master transport must
//                     already be running (the caller starts it + the click).
//   first note-on  -> anchor = the DOWNBEAT of the bar the note lands in; the
//                     note's offset from that downbeat is preserved. loop length
//                     = bars * beatsPerBar. State -> Recording.
//   during window  -> note-ONs inside [0, loopBeats) captured; later note-ons
//                     ignored. Expression (bend/timbre/pressure/mod/sustain)
//                     captured inside the window.
//   window end     -> playback starts IMMEDIATELY at the loop's beat 0, while the
//                     recorder stays open for the TAIL: note-OFFs for notes still
//                     held are captured at their WRAPPED tick so a phrase played
//                     "all the way out" rings into the top of the loop.
//   armOverdub()   -> layers a new pass onto the existing clip while it plays;
//                     positions wrap via fmod; runs until stop().
#pragma once
#include <Arduino.h>
#include <math.h>
#include <MidiSink.h>
#include <Clock.h>
#include "LoopEvent.h"
#include "LoopPlayer.h"

namespace tdsp {

class MidiLooper : public tdsp::MidiSink {
public:
    enum State : uint8_t { Idle = 0, Armed = 1, Recording = 2, Overdub = 3, Playing = 4 };

    // out = the synth sink that playback drives (NOT the arp).
    void begin(tdsp::Clock *clk, tdsp::MidiSink *out) {
        clk_ = clk; out_ = out; player_.begin(clk, out);
    }

    // ---- Configuration (takes effect on the next fresh recording) ----
    void setBars(uint8_t b) {                     // 1, 2, 4 or 8
        if (b == 1 || b == 2 || b == 4 || b == 8) bars_ = b;
    }
    uint8_t  bars()        const { return bars_; }
    // Meter comes from the master Clock (the authoritative grid), NOT a private
    // copy — so the loop's downbeats line up with drums/song/metronome even when
    // one of those owns the time signature. Set the signature via the clock
    // (e.g. Conductor::setBeatsPerBar) before recording.
    uint8_t  beatsPerBar() const { return curBpb(); }
    State    state()       const { return state_; }
    bool     hasClip()     const { return clip_.hasData(); }
    uint16_t eventCount()  const { return clip_.count; }

    // ---- Transport ----
    void armRecord() {                            // replace
        player_.stop();
        clip_.clear();
        resetPass();
        state_ = Armed;
    }
    void armOverdub() {
        if (!clip_.hasData()) { armRecord(); return; }   // nothing to layer onto
        resetPass();
        anchor_    = clipAnchor_;
        loopBeats_ = clip_.loopTicks / 24.0;
        if (!player_.isPlaying()) player_.play(&clip_, anchor_);
        state_ = Overdub;
    }
    void stop() {
        switch (state_) {
            case Recording:                        // finalize early
                forceReleaseHeld();
                startPlayback();
                break;
            case Overdub:                          // punch out, keep looping
                forceReleaseHeld();
                state_ = Playing;
                break;
            case Playing:                          // stop the loop (clip retained)
            case Armed:
                player_.stop();
                state_ = Idle;
                break;
            default: break;
        }
        tailOpen_ = false;
        graceOpen_ = false;
    }
    void clear() {
        player_.stop();
        clip_.clear();
        resetPass();
        state_ = Idle;
    }
    // Resume a retained clip, re-locked to its original grid phase.
    void resume() {
        if (state_ == Idle && clip_.hasData()) {
            player_.play(&clip_, clipAnchor_);
            state_ = Playing;
        }
    }

    // ---- Per-loop() servicing ----
    void poll() {                                  // call after Conductor::update()
        if (!clk_ || !clk_->running()) return;
        const double now = clk_->positionBeats();
        if (state_ == Recording && (now - anchor_) >= loopBeats_) {
            startPlayback();                       // window closed -> play + open tail
            graceOpen_ = true;                     // ...and briefly still accept a late "one"
        }
        // Grace spans only the tolerance past the end; after that the take is closed. Note only
        // this NATURAL close opens it — an early stop() finalizes with no grace window, so a note
        // played after you punch out can never sneak into the clip.
        if (graceOpen_ && (now - anchor_) >= loopBeats_ + kEdgeTol) graceOpen_ = false;
        if (tailOpen_) {
            if (nHeld_ == 0) tailOpen_ = false;
            else if ((now - anchor_) >= loopBeats_ * 2.0) {   // tail cap: 1 extra loop
                forceReleaseHeld();
                tailOpen_ = false;
            }
        }
    }
    void tick() { player_.tick(); }                // advance playback

    // 0..1000 progress for a UI bar (record fill or playback phase).
    uint16_t positionPermille() const {
        if (state_ == Playing || state_ == Overdub) return player_.positionPermille();
        if (state_ == Recording && loopBeats_ > 0.0 && clk_) {
            double rel = clk_->positionBeats() - anchor_;
            if (rel < 0) rel = 0;
            if (rel > loopBeats_) rel = loopBeats_;
            return (uint16_t)(rel / loopBeats_ * 1000.0);
        }
        return 0;
    }

    // ---- MidiSink capture (downstream of the arp) ----
    void onNoteOn(uint8_t ch, uint8_t note, uint8_t vel) override {
        if (vel == 0) { onNoteOff(ch, note, 0); return; }
        if (state_ == Armed) latchAnchor();        // first press: lock the loop grid
        // A "one" struck a hair LATE lands after the window closed — grace catches it and puts
        // it at the loop top rather than dropping it (the mirror of latchAnchor's early bias).
        const bool grace = !capturing() && graceOpen_;
        if (!capturing() && !grace) return;
        int32_t t = noteOnTick();
        if (t < 0) return;                         // outside the record window (+ tolerance)
        if (insertLive({ (uint16_t)t, LNoteOn, ch, note, vel, 0 })) {
            addHeld(ch, note, grace);
            if (grace) tailOpen_ = true;           // keep capture open so its release is recorded
        }
    }
    void onNoteOff(uint8_t ch, uint8_t note, uint8_t vel) override {
        if (!capturingOrTail()) return;
        const int8_t hi = heldIndex(ch, note);
        if (hi < 0) return;                        // only release notes this pass recorded
        int32_t t = noteOffTick(held_[hi][2] != 0);
        removeHeldAt(hi);
        if (t >= 0) insertLive({ (uint16_t)t, LNoteOff, ch, note, vel, 0 });
    }
    void onPitchBend(uint8_t ch, float semis) override {
        int16_t v = (int16_t)lroundf(semis * 256.0f);
        if (!ctrlGate(ch, LPitchBend, v)) return;
        int32_t t = ctrlTick(); if (t < 0) return;
        insertLive({ (uint16_t)t, LPitchBend, ch, 0, 0, v });
    }
    void onTimbre(uint8_t ch, float value)   override { ctrlU8(ch, LTimbre,   value); }
    void onPressure(uint8_t ch, float value) override { ctrlU8(ch, LPressure, value); }
    void onModWheel(uint8_t ch, float value) override { ctrlU8(ch, LModWheel, value); }
    void onSustain(uint8_t ch, bool on) override {
        uint8_t v = on ? 1 : 0;
        if (!ctrlGate(ch, LSustain, v)) return;
        int32_t t = ctrlTick(); if (t < 0) return;
        insertLive({ (uint16_t)t, LSustain, ch, 0, v, 0 });
    }
    // Program changes and panics are global synth state, not performance — skip.

private:
    // ---- capture-phase predicates ----
    bool capturing()       const { return state_ == Recording || state_ == Overdub; }
    bool capturingOrTail() const { return capturing() || tailOpen_; }

    // Insert a captured event, keeping the playing cursor valid: insert() shifts
    // array elements, so any live insert (tail note-off, overdub) below the player's
    // cursor would otherwise desync it — resync after mutating a playing clip.
    bool insertLive(const LoopEvent &e) {
        bool ok = clip_.insert(e);
        if (ok && player_.isPlaying()) player_.resyncCursor();
        return ok;
    }

    // Musical edge tolerance, in beats. Players ANTICIPATE the downbeat: the "one" is struck a
    // hair EARLY (just before the bar line) as often as it is late. Both mean the same thing, so
    // a hit inside this window counts as landing ON the one — instead of being floored back a
    // whole bar (early) or dropped (late). ~1/8 beat = a 32nd at 4/4 (~62 ms at 120 BPM): wide
    // enough for human timing, narrow enough that a note deliberately held past the seam still
    // reads as a trail and wraps into the loop top.
    static constexpr double kEdgeTol = 0.125;

    // First note-on of a fresh (replace) recording latches the anchor to the
    // downbeat of the bar the note lands in — preserving its offset from that
    // downbeat — and fixes the loop length. The +kEdgeTol bias is what makes an early
    // "one" stay put: without it, floor() on a note struck a few ms BEFORE the bar line
    // snaps the anchor back a full bar, parking your downbeat at the very END of the loop.
    void latchAnchor() {
        const double now = clk_->positionBeats();
        const uint8_t bpb = curBpb();
        anchor_    = floor((now + kEdgeTol) / (double)bpb) * (double)bpb;
        loopBeats_ = (double)bars_ * (double)bpb;
        clip_.loopTicks   = (uint32_t)lround(loopBeats_ * 24.0);
        clip_.bars        = bars_;
        clip_.beatsPerBar = bpb;
        state_ = Recording;
    }
    uint8_t curBpb() const {
        uint8_t n = clk_ ? clk_->beatsPerBar() : 4;
        return n ? n : 4;
    }

    // Tick of a note-ON. Replace: inside [0,loopBeats) plus kEdgeTol of slack at BOTH edges — a
    // hair early (before the top) or a hair past the end both mean "the one", so both land at
    // tick 0 rather than being dropped. Overdub: fmod-wrapped.
    int32_t noteOnTick() {
        const double now = clk_->positionBeats();
        if (state_ == Overdub) return wrapTick(now - anchor_);
        double rel = now - anchor_;
        if (rel < 0)                     return (rel >= -kEdgeTol) ? 0 : -1;   // just before the top
        if (rel < loopBeats_)            return clampTick(rel);
        if (rel < loopBeats_ + kEdgeTol) return 0;                             // just past the end -> the top
        return -1;
    }
    // Tick of a note-OFF. `graceNote` = its note-on was caught just past the loop end and placed
    // at tick 0, so its release is measured from that top (wrap), not from the closed window.
    int32_t noteOffTick(bool graceNote) {
        const double now = clk_->positionBeats();
        if (state_ == Overdub) return wrapTick(now - anchor_);
        double rel = now - anchor_;
        if (graceNote)        return wrapTick(rel - loopBeats_);
        if (rel < loopBeats_) return clampTick(rel < 0 ? 0 : rel);
        // Let go just past the end: you meant to release ON the loop's end. Clamp to the last
        // tick instead of wrapping — a slightly-late release must never land INSIDE the top,
        // where it would cut the loop's opening note. Hold it longer than the tolerance and it
        // IS a deliberate trail, which still wraps in below.
        if (rel < loopBeats_ + kEdgeTol) return (int32_t)clip_.loopTicks - 1;
        return wrapTick(rel - loopBeats_);         // tail release rings into the top
    }
    int32_t ctrlTick() {
        const double now = clk_->positionBeats();
        if (state_ == Overdub) return wrapTick(now - anchor_);
        double rel = now - anchor_;
        if (rel < 0) return (rel >= -kEdgeTol) ? 0 : -1;   // early expression rides with its note
        if (rel >= loopBeats_) return -1;                  // expression only inside the window
        return clampTick(rel);
    }
    int32_t clampTick(double relBeats) const {
        int32_t t = (int32_t)lround(relBeats * 24.0);
        int32_t max = (int32_t)clip_.loopTicks - 1;
        if (t < 0) t = 0;
        if (t > max) t = max;
        return t;
    }
    int32_t wrapTick(double relBeats) const {
        if (loopBeats_ <= 0.0) return 0;
        double w = fmod(relBeats, loopBeats_);
        if (w < 0) w += loopBeats_;
        return clampTick(w);
    }

    // Expression decimation: record a controller only when its quantized value
    // actually moved, so continuous MPE streams don't flood the clip.
    void ctrlU8(uint8_t ch, uint8_t type, float value) {
        int v = (int)lroundf(value * 255.0f);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        if (!ctrlGate(ch, type, (int16_t)v)) return;
        int32_t t = ctrlTick(); if (t < 0) return;
        insertLive({ (uint16_t)t, type, ch, 0, (uint8_t)v, 0 });
    }
    bool ctrlGate(uint8_t ch, uint8_t type, int16_t v) {
        if (!capturing()) return false;            // no expression in the tail
        if (ch < 1 || ch > 16) return false;
        int16_t &last = lastCtrl_[type & 7][ch];
        if (v == last) return false;
        // pitch bend: ignore sub-1/16-semitone jitter (256/16 = 16 counts)
        if (type == LPitchBend && last != INT16_MIN && (int)abs((int)v - (int)last) < 16) return false;
        last = v;
        return true;
    }

    // ---- held-note set (this recording pass) ----
    // Each entry is [ch, note, grace]; `grace` marks a note caught just past the loop end (and
    // therefore placed at tick 0), so its release is timed from the loop top instead.
    int8_t heldIndex(uint8_t ch, uint8_t note) const {
        for (uint8_t i = 0; i < nHeld_; ++i)
            if (held_[i][0] == ch && held_[i][1] == note) return (int8_t)i;
        return -1;
    }
    void addHeld(uint8_t ch, uint8_t note, bool grace) {
        if (heldIndex(ch, note) >= 0) return;
        if (nHeld_ < kMaxHeld) { held_[nHeld_][0] = ch; held_[nHeld_][1] = note;
                                 held_[nHeld_][2] = grace ? 1 : 0; nHeld_++; }
    }
    void removeHeldAt(int8_t i) {
        if (i < 0 || i >= (int8_t)nHeld_) return;
        held_[i][0] = held_[nHeld_ - 1][0];
        held_[i][1] = held_[nHeld_ - 1][1];
        held_[i][2] = held_[nHeld_ - 1][2];
        nHeld_--;
    }
    void forceReleaseHeld() {
        for (uint8_t i = 0; i < nHeld_; ++i) {
            int32_t t = noteOffTick(held_[i][2] != 0);
            if (t >= 0) insertLive({ (uint16_t)t, LNoteOff, held_[i][0], held_[i][1], 0, 0 });
        }
        nHeld_ = 0;
    }

    void resetPass() {
        nHeld_ = 0; tailOpen_ = false; graceOpen_ = false;
        for (uint8_t t = 0; t < 8; ++t)
            for (uint16_t c = 0; c < 17; ++c) lastCtrl_[t][c] = INT16_MIN;
    }
    // Close the record window: freeze the anchor, start playback, open the tail.
    void startPlayback() {
        clipAnchor_ = anchor_;
        clip_.bars = bars_;
        player_.play(&clip_, anchor_);
        state_    = Playing;
        tailOpen_ = (nHeld_ > 0);
    }

    tdsp::Clock    *clk_ = nullptr;
    tdsp::MidiSink *out_ = nullptr;
    LoopPlayer      player_;
    LoopClip        clip_;

    State   state_      = Idle;
    uint8_t bars_       = 4;
    double  anchor_     = 0.0;     // record window's beat-0 (this pass)
    double  clipAnchor_ = 0.0;     // committed clip beat-0 (for resume / overdub)
    double  loopBeats_  = 0.0;
    bool    tailOpen_   = false;
    bool    graceOpen_  = false;   // brief post-close window that still accepts a late "one"

    static constexpr uint8_t kMaxHeld = 16;
    uint8_t held_[kMaxHeld][3];   // [ch, note, grace]
    uint8_t nHeld_ = 0;

    int16_t lastCtrl_[8][17];     // [type&7][channel] last recorded value (INT16_MIN = none)
};

}  // namespace tdsp
