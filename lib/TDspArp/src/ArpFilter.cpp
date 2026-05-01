// ArpFilter — implementation.

#include "ArpFilter.h"

// We only store a tdsp::Clock* pointer and never dereference it here,
// so the forward declaration in the header is sufficient — no Clock.h
// include needed on the implementation side.

namespace tdsp {

// --- Tiny RNG -------------------------------------------------------------
//
// xorshift32. Arduino's random() would do, but we want the arp deterministic
// across builds + independent of other code so unit tests (and the preset
// "Random Walk" feel) stay reproducible. Seeded from a fixed value at
// construction; `panic()` does NOT reset it so you don't hear the same
// pattern every recovery.
static uint32_t s_rng = 0x6c8a7f1du;
static inline uint32_t arpRand() {
    uint32_t x = s_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    s_rng = x;
    return x;
}
static inline uint32_t arpRandRange(uint32_t n) {
    return n ? (arpRand() % n) : 0;
}

// --- Ticks-per-step lookup table (24 PPQN) --------------------------------

static constexpr uint16_t kRateTicks[ArpFilter::Rate_Count] = {
    96,  // 1/1
    72,  // 1/2.
    48,  // 1/2
    32,  // 1/2T
    36,  // 1/4.
    24,  // 1/4
    16,  // 1/4T
    18,  // 1/8.
    12,  // 1/8
     8,  // 1/8T
     9,  // 1/16.
     6,  // 1/16
     4,  // 1/16T
     3,  // 1/32
     2,  // 1/32T
};

// --- Scale tables ---------------------------------------------------------
//
// Each scale is a 12-bit mask. Bit N set = scale degree N is "in".
// Quantize picks the nearest in-scale note to the input.

static constexpr uint16_t kScaleMasks[ArpFilter::ScaleCount] = {
    0x0000,  // Off — unused; quantizeToScale() shortcircuits first
    0x0FFF,  // Chromatic          all
    0x0AB5,  // Major              (0 2 4 5 7 9 11)
    0x05AD,  // Natural Minor      (0 2 3 5 7 8 10)
    0x06AD,  // Dorian             (0 2 3 5 7 9 10)
    0x05AB,  // Phrygian           (0 1 3 5 7 8 10)
    0x0AD5,  // Lydian             (0 2 4 6 7 9 11)
    0x06B5,  // Mixolydian         (0 2 4 5 7 9 10)
    0x056B,  // Locrian            (0 1 3 5 6 8 10)
    0x09AD,  // Harmonic Minor     (0 2 3 5 7 8 11)
    0x0AAD,  // Melodic Minor      (0 2 3 5 7 9 11)
    0x04A9,  // Minor Pentatonic   (0 3 5 7 10)
    0x0295,  // Major Pentatonic   (0 2 4 7 9)
    0x04E9,  // Blues              (0 3 5 6 7 10)
    0x018D,  // Hirajoshi          (0 2 3 7 8)
    0x01A3,  // In                 (0 1 5 7 8)
    0x02A5,  // Yo                 (0 2 5 7 9)
    0x0555,  // Whole Tone         (0 2 4 6 8 10)
    0x0B6D,  // Diminished (W-H)   (0 2 3 5 6 8 9 11)
    0x05B3,  // Phrygian Dominant  (0 1 4 5 7 8 10)
};

// ==========================================================================
// ArpFilter
// ==========================================================================

ArpFilter::ArpFilter() {
    // All defaults set via in-class initializers. Held-note storage
    // starts empty; pending-off queue empty. _orderStamp starts at 0.
}

// --- Downstream sink registration ----------------------------------------

bool ArpFilter::addDownstream(MidiSink *sink) {
    if (!sink) return false;
    if (_downstreamCount >= kMaxDownstream) return false;
    for (int i = 0; i < _downstreamCount; ++i) {
        if (_downstream[i] == sink) return false;
    }
    _downstream[_downstreamCount++] = sink;
    return true;
}

void ArpFilter::removeDownstream(MidiSink *sink) {
    if (!sink) return;
    for (int i = 0; i < _downstreamCount; ++i) {
        if (_downstream[i] == sink) {
            for (int j = i + 1; j < _downstreamCount; ++j) {
                _downstream[j - 1] = _downstream[j];
            }
            --_downstreamCount;
            _downstream[_downstreamCount] = nullptr;
            return;
        }
    }
}

void ArpFilter::removeAllDownstream() {
    for (int i = 0; i < kMaxDownstream; ++i) _downstream[i] = nullptr;
    _downstreamCount = 0;
}

// --- MidiSink overrides (from upstream router) ----------------------------
//
// Note on/off are the interesting ones: in bypass they pass through; in
// active they populate the held set AND are suppressed from downstream.
// Everything else forwards unconditionally so synths track expression.

void ArpFilter::onNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (!_enabled) {
        forwardNoteOn(channel, note, velocity);
        return;
    }
    _mostRecentSourceChannel = channel;

    // Latch behaviour: a fresh press after all keys released wipes the
    // previously-latched set. `_awaitingFreshPress` becomes true when
    // the last physical key goes up under latch.
    if ((_latch || _hold) && _awaitingFreshPress) {
        clearAllHeld();
        _awaitingFreshPress = false;
    }

