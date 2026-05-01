// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.

#include "Clock.h"

namespace tdsp {

// BPM estimator smoothing. Each tick's interval is folded into the
// running estimate via a one-pole IIR:
//
//     _measuredIntervalUs = (old * (N-1) + new) / N
//
// N=8 is a reasonable balance: a sequencer that lands each tick ~1%
// late from jitter settles within a beat, but a genuine tempo change
// still follows within a few beats. If this turns out to feel
// sluggish we can switch to a shorter window or a median-of-3.
static constexpr uint32_t kBpmSmoothingN = 8;

Clock::Clock() = default;

// -------- Feed --------

void Clock::onMidiTick() {
    if (_source != External) return;     // internal source owns its ticks
    if (!_running) return;               // ticks before Start are noise
    // Use micros() through the caller's lens — we captured `nowMicros`
    // at the top of update() above the MIDI drain loop. BUT onMidiTick
    // can arrive between update() calls; capturing micros() directly
    // here is fine (it's an IRQ-safe single-register read on Teensy).
    // We deliberately avoid a macro/Arduino dep in the header, so just
    // approximate "now" via _lastUpdateMicros when available.
    //
    // In practice: the sketch calls update() first, then drains MIDI,
    // then update() again on the next loop(). For BPM estimation the
    // exact capture time within a loop() iteration matters less than
    // having a consistent reference, so we use the last-known micros
    // advanced by the interval since the previous tick. If no update()
    // has run yet, fall back to monotonic counter via _lastTickMicros
    // + 1 tick's worth of time.
    //
    // Rather than pull in Arduino.h here, we let main.cpp route ticks
    // through an overload that takes `nowMicros`. To keep the public
    // surface narrow we compute the stamp as "whatever the last
    // update() recorded" — jitter vs exact micros() is sub-ms and
    // well inside tick-to-tick variance.
    advanceOneTick(_lastUpdateMicros);
}

void Clock::onMidiStart() {
    // "Start" resets to the top. Per MIDI spec, the first 0xF8 after
    // Start falls on beat 1 — so we zero the tick count and mark
    // running.
    _tickCount        = 0;
    _running          = true;
    _beatEdge         = true;      // downbeat of bar 1
    _barEdge          = true;
    _lastTickMicros   = 0;
    _internalAccumUs  = 0;
}

void Clock::onMidiContinue() {
    // Resume from wherever Stop left us.
    _running        = true;
    _lastTickMicros = 0;
    _internalAccumUs = 0;
}

void Clock::onMidiStop() {
    _running = false;
    // Leave _tickCount intact so onContinue() picks up where we left
    // off, and so UI can still read the final position.
}

// -------- Foreground tick --------

void Clock::update(uint32_t nowMicros) {
    _lastUpdateMicros = nowMicros;

    if (_source == Internal) {
        // Drive ticks from local time. When not running, don't accrue
        // — Start/Continue will re-seed _lastTickMicros.
        if (!_running) return;

        const uint32_t perTick = microsPerTick(_internalBpm);
        if (_lastTickMicros == 0) {
            _lastTickMicros = nowMicros;
            return;
        }
        uint32_t due = nowMicros - _lastTickMicros;
        // Fire catch-up ticks if loop() was late. Cap at one bar's
        // worth so a debug pause doesn't flood the tick stream.
        const uint32_t maxCatchup = perTick * kPpqn * kDefaultBeatsPerBar;
        if (due > maxCatchup) due = maxCatchup;
        while (due >= perTick) {
            _lastTickMicros += perTick;
            due             -= perTick;
            advanceOneTick(_lastTickMicros);
            // Fan out to any downstream 24-PPQN consumers (arp, beats,
            // etc.). Only fired on the Internal path — external ticks
            // reach those consumers via the same MIDI dispatch that
            // fed onMidiTick().
            if (_internalTickCb) _internalTickCb(_internalTickUser);
        }
        // _measuredIntervalUs tracks the authoritative tempo; keep it
        // in sync with internal so bpm() queries return the setter's
        // value rather than a stale external number.
        _measuredIntervalUs = perTick;
        _bpm                = _internalBpm;
        return;
    }

    // External: watchdog. If we've been running and haven't seen a
    // tick in kExternalStallUs, assume the upstream sequencer died
    // and flip to stopped so downstream consumers stop locking to a
    // phantom grid. We keep _bpm pinned at its last value as a hint.
    if (_running && _lastTickMicros != 0) {
        if ((uint32_t)(nowMicros - _lastTickMicros) > kExternalStallUs) {
            _running = false;
        }
    }
}

// -------- Configuration --------

void Clock::setSource(Source s) {
    if (_source == s) return;
    _source = s;
    // Preserve tick count on source switch so a UI toggle doesn't
    // yank the phase out from under an already-running pattern. But
    // reset the interval scaffolding since the old source's stamps
    // are meaningless under the new regime.
    _lastTickMicros     = 0;
    _internalAccumUs    = 0;
    _measuredIntervalUs = 0;
    if (s == Internal) {
        // Internal mode can be "always on" so LFOs keep ticking without
        // requiring an external Start. Start running immediately; the
        // first update() seeds the phase.
        _running = true;
    } else {
        // External: wait for real transport messages.
        _running = false;
    }
}

void Clock::setInternalBpm(float bpm) {
    if (bpm < 20.0f)  bpm = 20.0f;
    if (bpm > 300.0f) bpm = 300.0f;
    _internalBpm = bpm;
    if (_source == Internal) {
        _measuredIntervalUs = microsPerTick(bpm);
        _bpm                = bpm;
    }
}

void Clock::setBeatsPerBar(uint8_t n) {
    if (n < 1)  n = 1;
    if (n > 16) n = 16;
    _beatsPerBar = n;
}

// -------- Phase queries --------

float Clock::beatPhase() const {
    const uint32_t tickInBeat = _tickCount % kPpqn;
    // Base phase from the integer tick count.
    float phase = (float)tickInBeat / (float)kPpqn;
    // Interpolate within the current tick using wall-clock time so
    // consumers that read beatPhase() between ticks see a smooth
    // ramp rather than a staircase.
    if (_measuredIntervalUs > 0 && _lastTickMicros != 0 && _running) {
        const uint32_t since = _lastUpdateMicros - _lastTickMicros;
        float frac = (float)since / (float)_measuredIntervalUs;
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        phase += frac * (1.0f / (float)kPpqn);
        if (phase >= 1.0f) phase = 0.999999f;   // don't roll past 1
    }
    return phase;
}

float Clock::barPhase() const {
    const uint32_t ticksPerBar = (uint32_t)kPpqn * _beatsPerBar;
    if (ticksPerBar == 0) return 0.0f;
    const uint32_t tickInBar = _tickCount % ticksPerBar;
    float phase = (float)tickInBar / (float)ticksPerBar;
    if (_measuredIntervalUs > 0 && _lastTickMicros != 0 && _running) {
        const uint32_t since = _lastUpdateMicros - _lastTickMicros;
        float frac = (float)since / (float)_measuredIntervalUs;
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        phase += frac * (1.0f / (float)ticksPerBar);
        if (phase >= 1.0f) phase = 0.999999f;
    }
    return phase;
}

bool Clock::consumeBeatEdge() {
    const bool e = _beatEdge;
    _beatEdge = false;
    return e;
}

bool Clock::consumeBarEdge() {
    const bool e = _barEdge;
    _barEdge = false;
    return e;
}

// -------- Internals --------

void Clock::advanceOneTick(uint32_t nowMicros) {
    // Update BPM estimate from the interval since the previous tick.
    // Skip the first tick after Start (no previous stamp).
    if (_lastTickMicros != 0) {
        const uint32_t interval = nowMicros - _lastTickMicros;
        // Reject wildly implausible intervals — either <2 ms (> 1250 BPM)
        // or > 250 ms (< 20 BPM) — as transient noise. This protects
        // the estimator from buffered-MIDI bursts at connect time.
        if (interval >= 2'000 && interval <= 250'000) {
            if (_measuredIntervalUs == 0) {
                _measuredIntervalUs = interval;
            } else {
                // IIR: new = (old * (N-1) + sample) / N
                const uint64_t acc =
                    (uint64_t)_measuredIntervalUs * (kBpmSmoothingN - 1) + interval;
                _measuredIntervalUs = (uint32_t)(acc / kBpmSmoothingN);
            }
            updateBpmFromInterval();
        }
    }
    _lastTickMicros = nowMicros;

    ++_tickCount;

    // Edge detection. A tick that lands on a beat boundary fires
    // beatEdge; a tick that lands on bar 1, beat 0 additionally
    // fires barEdge. The Start handler pre-arms both for tick 0.
    if ((_tickCount % kPpqn) == 0) {
        _beatEdge = true;
        const uint32_t ticksPerBar = (uint32_t)kPpqn * _beatsPerBar;
        if (ticksPerBar > 0 && (_tickCount % ticksPerBar) == 0) {
            _barEdge = true;
        }
    }
}

void Clock::updateBpmFromInterval() {
    if (_measuredIntervalUs == 0) return;
    // BPM = 60_000_000 micros-per-minute / (PPQN * micros-per-tick)
    const float denom = (float)kPpqn * (float)_measuredIntervalUs;
    if (denom <= 0.0f) return;
    _bpm = 60'000'000.0f / denom;
}

uint32_t Clock::microsPerTick(float bpm) {
    if (bpm < 1.0f) bpm = 1.0f;
    // 60_000_000 us/min / (bpm * 24 ppqn)
    return (uint32_t)(60'000'000.0f / (bpm * (float)kPpqn));
}

}  // namespace tdsp
