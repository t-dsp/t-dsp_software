// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.
//
// Conductor — the single tempo authority + transport for a synth.
//
// One Conductor owns a tdsp::Clock (the timebase) plus THE master BPM, and
// fans tempo/transport changes out to a list of ITempoFollower modules. It is
// the "master section" of the device: one BPM knob, one Start/Stop, and every
// tempo-aware module (song player, drum groove, arp, LFO, tempo-synced delay)
// stays locked because they all read from — or are driven by — this object.
//
// Two ways a module consumes the Conductor (see README.md for full recipes):
//
//   1. Tempo followers (scale-based) — register with addFollower(); get
//      onBpm()/onStart()/onStop()/onBarEdge() callbacks. PlayerFollower wraps a
//      MidiFilePlayer this way.
//
//   2. Tick consumers (24-PPQN) — the arp and anything onClock()-driven attach
//      to clock() and are fed by setTickHook() (typically wired to
//      MidiRouter::handleClock()). These need no follower registration.
//
// Design rule: setBpm() is the SINGLE write path for tempo. Keep it that way and
// the "one authority" guarantee holds no matter how many modules subscribe.
//
// Threading: like tdsp::Clock, this is a foreground object — call begin(),
// update(), setBpm() and the transport methods from loop(), not an ISR.

#pragma once

#include <stdint.h>

#include <Clock.h>

#include "TempoFollower.h"

namespace tdsp {

class Conductor {
public:
    // Up to this many modules can follow one Conductor (song, drums, arp, an
    // LFO or two...). Bump if a project needs more — it is a fixed array so
    // there is no heap churn on an embedded target.
    static constexpr uint8_t kMaxFollowers = 8;

    // Bring the master clock up in Internal mode at `bpm`. Internal mode is
    // free-running (the Clock auto-runs), so tick consumers start receiving
    // 24-PPQN immediately — a live arp works even with no song/drums playing.
    // Switch to external sync later with setSource(Clock::External).
    void begin(float bpm = Clock::kDefaultInternalBpm) {
        _clock.setSource(Clock::Internal);
        _clock.setInternalBpm(bpm);
        _bpm = _clock.internalBpm();   // read back the clamped value
    }

    // Call once per loop() with micros(). Advances the Clock (emitting catch-up
    // ticks in Internal mode) and fans bar boundaries to followers.
    void update(uint32_t nowMicros) {
        _clock.update(nowMicros);
        if (_clock.consumeBarEdge())
            for (uint8_t i = 0; i < _n; ++i) _followers[i]->onBarEdge();
    }

    // -------- Tempo (the one knob) --------

    // THE single tempo write path. Retimes the Clock and every follower.
    void setBpm(float bpm) {
        _clock.setInternalBpm(bpm);
        _bpm = _clock.internalBpm();   // clamped
        for (uint8_t i = 0; i < _n; ++i) _followers[i]->onBpm(_bpm);
    }
    float bpm() const { return _bpm; }

    void    setBeatsPerBar(uint8_t n) { _clock.setBeatsPerBar(n); }
    uint8_t beatsPerBar() const       { return _clock.beatsPerBar(); }

    // -------- Transport --------

    // Start realigns to bar 1, beat 1: the Clock's tick counter is zeroed, so
    // tick consumers (arp) and followers share a downbeat. Call this the moment
    // a groove/song begins so the whole kit locks to that downbeat.
    void start() {
        _clock.onMidiStart();
        for (uint8_t i = 0; i < _n; ++i) _followers[i]->onStart();
    }
    void stop() {
        _clock.onMidiStop();
        for (uint8_t i = 0; i < _n; ++i) _followers[i]->onStop();
    }
    void continueRun() { _clock.onMidiContinue(); }
    bool running() const { return _clock.running(); }

    // -------- Registration / access --------

    // Register a scale-based follower. It is seeded with the current BPM
    // immediately so it retimes correctly before the next setBpm().
    void addFollower(ITempoFollower *f) {
        if (!f || _n >= kMaxFollowers) return;
        _followers[_n++] = f;
        f->onBpm(_bpm);
    }

    // The shared timebase. Tick consumers (arp) call clock().tickCount() /
    // beatPhase() / etc.; an ArpFilter is wired via arp.setClock(&conductor.clock()).
    Clock &clock() { return _clock; }

    // Slave the whole kit to external MIDI clock (0xF8) instead of the internal
    // BPM. Pair with a ClockSink registered on the MidiRouter.
    void setSource(Clock::Source s) { _clock.setSource(s); }
    Clock::Source source() const    { return _clock.source(); }

    // Fan the internal 24-PPQN tick to a consumer — typically
    // MidiRouter::handleClock(), so the arp (and any onClock() sink) steps in
    // Internal mode exactly as it would from external 0xF8.
    void setTickHook(Clock::TickCallback cb, void *user) {
        _clock.setInternalTickHook(cb, user);
    }

private:
    Clock           _clock;
    float           _bpm = Clock::kDefaultInternalBpm;
    ITempoFollower *_followers[kMaxFollowers] = {};
    uint8_t         _n = 0;
};

}  // namespace tdsp