    addHeld(channel, note, velocity);
    // Cold-start: if no step has fired yet AND no clock has arrived,
    // emit the first step immediately so the first key-down produces
    // an audible note (otherwise the user waits up to one step before
    // hearing anything). We gate this on "first press into an empty
    // set" so a key-down in the middle of a running pattern doesn't
    // jump forward.
    if (_heldCount == 1 && _lastStepTick == 0xFFFFFFFFu) {
        _stepIndex = 0;
        _repeatIndex = 0;
        _octavePassIndex = 0;
        _patternStateAscending = true;
        uint8_t notes[8], vels[8], chans[8], srcChans[8];
        const int n = nextStepNotes(notes, vels, chans, srcChans, 8);
        const uint16_t tps = ticksPerStep();
        for (int i = 0; i < n; ++i) {
            emitNoteOn(chans[i], notes[i], vels[i], (uint32_t)(tps * _gate), srcChans[i]);
        }
        _lastStepTick = _clockTickCount;
        _stepIndex++;
    }
}

void ArpFilter::onNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (!_enabled) {
        forwardNoteOff(channel, note, velocity);
        return;
    }
    // Mark the note as physically-released. Under latch/hold we keep it
    // in the set; once every held note is physically-released we arm
    // the "clear-on-next-press" latch semantic.
    for (uint8_t i = 0; i < _heldCount; ++i) {
        if (_held[i].note == note && _held[i].channel == channel) {
            _held[i].physicallyDown = false;
            break;
        }
    }

    if (!_latch && !_hold) {
        removeHeld(channel, note);
    } else {
        // Check if every note is physically released → arm fresh-press
        bool anyDown = false;
        for (uint8_t i = 0; i < _heldCount; ++i) {
            if (_held[i].physicallyDown) { anyDown = true; break; }
        }
        if (!anyDown) _awaitingFreshPress = true;
    }
}

void ArpFilter::onPitchBend(uint8_t channel, float semitones) {
    if (channel >= 1 && channel <= 16) _lastPitchBendSemi[channel] = semitones;
    forwardPitchBend(channel, semitones);
}

void ArpFilter::onTimbre(uint8_t channel, float value) {
    if (channel >= 1 && channel <= 16) _lastTimbre[channel] = value;
    forwardTimbre(channel, value);
}

void ArpFilter::onPressure(uint8_t channel, float value) {
    if (channel >= 1 && channel <= 16) _lastPressure[channel] = value;
    forwardPressure(channel, value);
}

void ArpFilter::onModWheel(uint8_t channel, float value) {
    forwardModWheel(channel, value);
}

void ArpFilter::onSustain(uint8_t channel, bool on) {
    forwardSustain(channel, on);
}

void ArpFilter::onProgramChange(uint8_t channel, uint8_t program) {
    forwardProgramChange(channel, program);
}

void ArpFilter::onAllNotesOff(uint8_t channel) {
    // Always clear OUR held state + pending gate-offs, even in bypass,
    // so a panic actually panics.
    clearAllHeld();
    clearPendingOffs();
    forwardAllNotesOff(channel);
}

void ArpFilter::onSysEx(const uint8_t *data, size_t length, bool last) {
    forwardSysEx(data, length, last);
}

void ArpFilter::onClock() {
    // Forward first so downstream clock-consumers (none yet, but keep
    // the contract) see the tick before we potentially emit notes on
    // the same cycle.
    forwardClock();
    _clockTickCount++;

    if (!_enabled) return;
    if (_heldCount == 0) {
        // Idle: re-arm the step clock. Without this, ticks accumulate
        // while nothing is held and onClock would then "catch up" on
        // the next key press by firing a burst of rapid-fire steps
        // until _lastStepTick re-aligns with the current tick count.
        _lastStepTick = 0xFFFFFFFFu;
        _stepIndex = 0;
        _repeatIndex = 0;
        _octavePassIndex = 0;
        _patternStateAscending = true;
        return;
    }

    // First step under a running clock: baseline _lastStepTick from
    // current count so the first downbeat lands on a tick boundary.
    if (_lastStepTick == 0xFFFFFFFFu) {
        _lastStepTick = _clockTickCount;
        // Don't emit on this exact tick — wait a full step so the
        // pattern isn't double-triggered when onNoteOn() already fired
        // the cold-start step above.
        return;
    }

    const uint16_t tps = ticksPerStep();
    if (tps == 0) return;

    // Swing: even steps fire on the beat, odd steps delayed by
    //   delay = (swing - 0.5) * 2 * tps
    // Net cadence is tps average, just uneven.
    uint32_t interval = tps;
    if (_swing > 0.5f) {
        // We alternate short/long: next step that's "on the swing"
        // (odd _stepIndex) waits the longer tps + skew; the step after
        // shortens by the same skew so the average remains tps.
        const float skew = (_swing - 0.5f) * 2.0f * (float)tps;
        if ((_stepIndex & 1u) == 1u) {
            interval = (uint32_t)((float)tps + skew);
        } else {
            int32_t shortened = (int32_t)tps - (int32_t)skew;
            if (shortened < 1) shortened = 1;
            interval = (uint32_t)shortened;
        }
    }

    if (_clockTickCount - _lastStepTick < interval) return;

    uint8_t notes[8], vels[8], chans[8], srcChans[8];
    const int n = nextStepNotes(notes, vels, chans, srcChans, 8);
    const uint32_t gateTicks = (uint32_t)((float)tps * _gate);
    for (int i = 0; i < n; ++i) {
        emitNoteOn(chans[i], notes[i], vels[i], gateTicks, srcChans[i]);
    }

    _lastStepTick += interval;
    _stepIndex++;
}

