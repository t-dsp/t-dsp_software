// LoopPlayer.h — plays a LoopClip back, locked to the master Clock.
//
// The clip stores each event at a loop-relative 24-PPQN tick. We track the
// clock's loop-relative position rel = fmod(positionBeats - origin, loopBeats)
// and fire every event whose tick has been reached, in order, then wrap when
// rel decreases. Because the wrap is driven by the clock crossing loopBeats
// (not by ms accumulation), a loop played next to a drum groove on the same
// Clock can never drift. origin = the recording downbeat, so playback that
// starts exactly one loop after the anchor lands on the loop's beat 0.
//
// Seam handling is baked into the DATA, not the player: a note held across the
// loop end has its note-off stored at the wrapped (early) tick by the recorder,
// so replay is a plain sorted-event scan with no pending-off carry here.
#pragma once
#include <Arduino.h>
#include <math.h>
#include <MidiSink.h>
#include <Clock.h>
#include "LoopEvent.h"

namespace tdsp {

class LoopPlayer {
public:
    void begin(tdsp::Clock *clk, tdsp::MidiSink *out) { clk_ = clk; out_ = out; }

    // Start playing `clip` with its beat-0 anchored at absolute beat `origin`.
    void play(const LoopClip *clip, double origin) {
        clip_ = clip; origin_ = origin;
        cursor_ = 0; lastRel_ = -1.0; playing_ = (clip && clip->hasData());
        clearHeld();   // fresh take: no notes owed
    }

    // Stop playback. CRITICAL: the loop's output sink is SHARED with the synth's live playing (and
    // with other loops), so we must NOT panic the whole sink (onAllNotesOff) — that would cut the
    // player's own live notes. Instead we note-off ONLY the notes THIS loop currently holds, so
    // arming/stopping a loop never silences what you're playing on that synth (bad on a 1-voice OPLL).
    void stop() {
        if (playing_) releaseHeld();
        playing_ = false;
    }

    bool isPlaying() const { return playing_; }

    // Re-derive the event cursor from the current clock position. MUST be called
    // after the clip is mutated while playing (tail note-off inserts, overdub) —
    // LoopClip::insert() shifts elements, so a raw cursor index would otherwise
    // point at the wrong event for the rest of the iteration. Events at/before the
    // current position are treated as already fired (an overdubbed note first
    // sounds on the NEXT loop, which is what you want).
    void resyncCursor() {
        if (!playing_ || !clk_ || !clip_ || !clip_->hasData()) return;
        const double lb = clip_->loopTicks / 24.0;
        double rel = fmod(clk_->positionBeats() - origin_, lb);
        if (rel < 0) rel += lb;
        const double curTick = rel * 24.0;
        uint16_t c = 0;
        while (c < clip_->count && (double)clip_->ev[c].tick <= curTick) c++;
        cursor_ = c;
        lastRel_ = rel;
    }

    // 0..1000 phase within the loop (for a progress bar).
    uint16_t positionPermille() const {
        if (!playing_ || !clk_ || !clip_ || !clip_->hasData()) return 0;
        double lb = clip_->loopTicks / 24.0;
        double rel = fmod(clk_->positionBeats() - origin_, lb);
        if (rel < 0) rel += lb;
        return (uint16_t)(rel / lb * 1000.0);
    }

    // Call every loop() (after Conductor::update()).
    void tick() {
        if (!playing_ || !out_ || !clk_ || !clip_ || !clip_->hasData()) return;
        const double lb = clip_->loopTicks / 24.0;
        double rel = fmod(clk_->positionBeats() - origin_, lb);
        if (rel < 0) rel += lb;
        const double curTick = rel * 24.0;
        if (rel < lastRel_) {                    // wrapped past the seam
            while (cursor_ < clip_->count) emit(clip_->ev[cursor_++]);  // flush loop tail
            cursor_ = 0;
        }
        while (cursor_ < clip_->count && (double)clip_->ev[cursor_].tick <= curTick)
            emit(clip_->ev[cursor_++]);
        lastRel_ = rel;
    }

private:
    // Track which notes THIS loop currently has sounding, so stop() releases exactly those (never a
    // global panic on the shared sink). Bitmap: held_[channel][note>>3], bit (note&7).
    uint8_t held_[16][16] = {};
    void markHeld(uint8_t ch, uint8_t note, bool on) {
        if (ch > 15 || note > 127) return;
        if (on) held_[ch][note >> 3] |=  (uint8_t)(1u << (note & 7));
        else    held_[ch][note >> 3] &= (uint8_t)~(1u << (note & 7));
    }
    void clearHeld() { for (auto &row : held_) for (auto &b : row) b = 0; }
    void releaseHeld() {
        for (uint8_t ch = 0; ch < 16; ch++)
            for (uint8_t g = 0; g < 16; g++) {
                uint8_t bits = held_[ch][g];
                held_[ch][g] = 0;
                while (bits) {
                    uint8_t i = (uint8_t)__builtin_ctz(bits);
                    if (out_) out_->onNoteOff(ch, (uint8_t)((g << 3) | i), 0);
                    bits &= (uint8_t)(bits - 1);
                }
            }
    }
    void emit(const LoopEvent &e) {
        switch (e.type) {
            case LNoteOn:    out_->onNoteOn (e.channel, e.d1, e.d2); markHeld(e.channel, e.d1, e.d2 != 0); break;
            case LNoteOff:   out_->onNoteOff(e.channel, e.d1, e.d2); markHeld(e.channel, e.d1, false);     break;
            case LAllOff:    out_->onAllNotesOff(e.channel);         clearHeld();                          break;
            case LPitchBend: out_->onPitchBend(e.channel, (float)e.bend / 256.0f); break;
            case LTimbre:    out_->onTimbre  (e.channel, e.d2 / 255.0f); break;
            case LPressure:  out_->onPressure(e.channel, e.d2 / 255.0f); break;
            case LModWheel:  out_->onModWheel(e.channel, e.d2 / 255.0f); break;
            case LSustain:   out_->onSustain (e.channel, e.d2 != 0);  break;
            case LProgram:   out_->onProgramChange(e.channel, e.d1);  break;
            default: break;
        }
    }

    tdsp::Clock    *clk_    = nullptr;
    tdsp::MidiSink *out_    = nullptr;
    const LoopClip *clip_   = nullptr;
    double          origin_ = 0.0;
    double          lastRel_= -1.0;
    uint16_t        cursor_ = 0;
    bool            playing_= false;
};

}  // namespace tdsp
