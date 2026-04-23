#include "BeatSequencer.h"

namespace tdsp {
namespace beats {

namespace {
constexpr float kMinBpm   = 20.0f;
constexpr float kMaxBpm   = 300.0f;
constexpr float kMaxSwing = 0.75f;
}  // namespace

BeatSequencer::BeatSequencer()
    : _bpm(120.0f),
      _swing(0.0f),
      _running(false),
      _cursor(-1),
      _nextStep(0),
      _nextStepUs(0),
      _clockSource(ClockSource::Internal),
      _externalPulseAccum(0) {
    for (int t = 0; t < kTracks; ++t) {
        _muted[t] = false;
        for (int s = 0; s < kSteps; ++s) {
            _pattern[t][s] = { false, 100 };
        }
    }
}

void BeatSequencer::setStep(int track, int step, bool on, uint8_t vel) {
    if (!inBoundsTrack(track) || !inBoundsStep(step)) return;
    _pattern[track][step].on  = on;
    _pattern[track][step].vel = (vel > 127) ? 127 : vel;
}

void BeatSequencer::toggleStep(int track, int step) {
    if (!inBoundsTrack(track) || !inBoundsStep(step)) return;
    _pattern[track][step].on = !_pattern[track][step].on;
}

bool BeatSequencer::getStepOn(int track, int step) const {
    if (!inBoundsTrack(track) || !inBoundsStep(step)) return false;
    return _pattern[track][step].on;
}

uint8_t BeatSequencer::getStepVel(int track, int step) const {
    if (!inBoundsTrack(track) || !inBoundsStep(step)) return 0;
    return _pattern[track][step].vel;
}

void BeatSequencer::clear(int track) {
    const int t0 = (track < 0) ? 0 : track;
    const int t1 = (track < 0) ? kTracks : (track + 1);
    if (t0 >= kTracks || t1 > kTracks) return;
    for (int t = t0; t < t1; ++t) {
        for (int s = 0; s < kSteps; ++s) {
            _pattern[t][s].on  = false;
            _pattern[t][s].vel = 100;
        }
    }
}

void BeatSequencer::setBpm(float bpm) {
    if (bpm < kMinBpm) bpm = kMinBpm;
    if (bpm > kMaxBpm) bpm = kMaxBpm;
    _bpm = bpm;
}

void BeatSequencer::setSwing(float swing) {
    if (swing < 0.0f)      swing = 0.0f;
    if (swing > kMaxSwing) swing = kMaxSwing;
    _swing = swing;
}

void BeatSequencer::setMute(int track, bool muted) {
    if (!inBoundsTrack(track)) return;
    _muted[track] = muted;
}

bool BeatSequencer::isMuted(int track) const {
    if (!inBoundsTrack(track)) return false;
    return _muted[track];
}

void BeatSequencer::start(uint32_t nowUs) {
    _running    = true;
    _cursor     = -1;
    _nextStep   = 0;
    _nextStepUs = nowUs;  // fire step 0 on the next tick
}

void BeatSequencer::stop() {
    _running = false;
}

// Swing math: base = 16th-note interval. Even steps (0,2,4,...) fire on
// the straight grid and "borrow" time from their following odd step to
// push it later; odd steps fire `swing * base` microseconds AFTER the
// straight grid. Net over two 16ths: still one 8th. We implement this
// by returning, for a given step-about-to-fire, the *time since the
// previous step fire*:
//
//   even step (0,2,4,...): base * (1 + swing)   [came after a swung odd step]
//   odd  step (1,3,5,...): base * (1 - swing)   [came after a straight even step]
//
// Exception: the very first step (stepIndex==0 right after start()) has
// no prior-step reference. tick() handles that by setting _nextStepUs
// = nowUs at start(), so the returned interval is irrelevant for step 0.
uint32_t BeatSequencer::stepIntervalUs(int stepIndex) const {
    const float baseUs = 60.0e6f / (_bpm * 4.0f);
    const bool  isOdd  = (stepIndex & 1) != 0;
    const float factor = isOdd ? (1.0f - _swing) : (1.0f + _swing);
    return static_cast<uint32_t>(baseUs * factor);
}

void BeatSequencer::tick(uint32_t nowUs) {
    // External clock mode: tick() is a no-op. clockPulse() drives
    // progression from MIDI real-time messages, not the sketch's
    // microsecond clock. Users still call tick() unconditionally from
    // loop() — cheap early-out keeps the API uniform.
    if (_clockSource == ClockSource::External) return;

    if (!_running) return;

    // Multiple steps may have come due since the last tick (shouldn't
    // happen in a healthy sketch but we handle it). Each iteration
    // fires one step and schedules the next.
    while (_running && (int32_t)(nowUs - _nextStepUs) >= 0) {
        fireStep(_nextStep);
        _nextStep = (_nextStep + 1) % kSteps;
        _nextStepUs += stepIntervalUs(_nextStep);
    }
}

// Internal helper: fire all track callbacks for this step, emit advance,
// advance _cursor. Does NOT touch _nextStep / _nextStepUs — caller owns
// the scheduling half.
void BeatSequencer::_fireStepImpl(int step) {
    if (_fire) {
        for (int t = 0; t < kTracks; ++t) {
            if (_muted[t]) continue;
            const Step& s = _pattern[t][step];
            if (!s.on) continue;
            const float vel = static_cast<float>(s.vel) * (1.0f / 127.0f);
            _fire(_fireCtx, t, step, vel);
        }
    }
    if (_advance) _advance(_advanceCtx, step);
    _cursor = step;
}

void BeatSequencer::setClockSource(ClockSource src) {
    if (src == _clockSource) return;
    // Switching sources stops playback — transport state across sources
    // isn't meaningful (internal scheduling time vs external pulse
    // count). Caller / MIDI Start re-engages in the new mode.
    _running            = false;
    _externalPulseAccum = 0;
    _clockSource        = src;
}

void BeatSequencer::clockPulse() {
    if (_clockSource != ClockSource::External || !_running) return;

    // 24 ppqn MIDI clock → 6 pulses per 16th-note step. Fire step 0 on
    // the FIRST pulse after Start/Continue (handled by how we seed the
    // accumulator in onMidiStart/onMidiContinue — accum=0 + first pulse
    // takes us to 1, then steps 1..6 fall naturally). Simpler: fire
    // when accum == 0 AND advance accum after.
    if (_externalPulseAccum == 0) {
        fireStep(_nextStep);
        _nextStep = (_nextStep + 1) % kSteps;
    }
    _externalPulseAccum = (_externalPulseAccum + 1) % 6;
}

void BeatSequencer::onMidiStart(uint32_t /*nowUs*/) {
    if (_clockSource != ClockSource::External) return;
    _running            = true;
    _cursor             = -1;
    _nextStep           = 0;
    _externalPulseAccum = 0;  // next clockPulse fires step 0
}

void BeatSequencer::onMidiContinue(uint32_t /*nowUs*/) {
    if (_clockSource != ClockSource::External) return;
    // Resume from wherever cursor left off. _nextStep is already the
    // next step to fire; accum keeps its partial-through-step value so
    // a mid-step Stop/Continue cycle resumes at the same sub-division.
    _running = true;
}

void BeatSequencer::onMidiStop(uint32_t /*nowUs*/) {
    if (_clockSource != ClockSource::External) return;
    _running = false;
}

// Thin public alias used by the implementations above. Keeps the
// naming convention consistent (all private helpers underscore-prefixed).
void BeatSequencer::fireStep(int step) { _fireStepImpl(step); }

}  // namespace beats
}  // namespace tdsp