void ArpFilter::onStart() {
    _clockTickCount = 0;
    _lastStepTick = 0xFFFFFFFFu;
    _stepIndex = 0;
    _repeatIndex = 0;
    _octavePassIndex = 0;
    _patternStateAscending = true;
    forwardStart();
}

void ArpFilter::onContinue() {
    // Resume: keep counters, just allow clock ticks to drive again.
    forwardContinue();
}

void ArpFilter::onStop() {
    // Transport stopped: release anything we'd queued, let upstream
    // controllers keep sending pitch/pressure, but no new steps.
    releaseAllEmitted();
    forwardStop();
}

// --- tick() from main loop ------------------------------------------------
//
// Drains the pending-off queue based on micros() so gate timing can be
// finer than one clock tick. Called from loop() in main.cpp; caller
// passes micros(). If _clock is null we skip the micros-based path; the
// clock-tick gate-off math still fires via _pending's offAtMicros=0
// sentinel handling.

void ArpFilter::tick(uint32_t nowMicros) {
    drainPendingOffs(nowMicros);
}

// --- Parameter setters ----------------------------------------------------

void ArpFilter::setEnabled(bool on) {
    if (on == _enabled) return;
    _enabled = on;
    if (!on) {
        // Leaving active mode: flush everything we were holding in the arp's
        // name so the synths don't get stuck notes.
        releaseAllEmitted();
        clearAllHeld();
        _awaitingFreshPress = false;
        _lastStepTick = 0xFFFFFFFFu;
        _stepIndex = 0;
    } else {
        // Entering active mode: reset step position so the first held
        // key kicks off a fresh cycle.
        _stepIndex = 0;
        _repeatIndex = 0;
        _octavePassIndex = 0;
        _patternStateAscending = true;
        _lastStepTick = 0xFFFFFFFFu;
    }
}

void ArpFilter::setPattern(Pattern p) {
    if ((uint8_t)p >= (uint8_t)PatCount) return;
    _pattern = p;
    _stepIndex = 0;
    _repeatIndex = 0;
    _octavePassIndex = 0;
    _patternStateAscending = true;
}

void ArpFilter::setRate(Rate r) {
    if ((uint8_t)r >= (uint8_t)Rate_Count) return;
    _rate = r;
    _lastStepTick = 0xFFFFFFFFu;  // resync to next tick
}

uint16_t ArpFilter::ticksPerStep() const {
    return kRateTicks[(uint8_t)_rate];
}

void ArpFilter::setGate(float g) {
    if (g < 0.05f) g = 0.05f;
    if (g > 1.5f)  g = 1.5f;
    _gate = g;
}

void ArpFilter::setSwing(float s) {
    if (s < 0.5f)  s = 0.5f;
    if (s > 0.85f) s = 0.85f;
    _swing = s;
}

void ArpFilter::setOctaveRange(uint8_t n) {
    if (n < 1) n = 1;
    if (n > 4) n = 4;
    _octaveRange = n;
}

void ArpFilter::setOctaveMode(OctaveMode m) {
    if ((uint8_t)m >= (uint8_t)OctCount) return;
    _octaveMode = m;
}

void ArpFilter::setLatch(bool latch) {
    _latch = latch;
    if (!latch && !_hold) {
        // Dropping latch — release all physically-unheld notes now.
        for (int i = (int)_heldCount - 1; i >= 0; --i) {
            if (!_held[i].physicallyDown) {
                for (int j = i + 1; j < (int)_heldCount; ++j) _held[j - 1] = _held[j];
                --_heldCount;
            }
        }
        _awaitingFreshPress = false;
    }
}

void ArpFilter::setHold(bool hold) {
    _hold = hold;
    if (!hold && !_latch) {
        for (int i = (int)_heldCount - 1; i >= 0; --i) {
            if (!_held[i].physicallyDown) {
                for (int j = i + 1; j < (int)_heldCount; ++j) _held[j - 1] = _held[j];
                --_heldCount;
            }
        }
        _awaitingFreshPress = false;
    }
}

void ArpFilter::setVelMode(VelMode m) {
    if ((uint8_t)m >= (uint8_t)VelCount) return;
    _velMode = m;
}

void ArpFilter::setFixedVelocity(uint8_t v) {
    if (v > 127) v = 127;
    _fixedVelocity = v;
}

void ArpFilter::setAccentVelocity(uint8_t v) {
    if (v > 127) v = 127;
    _accentVelocity = v;
}

void ArpFilter::setStepMask(uint32_t m) { _stepMask = m; }

void ArpFilter::setStepLength(uint8_t len) {
    if (len < 1)  len = 1;
    if (len > 32) len = 32;
    _stepLength = len;
}

void ArpFilter::setMpeMode(MpeMode m) {
    if ((uint8_t)m >= (uint8_t)MpeCount) return;
    _mpeMode = m;
}

