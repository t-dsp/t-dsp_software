// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.
//
// Clock — shared musical-time reference for the whole device.
//
// One clock drives every tempo-aware consumer (loopers, LFOs,
// arpeggiators, delays). Consumers pull state — nothing is wired
// through templated callbacks. A foreground `update(nowMicros)` call
// from loop() both advances the internal source (when selected) and
// detects loss of an external source.
//
// Sources
// -------
//   External   slaved to incoming MIDI Timing Clock (0xF8) messages,
//              typically delivered via the MidiRouter + ClockSink.
//              BPM is estimated from the interval between ticks.
//   Internal   free-running at `setInternalBpm(...)`. Each `update()`
//              emits as many 24-PPQN ticks as the elapsed time covers.
//
// PPQN = 24 per quarter note (the MIDI Clock standard). A beat is
// one quarter note; a bar is `beatsPerBar()` beats (default 4, i.e. 4/4).
//
// Edge consumption
// ----------------
// consumeBeatEdge() / consumeBarEdge() are foreground-polled latches.
// A single beat or bar boundary is reported exactly once — read it
// from loop() and act on it (e.g. Looper.record() on next beat).
//
// Thread safety
// -------------
// onMidi* and update() may race. We keep all state on a single thread
// for MVP: call everything from loop(). That matches how the existing
// MidiRouter is used — USB host MIDI is drained in loop() via
// `g_midiIn.read()`, not from an ISR. If a future path delivers MIDI
// clock from an ISR, the volatile counters here are single-word reads
// that Cortex-M7 makes atomic, but the tick-interval math would want
// __disable_irq guards — revisit then.

#pragma once

#include <stdint.h>

namespace tdsp {

class Clock {
public:
    static constexpr uint8_t kPpqn = 24;             // MIDI Timing Clock
    static constexpr float   kDefaultInternalBpm = 120.0f;
    static constexpr uint8_t kDefaultBeatsPerBar = 4;

    enum Source : uint8_t {
        External = 0,   // slaved to MIDI 0xF8 ticks
        Internal = 1,   // driven by update(nowMicros)
    };

    Clock();

    // -------- Feed (from MidiSink / router) --------
    //
    // Each call is one MIDI Real-Time byte. onMidiTick() is 0xF8, the
    // rest match the name. When Source==Internal, onMidiTick() is
    // ignored (internal time is authoritative). Transport messages
    // are always honored so an external sequencer's Start/Stop still
    // resets position even if we're running internal tempo — this
    // mirrors how most DAW+drum-machine pairings behave.
    void onMidiTick();
    void onMidiStart();
    void onMidiContinue();
    void onMidiStop();

    // -------- Foreground tick --------
    //
    // Call once per loop() with micros(). Drives the internal source
    // when selected; detects stalled external clock when selected.
    void update(uint32_t nowMicros);

    // -------- Configuration --------
    void   setSource(Source s);
    Source source() const { return _source; }

    // Internal-mode tempo. Clamped to [20, 300] — anything outside
    // that range is almost always a units bug rather than a real BPM.
    void  setInternalBpm(float bpm);
    float internalBpm() const { return _internalBpm; }

    void    setBeatsPerBar(uint8_t n);
    uint8_t beatsPerBar() const { return _beatsPerBar; }

    // -------- Transport --------
    bool running() const { return _running; }

    // -------- Tempo / phase --------
    //
    // bpm() returns the LAST-MEASURED (external) or currently-set
    // (internal) tempo. On cold boot or after a long external-clock
    // stall, returns the last good value rather than snapping to 0.
    float    bpm() const       { return _bpm; }
    uint32_t tickCount() const { return _tickCount; }    // since start
    uint32_t beatCount() const { return _tickCount / kPpqn; }
    uint8_t  beatInBar() const { return (uint8_t)(beatCount() % _beatsPerBar); }

    // 0..1 phase within the current beat (quarter note). Interpolates
    // between ticks using wall-clock time, so LFOs / visualizers get
    // a smooth ramp rather than a 24-step staircase.
    float beatPhase() const;

    // 0..1 phase within the current bar.
    float barPhase() const;

    // Edge latches. Each returns `true` at most once per beat/bar,
    // clearing on read. Designed for a single foreground consumer;
    // if multiple consumers need the same edge, build a fan-out.
    bool consumeBeatEdge();
    bool consumeBarEdge();

private:
    // Core state
    Source   _source       = External;
    bool     _running      = false;
    uint8_t  _beatsPerBar  = kDefaultBeatsPerBar;
    float    _internalBpm  = kDefaultInternalBpm;
    float    _bpm          = kDefaultInternalBpm;

    // Tick counter since last Start. Wraps at 2^32 ticks (~49 days
    // at 120 BPM) — irrelevant in practice. Phase queries use `%`.
    uint32_t _tickCount    = 0;

    // Wall-clock stamps for BPM estimation + internal-source accrual.
    // Units are micros(). 0 means "not yet captured".
    uint32_t _lastTickMicros       = 0;    // when the last tick fired
    uint32_t _measuredIntervalUs   = 0;    // smoothed micros-per-tick
    uint32_t _internalAccumUs      = 0;    // phase for internal source
    uint32_t _lastUpdateMicros     = 0;

    // External stall detection. If we've seen clock and then haven't
    // seen one for this long, treat it as stopped so phase/BPM don't
    // lie indefinitely. 500 ms is generous at our slowest supported
    // tempo (20 BPM = 125 ms per tick).
    static constexpr uint32_t kExternalStallUs = 500'000;

    // Edge latches. Set by the tick path, consumed by foreground.
    bool _beatEdge = false;
    bool _barEdge  = false;

    // Advance the counter by one tick and update edge latches. The
    // `nowMicros` argument is the time at which the tick "happened"
    // (for external: time of 0xF8 receipt; for internal: the synthetic
    // target time). Used to refresh the BPM estimator.
    void advanceOneTick(uint32_t nowMicros);

    // Refresh _bpm from _measuredIntervalUs. Factored so internal
    // and external paths share the formula.
    void updateBpmFromInterval();

    // Compute micros-per-tick from a BPM value. Inline guard against
    // BPM <= 0 which would divide by zero.
    static uint32_t microsPerTick(float bpm);
};

}  // namespace tdsp
