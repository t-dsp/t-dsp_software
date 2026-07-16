// MidiFilePlayer.h — non-blocking song sequencer, synth-agnostic.
//
// Plays a MidiFileEvent[] stream (from MidiSmfParser or a baked array) by
// fanning events into a tdsp::MidiSink. Because it only ever talks to the
// MidiSink interface, the SAME player drives Dexed today and ymfm (or any
// future engine) tomorrow — the synth choice lives entirely on the sink side
// (see the TDSP_SYNTH build flag in spike_midi_player).
//
// Non-blocking: tick() is called every loop() and advances the song by the
// real elapsed milliseconds, so other work (USB, control, live MIDI) keeps
// running and the song can be stopped/switched mid-play. Nothing here touches
// the audio ISR.
//
// Channel policy lives here, not in the parser: setChannelMask() decides which
// of the 16 MIDI channels are emitted. The default masks channel 10 (index 9,
// drums) so a single melodic engine (e.g. one Dexed patch) does not try to
// play kick/snare notes. A multi-timbral / drum-capable backend should call
// setChannelMask(0xFFFF) to hear everything.
#pragma once
#include <Arduino.h>
#include <math.h>
#include <stdint.h>
#include <MidiSink.h>
#include <Clock.h>
#include "MidiFileEvent.h"

namespace tdsp {

class MidiFilePlayer {
public:
    // Default channel mask: all channels except index 9 (MIDI channel 10 =
    // drums). Bit i set => channel i is emitted.
    static constexpr uint16_t kMaskNoDrums = (uint16_t)0xFFFF & ~(uint16_t)(1u << 9);
    static constexpr uint16_t kMaskAll     = 0xFFFF;

    void setSink(MidiSink *sink) { sink_ = sink; }

    // Which MIDI channels to emit (bitmask; bit i => channel index i). Applies
    // to note/CC/bend/program events. Changing it mid-song does not retro-
    // actively silence already-sounding notes.
    void setChannelMask(uint16_t mask) { chMask_ = mask; }
    uint16_t channelMask() const { return chMask_; }

    // Forward program-change events to the sink (default true). Turn off for a
    // fixed-timbre backend that should ignore the file's instrument choices.
    void setProgramChangeEnabled(bool en) { pcEnabled_ = en; }

    // Loop the stream: when the last event is reached, restart from the top
    // instead of stopping (default off). Used by the drum-groove player so a
    // one-bar pattern plays as a continuous backing under a live synth. The
    // restart is seamless (no all-notes-off) — drum one-shots simply retrigger.
    void setLooping(bool en) { loop_ = en; }
    bool isLooping() const { return loop_; }

    // Playback speed multiplier (default 1.0). >1 = faster, <1 = slower. The
    // file's timing is scaled in real time, so a groove keeps its feel while you
    // dial the tempo. Clamped to a sane musical range.
    void setTempoScale(float s) {
        if (s < 0.10f) s = 0.10f;
        if (s > 4.00f) s = 4.00f;
        speed_ = s;
    }
    float tempoScale() const { return speed_; }

    // Scale every emitted note-on velocity (default 1.0) — a level control for
    // this player's contribution without touching the shared synth/bus gain.
    // 0 => effectively silent (velocity floors to 0). Clamped 0..2.
    void setVelocityScale(float v) {
        if (v < 0.0f) v = 0.0f;
        if (v > 2.0f) v = 2.0f;
        velScale_ = v;
    }
    float velocityScale() const { return velScale_; }

    // Default pitch-bend range in semitones, used to convert the file's 14-bit bend
    // into the MidiSink's float-semitone convention (default 2). A song can override
    // it PER CHANNEL via RPN 0,0 (CC 101=0, 100=0, 6=semitones) — many do (e.g. ±12);
    // ignoring that renders bends at a fraction of their intended depth.
    void setPitchBendRange(float semis) {
        pbDefault_ = semis;
        for (int c = 0; c < 16; c++) pbRange_[c] = semis;
    }