void ArpFilter::setOutputChannel(uint8_t ch) {
    if (ch < 1)  ch = 1;
    if (ch > 16) ch = 16;
    _outputChannel = ch;
}

void ArpFilter::setScatterBaseChannel(uint8_t ch) {
    if (ch < 1)  ch = 1;
    if (ch > 16) ch = 16;
    _scatterBase = ch;
}

void ArpFilter::setScatterCount(uint8_t n) {
    if (n < 1) n = 1;
    if (n > kMaxScatter) n = kMaxScatter;
    _scatterCount = n;
}

void ArpFilter::setScale(Scale s) {
    if ((uint8_t)s >= (uint8_t)ScaleCount) return;
    _scale = s;
}

void ArpFilter::setScaleRoot(uint8_t root) {
    if (root > 11) root = root % 12;
    _scaleRoot = root;
}

void ArpFilter::setTranspose(int8_t semi) {
    if (semi < -24) semi = -24;
    if (semi >  24) semi =  24;
    _transpose = semi;
}

void ArpFilter::setRepeat(uint8_t n) {
    if (n < 1) n = 1;
    if (n > 8) n = 8;
    _repeat = n;
}

void ArpFilter::panic() {
    releaseAllEmitted();
    clearAllHeld();
    clearPendingOffs();
    _awaitingFreshPress = false;
    _stepIndex = 0;
    _repeatIndex = 0;
    _lastStepTick = 0xFFFFFFFFu;
    for (int i = 0; i < _downstreamCount; ++i) {
        _downstream[i]->onAllNotesOff(0);
    }
}

// --- Forwarding helpers ---------------------------------------------------

void ArpFilter::forwardNoteOn(uint8_t ch, uint8_t note, uint8_t vel) {
    for (int i = 0; i < _downstreamCount; ++i) _downstream[i]->onNoteOn(ch, note, vel);
}
void ArpFilter::forwardNoteOff(uint8_t ch, uint8_t note, uint8_t vel) {
    for (int i = 0; i < _downstreamCount; ++i) _downstream[i]->onNoteOff(ch, note, vel);
}
void ArpFilter::forwardPitchBend(uint8_t ch, float semitones) {
    for (int i = 0; i < _downstreamCount; ++i) _downstream[i]->onPitchBend(ch, semitones);
}
void ArpFilter::forwardTimbre(uint8_t ch, float v) {
    for (int i = 0; i < _downstreamCount; ++i) _downstream[i]->onTimbre(ch, v);
}
void ArpFilter::forwardPressure(uint8_t ch, float v) {
    for (int i = 0; i < _downstreamCount; ++i) _downstream[i]->onPressure(ch, v);
}
void ArpFilter::forwardModWheel(uint8_t ch, float v) {
    for (int i = 0; i < _downstreamCount; ++i) _downstream[i]->onModWheel(ch, v);
}
void ArpFilter::forwardSustain(uint8_t ch, bool on) {
    for (int i = 0; i < _downstreamCount; ++i) _downstream[i]->onSustain(ch, on);
}
void ArpFilter::forwardProgramChange(uint8_t ch, uint8_t program) {
    for (int i = 0; i < _downstreamCount; ++i) _downstream[i]->onProgramChange(ch, program);
}
void ArpFilter::forwardAllNotesOff(uint8_t ch) {
    for (int i = 0; i < _downstreamCount; ++i) _downstream[i]->onAllNotesOff(ch);
}
void ArpFilter::forwardSysEx(const uint8_t *d, size_t len, bool last) {
    for (int i = 0; i < _downstreamCount; ++i) _downstream[i]->onSysEx(d, len, last);
}
void ArpFilter::forwardClock() {
    for (int i = 0; i < _downstreamCount; ++i) _downstream[i]->onClock();
}
void ArpFilter::forwardStart() {
    for (int i = 0; i < _downstreamCount; ++i) _downstream[i]->onStart();
}
void ArpFilter::forwardContinue() {
    for (int i = 0; i < _downstreamCount; ++i) _downstream[i]->onContinue();
}
void ArpFilter::forwardStop() {
    for (int i = 0; i < _downstreamCount; ++i) _downstream[i]->onStop();
}

// --- Emit + pending-off scheduling ---------------------------------------

void ArpFilter::emitNoteOn(uint8_t channel, uint8_t note, uint8_t velocity,
                           uint32_t gateTicks, uint8_t sourceCh) {
    // Transpose + scale quantize + range clamp on the way out.
    int n = (int)note + (int)_transpose;
    if (n < 0)   n = 0;
    if (n > 127) n = 127;
    const uint8_t finalNote = quantizeToScale((uint8_t)n);

    // ExprFollow: re-emit most recent bend/pressure/timbre on the
    // output channel before the note so the synth's per-channel state
    // is already steered when the voice starts. For PerNote the source
    // and output are the same MPE member channel, so the natural
    // upstream passthrough already carries expression — no extra work.
    if (_mpeMode == MpeExprFollow) {
        const uint8_t src = (sourceCh >= 1 && sourceCh <= 16) ? sourceCh : _mostRecentSourceChannel;
        for (int i = 0; i < _downstreamCount; ++i) {
            _downstream[i]->onPitchBend(channel, _lastPitchBendSemi[src]);
            _downstream[i]->onPressure (channel, _lastPressure     [src]);
            _downstream[i]->onTimbre   (channel, _lastTimbre       [src]);
        }
    }

    for (int i = 0; i < _downstreamCount; ++i) {
        _downstream[i]->onNoteOn(channel, finalNote, velocity);
    }

    if (gateTicks < 1) gateTicks = 1;
    enqueuePendingOff(channel, finalNote, _clockTickCount + gateTicks);
}

