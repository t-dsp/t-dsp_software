// BeatSequencer — 4-track × 16-step pattern sequencer.
//
// Pure logic, no audio dependency. The sketch drives it by calling
// tick(micros()) every loop() iteration; when a step's scheduled time
// has passed, the sequencer calls the registered StepFire callback
// with (track, step, velocity) for every un-muted track whose step
// is on. The callback triggers the appropriate audio voice.
//
// Timing model
// ------------
// Each 16th-note step is `60e6 / bpm / 4` microseconds. Even steps
// (0,2,4,...) fire exactly on-grid; odd steps (1,3,5,...) are delayed
// by `swing * stepIntervalUs`, which moves them toward the next
// downbeat (MPC-style swing). Swing=0 → straight 16ths. Swing≈0.33
// → triplet-style shuffle. Swing clamped to [0..0.75].
//
// Drift
// -----
// Each step's target time is computed from the *previous* step's target
// time, not from the actual fire time. A single slow tick() doesn't
// accumulate: the next step's target is still exact, so the sequencer
// catches up on the next tick. The sketch still needs to tick at a
// rate faster than the step interval (at 240 BPM that's ~62.5 ms per
// 16th — easily met by Arduino loop()).
//
// Live pattern editing
// --------------------
// setStep / toggleStep are safe to call between ticks. A step already
// fired this pass is not re-fired; a step toggled on BEFORE its fire
// time arrives will fire on schedule. There is no double-buffering —
// the caller is expected not to modify the pattern from an ISR.

#pragma once

#include <stdint.h>

namespace tdsp {
namespace beats {

class BeatSequencer {
public:
    static constexpr int kTracks = 4;
    static constexpr int kSteps  = 16;

    struct Step {
        bool    on;
        uint8_t vel;  // 0..127 (MIDI-style). Scaled to 0..1 for callback.
    };

    // StepFire: (userCtx, track 0..kTracks-1, step 0..kSteps-1, vel 0..1).
    // Called once per on-step per tick, in track-ascending order.
    using StepFire = void (*)(void* ctx, int track, int step, float velocity);

    // StepAdvance: fires ONCE per step boundary, after all track
    // callbacks for that step have been dispatched. Useful for the UI
    // to broadcast /beats/cursor without piling N copies per step.
    using StepAdvance = void (*)(void* ctx, int step);

    BeatSequencer();

    // ---- Pattern access -------------------------------------------------
    void   setStep   (int track, int step, bool on, uint8_t vel = 100);
    void   toggleStep(int track, int step);
    bool   getStepOn (int track, int step) const;
    uint8_t getStepVel(int track, int step) const;
    void   clear     (int track = -1);  // -1 clears all tracks

    // ---- Tempo / swing --------------------------------------------------
    void  setBpm  (float bpm);    // clamped to [20..300]
    float bpm     () const { return _bpm; }
    void  setSwing(float swing);  // clamped to [0..0.75]
    float swing   () const { return _swing; }

    // ---- Per-track mute --------------------------------------------------
    void setMute(int track, bool muted);
    bool isMuted(int track) const;

    // ---- Transport -------------------------------------------------------
    void start(uint32_t nowUs);  // resets cursor, schedules step 0 to fire on next tick
    void stop ();                // clears running; cursor left where it was
    bool isRunning() const { return _running; }
    int  cursor   () const { return _cursor; }  // last-fired step (-1 if none)

    // ---- Tick driver -----------------------------------------------------
    // Call from loop(). Advances internal cursor and dispatches callbacks
    // for any steps whose scheduled time has passed. Re-entrant-unsafe;
    // do not call from an ISR while the sketch is mid-tick.
    void tick(uint32_t nowUs);

    // ---- Clock source ----------------------------------------------------
    //
    // Internal: tick() schedules steps from _bpm / _swing.
    // External: tick() is a no-op; clockPulse() drives progression at
    //           24 ppqn (standard MIDI clock), so 6 pulses per 16th-note
    //           step. Swing is ignored in external mode — the sender's
    //           timing is authoritative.
    //
    // Switching source mid-play stops the sequencer; call start() again
    // in the new mode (or rely on MIDI Start 0xFA to do it) to resume.
    enum class ClockSource : uint8_t { Internal, External };
    void        setClockSource(ClockSource src);
    ClockSource clockSource() const { return _clockSource; }

    // Single MIDI clock pulse (F8). Safe to call from the USB host
    // polling context; pure arithmetic. Does nothing when running=false
    // or when clock source is Internal.
    void clockPulse();

    // MIDI Start (FA) / Continue (FB) / Stop (FC). Only take effect
    // when clock source is External; Internal mode ignores them so a
    // DAW sending transport while we're on internal timing doesn't
    // hijack playback. All three use the caller's nowUs for logging
    // timestamps and for future internal-to-external handoff.
    void onMidiStart   (uint32_t nowUs);
    void onMidiContinue(uint32_t nowUs);
    void onMidiStop    (uint32_t nowUs);

    // ---- Callback registration ------------------------------------------
    void setOnStepFire   (StepFire    fn, void* ctx) { _fire    = fn; _fireCtx    = ctx; }
    void setOnStepAdvance(StepAdvance fn, void* ctx) { _advance = fn; _advanceCtx = ctx; }

private:
    Step      _pattern[kTracks][kSteps];
    bool      _muted[kTracks];

    float     _bpm;
    float     _swing;

    bool      _running;
    int       _cursor;        // last-fired step index (-1 before first fire)
    int       _nextStep;      // index of the step the sequencer is waiting to fire
    uint32_t  _nextStepUs;    // monotonic time at which _nextStep fires (internal mode)

    ClockSource _clockSource;
    uint8_t     _externalPulseAccum;  // 0..5, fires step when it wraps past 6 (24 ppqn / 4 = 6 per 16th)

    StepFire    _fire        = nullptr;
    void*       _fireCtx     = nullptr;
    StepAdvance _advance     = nullptr;
    void*       _advanceCtx  = nullptr;

    // Duration of the step that is ABOUT to fire (index `stepIndex`).
    // Even steps get the base 16th-note interval; odd steps get the
    // base interval shifted by swing (the ADVANCE from odd back onto
    // the grid also shortens, but we express that by giving the even
    // step a longer interval when swing>0 — equivalent transformation,
    // simpler bookkeeping). See the .cpp file for the math.
    uint32_t stepIntervalUs(int stepIndex) const;

    // Fire all track callbacks for a step + emit the advance callback
    // + advance _cursor. Does NOT touch _nextStep / _nextStepUs; the
    // caller (tick for internal, clockPulse for external) owns that.
    void     fireStep(int step);
    void     _fireStepImpl(int step);

    static bool   inBoundsTrack(int t)        { return t >= 0 && t < kTracks; }
    static bool   inBoundsStep (int s)        { return s >= 0 && s < kSteps; }
};

}  // namespace beats
}  // namespace tdsp