    // Start playing ev[0..count). `ev` must outlive playback (not copied).
    void play(const MidiFileEvent *ev, uint32_t count) {
        stop();                       // release anything currently held
        if (!ev || count == 0) return;
        ev_ = ev; count_ = count; idx_ = 0;
        base_ = ev; baseCount_ = count;         // kept for seamless loop restart
        wait_ = ev_[0].deltaMs; clock_ = 0; acc_ = 0.0f;
        totalMs_ = 0; for (uint32_t i = 0; i < count; ++i) totalMs_ += ev[i].deltaMs;   // song length (for progress)
        elapsedMs_ = 0;
        synced_ = false;              // default: free-running ms engine; setSyncedMode() opts in
        for (int c = 0; c < 16; c++) {          // reset per-channel bend range + RPN state
            pbRange_[c] = pbDefault_; rpnMsb_[c] = rpnLsb_[c] = 0x7F;
        }
        playing_ = true;
    }

    // -------- Tick-synced (position-driven) mode --------
    //
    // Lock this player to the master Clock instead of the free-running ms
    // accumulator: events are dispatched by the clock's absolute BEAT position,
    // and the loop wraps on the EXACT musical length `loopBeats` (see
    // smf::initialLoopBeats). Two synced players reading the same clock cannot
    // drift apart — that is the whole point (planning/tick-sync-playback/PLAN.md).
    //
    // Call AFTER play(): play() loads the stream; this snaps the event cursor to
    // the running grid (`fmod(now, loopBeats)`), so a groove/song that JOINS a
    // running transport lands in phase (mid-loop join = mid-loop start). Starting
    // together from beat 0 → both begin at their loop's downbeat.
    //   clk       : the master clock (Conductor::clock()); nullptr disables sync.
    //   loopBeats : exact loop length in quarter-note beats (fractional OK: 6/8=3.0, 7/8=3.5).
    //   nativeBpm : the file's authored tempo — converts its ms deltas to beats.
    void setSyncedMode(tdsp::Clock *clk, double loopBeats, float nativeBpm) {
        clock_ptr_ = clk;
        loopBeats_ = (loopBeats > 0.0) ? loopBeats : 0.0;
        beatsPerMs_ = (nativeBpm > 0.0f) ? (double)nativeBpm / 60000.0 : 0.0;
        synced_ = (clk != nullptr && loopBeats_ > 0.0 && beatsPerMs_ > 0.0 &&
                   ev_ != nullptr && count_ > 0);
        if (!synced_) return;
        // Anchor to the global grid: this loop iteration's beat-0 sits at the
        // largest multiple of loopBeats <= now (so downbeats of all synced
        // players coincide). floor(now/loopBeats)*loopBeats == now - phase.
        const double now = clock_ptr_->positionBeats();
        lastMasterBeat_ = now;
        double phase = fmod(now, loopBeats_);
        if (phase < 0.0) phase = 0.0;
        loopBaseBeat_ = now - phase;
        idx_ = 0;
        evCursorBeat_ = (double)ev_[0].deltaMs * beatsPerMs_;   // beat of ev_[0]
        // Joined mid-loop: fast-forward (no dispatch) past events already gone by
        // this phase — drum one-shots simply catch the next hit; a song joins in
        // progress. (No-op when starting at the downbeat, phase ~ 0.)
        while (idx_ + 1 < count_ && evCursorBeat_ < phase) {
            ++idx_;
            evCursorBeat_ += (double)ev_[idx_].deltaMs * beatsPerMs_;
        }
    }

    // Revert to the free-running ms engine (e.g. a tempo-map song).
    void clearSyncedMode() { synced_ = false; clock_ptr_ = nullptr; }
    bool   isSynced()        const { return synced_; }
    // Debug/telemetry for @SYNCPROBE / @STATE (PLAN §9): loop length, the
    // loop-relative beat of the next event, and the master beat last seen.
    double syncLoopBeats()   const { return loopBeats_; }
    double syncCursorBeat()  const { return evCursorBeat_; }
    double syncMasterBeat()  const { return lastMasterBeat_; }