void ArpFilter::emitNoteOffNow(uint8_t channel, uint8_t note) {
    for (int i = 0; i < _downstreamCount; ++i) _downstream[i]->onNoteOff(channel, note, 0);
}

void ArpFilter::enqueuePendingOff(uint8_t channel, uint8_t note, uint32_t offAtTick) {
    if (_pendingCount >= kMaxPending) {
        // Queue full — force-off the oldest to avoid stuck notes.
        emitNoteOffNow(_pending[0].channel, _pending[0].note);
        for (uint8_t i = 1; i < _pendingCount; ++i) _pending[i - 1] = _pending[i];
        --_pendingCount;
    }
    _pending[_pendingCount++] = PendingOff{ note, channel, offAtTick };
}

void ArpFilter::drainPendingOffs(uint32_t /*nowMicros*/) {
    // Tick-based drain. nowMicros is unused today — gate-offs are scheduled
    // in 24-PPQN clock ticks and compared against _clockTickCount, which
    // is refreshed by onClock(). The argument is retained on the API for
    // a future micros-based path.
    uint8_t keep = 0;
    for (uint8_t i = 0; i < _pendingCount; ++i) {
        if ((int32_t)(_clockTickCount - _pending[i].offAtTick) >= 0) {
            emitNoteOffNow(_pending[i].channel, _pending[i].note);
        } else {
            _pending[keep++] = _pending[i];
        }
    }
    _pendingCount = keep;
}

void ArpFilter::clearPendingOffs() {
    _pendingCount = 0;
}

void ArpFilter::releaseAllEmitted() {
    for (uint8_t i = 0; i < _pendingCount; ++i) {
        emitNoteOffNow(_pending[i].channel, _pending[i].note);
    }
    _pendingCount = 0;
}

// --- Held-set mutation ----------------------------------------------------

void ArpFilter::addHeld(uint8_t ch, uint8_t note, uint8_t vel) {
    // De-dup: same note on same channel → bump order stamp, refresh vel.
    for (uint8_t i = 0; i < _heldCount; ++i) {
        if (_held[i].note == note && _held[i].channel == ch) {
            _held[i].velocity = vel;
            _held[i].order = ++_orderStamp;
            _held[i].physicallyDown = true;
            return;
        }
    }
    if (_heldCount >= kMaxHeldNotes) {
        // Evict the oldest note so the newest always wins.
        for (uint8_t i = 1; i < _heldCount; ++i) _held[i - 1] = _held[i];
        --_heldCount;
    }
    _held[_heldCount++] = HeldNote{ note, vel, ch, ++_orderStamp, true };
}

void ArpFilter::removeHeld(uint8_t ch, uint8_t note) {
    for (uint8_t i = 0; i < _heldCount; ++i) {
        if (_held[i].note == note && _held[i].channel == ch) {
            for (uint8_t j = i + 1; j < _heldCount; ++j) _held[j - 1] = _held[j];
            --_heldCount;
            return;
        }
    }
}

void ArpFilter::clearAllHeld() {
    _heldCount = 0;
    _orderStamp = 0;
    _awaitingFreshPress = false;
}

// --- Pattern generator ----------------------------------------------------

// Sort held-note indices: outSorted ascending by pitch, outPlayed
// ascending by press-order stamp. HeldNote is public on ArpFilter so
// this free function can name the type.
static void buildSorted(const ArpFilter::HeldNote *h, uint8_t n,
                        uint8_t outSorted[], uint8_t outPlayed[]) {
    for (uint8_t i = 0; i < n; ++i) { outSorted[i] = i; outPlayed[i] = i; }
    // Insertion sort — n ≤ 16 so this is trivial.
    for (uint8_t i = 1; i < n; ++i) {
        uint8_t k = outSorted[i];
        uint8_t j = i;
        while (j > 0 && h[outSorted[j - 1]].note > h[k].note) {
            outSorted[j] = outSorted[j - 1];
            --j;
        }
        outSorted[j] = k;
    }
    for (uint8_t i = 1; i < n; ++i) {
        uint8_t k = outPlayed[i];
        uint8_t j = i;
        while (j > 0 && h[outPlayed[j - 1]].order > h[k].order) {
            outPlayed[j] = outPlayed[j - 1];
            --j;
        }
        outPlayed[j] = k;
    }
}

int ArpFilter::patternSelect(uint32_t step,
                             const uint8_t *sortedAsc,  uint8_t cAsc,
                             const uint8_t *played,     uint8_t cPlayed,
                             uint8_t outIndices[], int outCap) {
    if (cAsc == 0 || outCap < 1) return 0;

    switch (_pattern) {
    case PatUp: {
        outIndices[0] = sortedAsc[step % cAsc];
        return 1;
    }
    case PatDown: {
        outIndices[0] = sortedAsc[(cAsc - 1) - (step % cAsc)];
        return 1;
    }
    case PatUpDown: {
        // Span is 2*cAsc - 2 for cAsc > 1; collapses to cAsc for 1.
        const uint32_t span = (cAsc <= 1) ? 1u : (2u * cAsc - 2u);
        const uint32_t pos = step % span;
        const uint32_t idx = (pos < cAsc) ? pos : (span - pos);
        outIndices[0] = sortedAsc[idx];
        return 1;
    }
    case PatDownUp: {
        const uint32_t span = (cAsc <= 1) ? 1u : (2u * cAsc - 2u);
        const uint32_t pos = step % span;
        const uint32_t idx = (pos < cAsc) ? (cAsc - 1 - pos) : (pos - (cAsc - 1));
        outIndices[0] = sortedAsc[idx];
        return 1;
    }
    case PatUpDownIncl: {
        const uint32_t span = 2u * cAsc;
        const uint32_t pos = step % span;
        const uint32_t idx = (pos < cAsc) ? pos : (span - 1 - pos);
        outIndices[0] = sortedAsc[idx];
        return 1;
    }
    case PatDownUpIncl: {
        const uint32_t span = 2u * cAsc;
        const uint32_t pos = step % span;
        const uint32_t idx = (pos < cAsc) ? (cAsc - 1 - pos) : (pos - cAsc);
        outIndices[0] = sortedAsc[idx];
        return 1;
    }
    case PatPlayed: {
        outIndices[0] = played[step % cPlayed];
        return 1;
    }
    case PatPlayedRev: {
        outIndices[0] = played[(cPlayed - 1) - (step % cPlayed)];
        return 1;
    }
    case PatRandom: {
        outIndices[0] = sortedAsc[arpRandRange(cAsc)];
        return 1;
    }
    case PatRandomWalk: {
        static uint8_t s_walk = 0;
        if (s_walk >= cAsc) s_walk = 0;
        const uint32_t r = arpRand();
        if ((r & 1u) && s_walk + 1 < cAsc) ++s_walk;
        else if (s_walk > 0)               --s_walk;
        outIndices[0] = sortedAsc[s_walk];
        return 1;
    }
    case PatChord: {
        const int n = (cAsc < outCap) ? cAsc : outCap;
        for (int i = 0; i < n; ++i) outIndices[i] = sortedAsc[i];
        return n;
    }
    case PatChordUp: {
        if (step & 1u) {
            outIndices[0] = sortedAsc[(step / 2u) % cAsc];
            return 1;
        } else {
            const int n = (cAsc < outCap) ? cAsc : outCap;
            for (int i = 0; i < n; ++i) outIndices[i] = sortedAsc[i];
            return n;
        }
    }
    case PatChordStab: {
        // chord, rest, chord, rest — rest handled by stepMask; here
        // just return the chord.
        if (step & 1u) return 0;
        const int n = (cAsc < outCap) ? cAsc : outCap;
        for (int i = 0; i < n; ++i) outIndices[i] = sortedAsc[i];
        return n;
    }
    case PatConverge: {
        // 0, c-1, 1, c-2, 2, ...
        const uint32_t pos = step % cAsc;
        uint32_t idx = (pos & 1u) ? (cAsc - 1 - (pos / 2u)) : (pos / 2u);
        if (idx >= cAsc) idx = cAsc - 1;
        outIndices[0] = sortedAsc[idx];
        return 1;
    }
    case PatDiverge: {
        // center-out: mid, mid+1, mid-1, mid+2, mid-2, ...
        const int mid = (int)cAsc / 2;
        const uint32_t pos = step % cAsc;
        int off = (int)((pos + 1) / 2);
        if ((pos & 1u) == 0u) off = -off;
        int idx = mid + off;
        if (idx < 0) idx = 0;
        if (idx >= (int)cAsc) idx = cAsc - 1;
        outIndices[0] = sortedAsc[idx];
        return 1;
    }
    case PatThumb: {
        // step: 0 → thumb (lowest); 1 → next up; 0, 1, 0, 2, 0, 3, ...
        if ((step & 1u) == 0u) {
            outIndices[0] = sortedAsc[0];
        } else {
            const uint32_t topIdx = 1u + ((step / 2u) % (cAsc > 1 ? (cAsc - 1) : 1));
            outIndices[0] = sortedAsc[topIdx % cAsc];
        }
        return 1;
    }
    case PatThumbUpDown: {
        if ((step & 1u) == 0u) {
            outIndices[0] = sortedAsc[0];
        } else {
            const uint32_t span = (cAsc <= 1) ? 1u : (2u * (cAsc - 1));
            const uint32_t pos = (step / 2u) % span;
            const uint32_t idx = (pos < (cAsc - 1)) ? (1 + pos) : (cAsc - 1 - (pos - (cAsc - 1)));
            outIndices[0] = sortedAsc[idx];
        }
        return 1;
    }
    case PatPinky: {
        if ((step & 1u) == 0u) {
            outIndices[0] = sortedAsc[cAsc - 1];
        } else {
            const uint32_t botIdx = (step / 2u) % (cAsc > 1 ? (cAsc - 1) : 1);
            outIndices[0] = sortedAsc[botIdx];
        }
        return 1;
    }
    case PatPinkyUpDown: {
        if ((step & 1u) == 0u) {
            outIndices[0] = sortedAsc[cAsc - 1];
        } else {
            const uint32_t span = (cAsc <= 1) ? 1u : (2u * (cAsc - 1));
            const uint32_t pos = (step / 2u) % span;
            const uint32_t idx = (pos < (cAsc - 1)) ? pos : (cAsc - 2 - (pos - (cAsc - 1)));
            outIndices[0] = sortedAsc[idx];
        }
        return 1;
    }
    case PatAscGroup3: {
        const uint32_t g = step / 3u;
        const uint32_t within = step % 3u;
        const uint32_t idx = (g + within) % cAsc;
        outIndices[0] = sortedAsc[idx];
        return 1;
    }
    case PatAscGroup4: {
        const uint32_t g = step / 4u;
        const uint32_t within = step % 4u;
        const uint32_t idx = (g + within) % cAsc;
        outIndices[0] = sortedAsc[idx];
        return 1;
    }
    case PatCrabWalk: {
        // Cumulative offsets of the "+2, -1" walk:
        //   step:  0  1  2  3  4  5  6  7 ...
        //   pos:   0  2  1  3  2  4  3  5 ...
        // Closed form: odd n → (n+3)/2, even n → n/2.
        const uint32_t pos = (step & 1u) ? ((step + 3u) / 2u) : (step / 2u);
        outIndices[0] = sortedAsc[pos % cAsc];
        return 1;
    }
    case PatStair: {
        const uint32_t idx = (step / 2u) % cAsc;
        outIndices[0] = sortedAsc[idx];
        return 1;
    }
    case PatSpiral: {
        // Each full pass through cAsc raises octave by one (handled by
        // octaveOffset). Otherwise same as PatUp.
        outIndices[0] = sortedAsc[step % cAsc];
        return 1;
    }
    case PatEuclidean: {
        // For Euclidean we just use the step mask as-is + return the
        // sorted-up index. The step mask provides the euclidean rhythm.
        outIndices[0] = sortedAsc[step % cAsc];
        return 1;
    }
    default:
        outIndices[0] = sortedAsc[step % cAsc];
        return 1;
    }
}