    // Which channels this player panics on stop() (bit i => MIDI channel i+1).
    // Default = all. The song player sets this to spare channel 10 so stopping /
    // restarting a song never cuts a separately-looping drum groove on ch10.
    void setPanicMask(uint16_t m) { panicMask_ = m; }

    // Stop and release every held note (sink panic). Safe to call when idle.
    void stop() {
        bool was = playing_;
        playing_ = false;
        ev_ = nullptr; count_ = 0; idx_ = 0;
        totalMs_ = 0; elapsedMs_ = 0;
        if (was && sink_) {
            if (panicMask_ == 0xFFFF) sink_->onAllNotesOff(0);   // 0 = panic all channels
            else for (uint8_t ch = 1; ch <= 16; ++ch)
                if (panicMask_ & (uint16_t)(1u << (ch - 1))) sink_->onAllNotesOff(ch);
        }
    }

    bool isPlaying() const { return playing_; }

    // One-shot "the loop just restarted" flag (for a looping player). Returns true
    // ONCE after each wrap back to the top, so a caller can align another event
    // (e.g. start a song on the groove's downbeat). Auto-clears on read.
    bool consumeLooped() { bool v = tookLoop_; tookLoop_ = false; return v; }

    // Progress 0..count (event index reached). Handy for a UI/heartbeat.
    uint32_t eventIndex() const { return idx_; }
    uint32_t eventCount() const { return count_; }

    // Total stream length in ms (sum of all deltas) = one loop period. Used to
    // derive loopBeats for a baked stream that carries no tick/PPQN metadata:
    // loopBeats = totalMs * nativeBpm / 60000 (then snap to the musical grid).
    uint32_t totalMs() const { return totalMs_; }

    // Playback position 0..1000 (permille) by ELAPSED SONG TIME (not event index —
    // events aren't evenly spaced). Drives the app's MIDI Player progress bar.
    uint16_t positionPermille() const {
        if (synced_ && loopBeats_ > 0.0) {                 // phase within the loop
            double into = lastMasterBeat_ - loopBaseBeat_;  // beats into this iteration
            if (into < 0.0) into = 0.0;
            if (into > loopBeats_) into = loopBeats_;
            return (uint16_t)(into / loopBeats_ * 1000.0);
        }
        if (totalMs_ == 0) return 0;
        uint32_t e = elapsedMs_ > totalMs_ ? totalMs_ : elapsedMs_;
        return (uint16_t)(((uint64_t)e * 1000u) / totalMs_);
    }