int8_t ArpFilter::octaveOffset(uint32_t step) const {
    if (_octaveRange <= 1) return 0;
    const uint32_t passesPerOctaveCycle = 1;  // one octave per "pattern pass"
    (void)passesPerOctaveCycle;
    // We approximate by dividing step by ~heldCount to find "pass index".
    const uint32_t pass = (_heldCount == 0) ? 0u : (step / _heldCount);
    switch (_octaveMode) {
    case OctUp:
        return (int8_t)((pass % _octaveRange) * 12u);
    case OctDown:
        return -(int8_t)((pass % _octaveRange) * 12u);
    case OctUpDown: {
        const uint32_t span = 2u * _octaveRange - 2u;
        if (span == 0u) return 0;
        const uint32_t p = pass % span;
        const uint32_t idx = (p < _octaveRange) ? p : (span - p);
        return (int8_t)(idx * 12u);
    }
    case OctRandom:
        return (int8_t)(arpRandRange(_octaveRange) * 12u);
    default:
        return 0;
    }
}

uint8_t ArpFilter::stepVelocity(uint32_t step, uint8_t sourceVel) const {
    switch (_velMode) {
    case VelFromSource:    return sourceVel;
    case VelFlat:          return _fixedVelocity;
    case VelAlternating:   return (step & 1u) ? _accentVelocity : _fixedVelocity;
    case VelRampUp: {
        const uint32_t pos = step % _stepLength;
        const float t = (float)pos / (float)(_stepLength > 1 ? (_stepLength - 1) : 1);
        const float v = 40.0f + t * ((float)_accentVelocity - 40.0f);
        return (uint8_t)(v + 0.5f);
    }
    case VelRampDown: {
        const uint32_t pos = step % _stepLength;
        const float t = (float)pos / (float)(_stepLength > 1 ? (_stepLength - 1) : 1);
        const float v = (float)_accentVelocity - t * ((float)_accentVelocity - 40.0f);
        return (uint8_t)(v + 0.5f);
    }
    case VelRandom: {
        const uint32_t r = arpRandRange((uint32_t)_accentVelocity - 40u);
        return (uint8_t)(40u + r);
    }
    case VelAccentEveryN: {
        // Accent on downbeat of every "step-length" window; flat otherwise.
        return ((step % _stepLength) == 0u) ? _accentVelocity : _fixedVelocity;
    }
    default:
        return sourceVel;
    }
}

uint8_t ArpFilter::quantizeToScale(uint8_t note) const {
    if (_scale == ScaleOff) return note;
    const uint16_t mask = kScaleMasks[(uint8_t)_scale];
    if (mask == 0) return note;

    // Rotate mask so bit 0 is the scale root.
    const uint8_t root = _scaleRoot;
    const int rel = ((int)note - (int)root) % 12;
    const int pc  = (rel < 0) ? (rel + 12) : rel;

    if (mask & (1u << pc)) return note;  // already in-scale

    // Search outward for the nearest in-scale pitch class.
    for (int d = 1; d <= 6; ++d) {
        const int up   = (pc + d) % 12;
        const int down = (pc + 12 - d) % 12;
        if (mask & (1u << up))   return (uint8_t)(note + d);
        if (mask & (1u << down)) return (uint8_t)(note >= (uint8_t)d ? note - d : 0);
    }
    return note;
}