    // Advance the song by elapsed real time. Call every loop(). Elapsed ms are
    // scaled by speed_ into an accumulator (fractional, so slow/fast tempos don't
    // drift), matching the original behaviour exactly at speed 1.0.
    void tick() {
        if (!playing_ || !sink_) return;
        if (synced_) { tickSynced(); return; }        // position-driven path
        uint32_t real = clock_;                       // real ms since the last tick
        clock_ = 0;
        acc_ += (float)real * speed_;                 // advance in "song time"
        while (playing_ && acc_ >= (float)wait_) {
            acc_ -= (float)wait_;
            elapsedMs_ += wait_;                       // advance playback position (for progress)
            dispatch(ev_[idx_]);
            if (++idx_ >= count_) {                    // reached the end
                if (loop_ && base_ && baseCount_) {    // seamless restart from the top
                    ev_ = base_; count_ = baseCount_; idx_ = 0;
                    elapsedMs_ = 0;                    // progress bar restarts with the loop
                    wait_ = ev_[0].deltaMs;            // CARRY acc_ (don't zero) so the loop
                                                       // keeps exact time — zeroing dropped the
                                                       // remainder each bar and slowly drifted.
                    tookLoop_ = true;                  // signal the downbeat (consumeLooped)
                    break;                             // one pass per tick (runaway-safe)
                }
                playing_ = false;
                sink_->onAllNotesOff(0);
                ev_ = nullptr;
                return;
            }
            wait_ = ev_[idx_].deltaMs;
        }
    }

private:
    // Position-driven dispatch. Fire every event whose absolute scheduled beat
    // (loopBaseBeat_ + evCursorBeat_) has been reached, in order, then wrap on
    // the EXACT musical boundary (loopBaseBeat_ + loopBeats_). Because the wrap
    // is driven by the clock crossing loopBeats_ — not by ms accumulation — the
    // loop length never rounds and never drifts. Seamless: NO all-notes-off at
    // the seam (drum tails / held notes survive; authored loops release before
    // it). Catch-up safe: if loop() stalled, keeps dispatching (and wrapping)
    // until it is level with the clock, bounded by a runaway guard.
    void tickSynced() {
        if (!clock_ptr_ || loopBeats_ <= 0.0) return;
        const double songBeat = clock_ptr_->positionBeats();
        lastMasterBeat_ = songBeat;
        uint32_t guard = 0;
        const uint32_t guardMax = count_ * 2u + 32u;   // > one loop's catch-up; self-heals next tick
        while (playing_) {
            const double boundaryAbs = loopBaseBeat_ + loopBeats_;
            const bool   haveEvent   = (idx_ < count_) && (evCursorBeat_ < loopBeats_);
            const double nextAbs     = loopBaseBeat_ + evCursorBeat_;
            if (haveEvent && songBeat >= nextAbs) {
                dispatch(ev_[idx_]);
                if (++idx_ < count_)
                    evCursorBeat_ += (double)ev_[idx_].deltaMs * beatsPerMs_;
                // idx_ == count_ -> haveEvent goes false; the boundary wrap below runs.
            } else if (songBeat >= boundaryAbs) {
                idx_          = 0;                       // seam: wrap to the loop top
                loopBaseBeat_ += loopBeats_;             // exact musical length (no round)
                evCursorBeat_ = (double)ev_[0].deltaMs * beatsPerMs_;
                tookLoop_     = true;                    // downbeat signal (consumeLooped)
            } else {
                break;                                   // level with the clock; nothing due
            }
            if (++guard >= guardMax) break;              // runaway guard (pathological jump)
        }
    }

    void dispatch(const MidiFileEvent &e) {
        if (e.kind == kRest) return;
        if (!(chMask_ & (uint16_t)(1u << e.channel))) return;   // channel filtered
        const uint8_t ch = (uint8_t)(e.channel + 1);            // MidiSink is 1-based
        switch (e.kind) {
            case kNoteOn: {
                uint8_t vel = e.data2;
                if (velScale_ != 1.0f) {               // per-player level trim
                    int s = (int)((float)vel * velScale_ + 0.5f);
                    vel = (uint8_t)(s < 0 ? 0 : (s > 127 ? 127 : s));
                }
                sink_->onNoteOn(ch, e.data1, vel);
                break;
            }
            case kNoteOff:       sink_->onNoteOff(ch, e.data1, e.data2); break;
            case kProgramChange: if (pcEnabled_) sink_->onProgramChange(ch, e.data1); break;
            case kControlChange: dispatchCC(ch, e.data1, e.data2); break;
            case kPitchBend: {
                const int value = ((int)e.data2 << 7) | (int)e.data1;   // 0..16383, center 8192
                sink_->onPitchBend(ch, ((float)(value - 8192) / 8192.0f) * pbRange_[e.channel]);
                break;
            }
            case kChannelPressure: sink_->onPressure(ch, e.data1 / 127.0f); break;  // MPE Z-axis
            default: break;
        }
    }

    // Translate the handful of controllers the MidiSink models. Anything else
    // (expression, pan, ...) is dropped for now — the sink has no generic CC
    // path, and these cover the musically important cases.
    void dispatchCC(uint8_t ch, uint8_t cc, uint8_t val) {
        const uint8_t ci = (uint8_t)(ch - 1);   // 0-based channel for RPN/range state
        switch (cc) {
            case 1:   sink_->onModWheel(ch, val / 127.0f); break;   // mod wheel
            case 74:  sink_->onTimbre  (ch, val / 127.0f); break;   // MPE timbre
            case 64:  sink_->onSustain (ch, val >= 64);    break;   // sustain pedal
            // RPN: select register (101=MSB, 100=LSB); data entry 6 sets its value.
            // RPN 0,0 = pitch-bend range in semitones (a song can raise it above ±2).
            case 101: rpnMsb_[ci] = val; break;
            case 100: rpnLsb_[ci] = val; break;
            case 6:   if (rpnMsb_[ci] == 0 && rpnLsb_[ci] == 0) pbRange_[ci] = (float)val; break;
            case 120: // all sound off
            case 123: sink_->onAllNotesOff(ch); break;              // all notes off
            default:  break;
        }
    }

    MidiSink            *sink_      = nullptr;
    const MidiFileEvent *ev_        = nullptr;
    const MidiFileEvent *base_      = nullptr;         // original stream (for looping)
    uint32_t             count_     = 0;
    uint32_t             baseCount_ = 0;
    uint32_t             idx_       = 0;
    uint32_t             wait_      = 0;
    elapsedMillis        clock_;
    float                acc_       = 0.0f;            // accumulated song-time ms
    uint32_t             totalMs_   = 0;               // total song length in ms (sum of deltas) — for progress
    uint32_t             elapsedMs_ = 0;               // song-time ms dispatched so far — for progress
    float                speed_     = 1.0f;            // tempo multiplier
    float                velScale_  = 1.0f;            // note-on velocity multiplier
    bool                 loop_      = false;           // restart at end
    bool                 tookLoop_  = false;           // set on each loop wrap (consumeLooped)
    bool                 playing_   = false;
    bool                 pcEnabled_ = true;
    float                pbDefault_ = 2.0f;              // fallback bend range (no RPN in file)
    uint16_t             panicMask_ = 0xFFFF;            // 0xFFFF = panic all; else per-channel bits (setPanicMask)
    float                pbRange_[16] = {2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2};  // per-channel, RPN-settable
    uint8_t              rpnMsb_[16] = {0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F};
    uint8_t              rpnLsb_[16] = {0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F};
    uint16_t             chMask_    = kMaskNoDrums;

    // --- Tick-synced (position-driven) state (setSyncedMode) ---
    bool         synced_        = false;      // true => tickSynced(), ignore ms path
    tdsp::Clock *clock_ptr_     = nullptr;    // master position authority (not owned)
    double       loopBeats_     = 0.0;        // exact loop length (quarter-note beats)
    double       beatsPerMs_    = 0.0;        // nativeBpm/60000 — ms-delta -> beats
    double       loopBaseBeat_  = 0.0;        // absolute beat of this iteration's beat-0
    double       evCursorBeat_  = 0.0;        // loop-relative beat of ev_[idx_]
    double       lastMasterBeat_= 0.0;        // last clock position seen (progress/telemetry)
};

// Expand a legacy {deltaMs,note,velocity} note stream (the old SongEv format)
// into MidiFileEvent[] on a single channel, so baked songs generated for the
// old player still play through the new one. Returns the event count written.
// `Legacy` must have .dms / .note / .vel fields (SongEv is layout-compatible).
template <typename Legacy>
static uint32_t expandLegacyNotes(const Legacy *in, uint32_t n,
                                  MidiFileEvent *out, uint32_t maxOut,
                                  uint8_t channel = 0) {
    uint32_t no = 0;
    for (uint32_t i = 0; i < n && no < maxOut; ++i) {
        uint8_t kind = in[i].vel ? kNoteOn : (in[i].note ? kNoteOff : kRest);
        out[no++] = { in[i].dms, kind, channel, in[i].note, in[i].vel };
    }
    return no;
}

} // namespace tdsp