uint8_t ArpFilter::pickScatterChannel() {
    const uint8_t base = _scatterBase;
    const uint8_t n    = _scatterCount;
    const uint8_t out  = (uint8_t)(base + (_scatterPos % n));
    _scatterPos = (uint8_t)((_scatterPos + 1u) % n);
    return (out > 16) ? 16 : (out < 1 ? 1 : out);
}

uint8_t ArpFilter::resolveOutputChannel(uint8_t sourceChannel) {
    switch (_mpeMode) {
    case MpeMono:        return _outputChannel;
    case MpeScatter:     return pickScatterChannel();
    case MpeExprFollow:  return _outputChannel;
    case MpePerNote:     return (sourceChannel >= 1 && sourceChannel <= 16) ? sourceChannel : _outputChannel;
    default:             return _outputChannel;
    }
}

bool ArpFilter::stepEnabled(uint32_t step) const {
    const uint32_t pos = step % _stepLength;
    if (pos >= 32u) return true;   // masks only cover first 32 steps
    return (_stepMask >> pos) & 1u;
}

// --- Step emission --------------------------------------------------------

int ArpFilter::nextStepNotes(uint8_t outNotes[], uint8_t outVels[],
                             uint8_t outChans[], uint8_t outSrcChans[], int outCap) {
    if (_heldCount == 0) return 0;

    // Respect step mask first — if this step is muted, no emission.
    if (!stepEnabled(_stepIndex)) {
        return 0;
    }

    // Handle repeat: re-emit the same step N times before advancing.
    uint32_t effectiveStep = _stepIndex;
    if (_repeat > 1) {
        effectiveStep = _stepIndex / _repeat;
    }

    // MpePerNote: emit a single step per source channel's held-note set.
    // We walk the held array and for each unique channel pick the one
    // note on that channel (or round-robin through multiple if the same
    // finger is holding a chord on its own channel, which is unusual).
    if (_mpeMode == MpePerNote) {
        int produced = 0;
        // Track which channels we've already processed this step.
        uint16_t seenChMask = 0;
        for (uint8_t i = 0; i < _heldCount && produced < outCap; ++i) {
            const uint8_t ch = _held[i].channel;
            if (ch < 1 || ch > 16) continue;
            const uint16_t bit = (uint16_t)(1u << (ch - 1));
            if (seenChMask & bit) continue;
            seenChMask |= bit;

            // Build per-channel held note list.
            uint8_t perCh[kMaxHeldNotes], perChCount = 0;
            for (uint8_t j = 0; j < _heldCount; ++j) {
                if (_held[j].channel == ch) perCh[perChCount++] = j;
            }
            if (perChCount == 0) continue;
            // Sort ascending
            for (uint8_t a = 1; a < perChCount; ++a) {
                uint8_t k = perCh[a], b = a;
                while (b > 0 && _held[perCh[b - 1]].note > _held[k].note) {
                    perCh[b] = perCh[b - 1]; --b;
                }
                perCh[b] = k;
            }
            // Pick index from pattern (use ascending only — played order
            // isn't meaningful within a single channel).
            uint8_t selIdx[8];
            const int n = patternSelect(effectiveStep, perCh, perChCount, perCh, perChCount, selIdx, 8);
            for (int s = 0; s < n && produced < outCap; ++s) {
                const HeldNote &h = _held[selIdx[s]];
                int8_t oct = octaveOffset(effectiveStep);
                int finalNote = (int)h.note + oct;
                if (finalNote < 0)   finalNote = 0;
                if (finalNote > 127) finalNote = 127;
                outNotes   [produced] = (uint8_t)finalNote;
                outVels    [produced] = stepVelocity(effectiveStep, h.velocity);
                outChans   [produced] = ch;  // PerNote: source channel is output channel
                outSrcChans[produced] = h.channel;
                ++produced;
            }
        }
        _repeatIndex = (_repeatIndex + 1) % _repeat;
        return produced;
    }

    // Standard modes — one global held-note set.
    uint8_t sortedAsc[kMaxHeldNotes];
    uint8_t played  [kMaxHeldNotes];
    buildSorted(_held, _heldCount, sortedAsc, played);

    uint8_t selIdx[8];
    const int n = patternSelect(effectiveStep, sortedAsc, _heldCount, played, _heldCount, selIdx, 8);
    if (n <= 0) return 0;

    int produced = 0;
    for (int s = 0; s < n && produced < outCap; ++s) {
        const HeldNote &h = _held[selIdx[s]];
        int8_t oct = octaveOffset(effectiveStep);
        int finalNote = (int)h.note + oct;
        if (finalNote < 0)   finalNote = 0;
        if (finalNote > 127) finalNote = 127;
        outNotes   [produced] = (uint8_t)finalNote;
        outVels    [produced] = stepVelocity(effectiveStep, h.velocity);
        outChans   [produced] = resolveOutputChannel(h.channel);
        outSrcChans[produced] = h.channel;
        ++produced;
    }
    _repeatIndex = (_repeatIndex + 1) % _repeat;
    return produced;
}

}  // namespace tdsp
