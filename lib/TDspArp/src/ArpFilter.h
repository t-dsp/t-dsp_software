// ArpFilter — MIDI arpeggiator as a routable MidiSink.
//
// Topology
// --------
// The filter sits between tdsp::MidiRouter (upstream, fans out raw MIDI)
// and the synth sinks (downstream, receive whatever the arp emits). The
// filter is itself a MidiSink, so it registers with the MidiRouter to
// receive every event; it then forwards (bypass) or re-synthesizes
// (active) events to its own `_downstream[]` list of sinks.
//
//   MidiRouter ──► ArpFilter ──► [Dexed, MPE, Neuro, Acid, Supersaw, Chip]
//       │
//       └──► viz, clock (stay on the router so they always see raw MIDI)
//
// Bypass vs Active
// ----------------
// setEnabled(false) → pure pass-through. Every event is forwarded to
// every downstream sink verbatim. setEnabled(true) → note-on / note-off
// are CAPTURED into a held-note set; the filter emits its own arpeggiated
// note stream on clock ticks. All other events (pitch bend, pressure,
// timbre, modwheel, sustain, program change, sysex, clock, transport)
// are forwarded unconditionally so the synths still track expression
// even when the arp is running.
//
// Clock
// -----
// Drive from MidiRouter's onClock() (24 PPQN, external or internal via
// ClockSink). A separate tick(nowMicros) from loop() runs the gate-off
// scheduler so note-offs can land between clock ticks.
//
// Latch / Hold
// ------------
// latch=true: physical key-up does not remove the note from the set;
//   the set is cleared on the next fresh key-down (after a full all-off).
// hold=true: temporary latch — same semantics as sustain pedal on a
//   classic arp. Cleared when set back to false.
//
// MPE modes
// ---------
// MpeMono        : accept note-ons from every channel into a single pool;
//                  emit on `outputChannel` (default 1).
// MpeScatter     : emit each step's note on a round-robin output channel
//                  in [_scatterBase .. _scatterBase + _scatterCount - 1].
// MpeExprFollow  : MpeMono, but forward the most-recent source-channel
//                  pitchbend/pressure/timbre onto the output channel so
//                  emitted notes inherit the live gesture.
// MpePerNote     : each held source channel runs its own mini-arp and
//                  emits on that same source channel. Expression stays
//                  native. If only one note is held per channel, the
//                  arp repeats that note in octave variations.

#pragma once

#include <stdint.h>
#include <stddef.h>

#include "MidiSink.h"

namespace tdsp { class Clock; }

namespace tdsp {

class ArpFilter : public MidiSink {
public:
    static constexpr int kMaxDownstream = 8;
    static constexpr int kMaxHeldNotes  = 16;
    static constexpr int kMaxPending    = 32;    // in-flight gate-off queue
    static constexpr int kMaxScatter    = 8;
    static constexpr int kMaxSteps      = 32;

    // Pattern direction / selection. The generator consumes a sorted
    // ascending index list and picks which index plays at each step.
    enum Pattern : uint8_t {
        PatUp = 0,
        PatDown,
        PatUpDown,          // up, then down without repeating endpoints
        PatDownUp,
        PatUpDownIncl,      // repeats top & bottom
        PatDownUpIncl,
        PatPlayed,          // in the order keys were pressed
        PatPlayedRev,
        PatRandom,          // uniform random pick
        PatRandomWalk,      // adjacent-index random walk
        PatChord,           // all held notes simultaneously every step
        PatChordUp,         // alternates chord / up-note
        PatChordStab,       // chord then rest then chord
        PatConverge,        // outside-in
        PatDiverge,         // inside-out
        PatThumb,           // low pedals, rest cycle up
        PatThumbUpDown,
        PatPinky,           // high pedals, rest cycle up
        PatPinkyUpDown,
        PatAscGroup3,       // 1 2 3 / 2 3 4 / 3 4 5 ...
        PatAscGroup4,
        PatCrabWalk,        // +2, -1, +2, -1 ...
        PatStair,           // 1 1 2 2 3 3 ...
        PatSpiral,          // cycles with octave lift every pass
        PatEuclidean,       // evenly-distributed hits across stepLength
        PatCount
    };

    // Musical rate divisions expressed in 24 PPQN ticks. Short names keep
    // the switch table terse; the presets map enum names to ticks.
    enum Rate : uint8_t {
        Rate_1_1 = 0,    // whole                96
        Rate_1_2d,       // dotted half          72
        Rate_1_2,        // half                 48
        Rate_1_2t,       // half triplet         32
        Rate_1_4d,       // dotted quarter       36
        Rate_1_4,        // quarter              24
        Rate_1_4t,       // quarter triplet      16
        Rate_1_8d,       // dotted eighth        18
        Rate_1_8,        // eighth               12
        Rate_1_8t,       // eighth triplet        8
        Rate_1_16d,      // dotted sixteenth      9
        Rate_1_16,       // sixteenth             6
        Rate_1_16t,      // sixteenth triplet     4
        Rate_1_32,       // 32nd                  3
        Rate_1_32t,      // 32nd triplet          2
        Rate_Count
    };

    enum OctaveMode : uint8_t {
        OctUp = 0,
        OctDown,
        OctUpDown,
        OctRandom,
        OctCount
    };

    enum VelMode : uint8_t {
        VelFromSource = 0,   // use the velocity the key was pressed with
        VelFlat,             // constant (_fixedVelocity)
        VelAlternating,      // flat / accent
        VelRampUp,           // linearly rises across the step sequence
        VelRampDown,
        VelRandom,           // uniform [0.4, 1.0] of max
        VelAccentEveryN,     // accent every Nth step, else flat
        VelCount
    };

    enum MpeMode : uint8_t {
        MpeMono = 0,
        MpeScatter,
        MpeExprFollow,
        MpePerNote,
        MpeCount
    };

    enum Scale : uint8_t {
        ScaleOff = 0,
        ScaleChromatic,
        ScaleMajor,
        ScaleMinor,
        ScaleDorian,
        ScalePhrygian,
        ScaleLydian,
        ScaleMixolydian,
        ScaleLocrian,
        ScaleHarmonicMinor,
        ScaleMelodicMinor,
        ScaleMinorPenta,
        ScaleMajorPenta,
        ScaleBlues,
        ScaleHirajoshi,
        ScaleIn,
        ScaleYo,
        ScaleWholeTone,
        ScaleDiminished,
        ScalePhrygianDominant,
        ScaleCount
    };

    ArpFilter();

    // -------- Downstream sinks --------
    // Synth sinks register here (instead of on the MidiRouter) so the
    // arp can either forward or replace their input. Returns false on
    // null, duplicate, or capacity exhausted.
    bool addDownstream(MidiSink *sink);
    void removeDownstream(MidiSink *sink);
    void removeAllDownstream();

    // Optional clock reference for tempo-derived behaviours that need
    // to know the current BPM / beatPhase. Not load-bearing — the arp
    // runs entirely off MIDI clock pulses through onClock().
    void setClock(tdsp::Clock *clock) { _clock = clock; }

    // Called once per loop() with micros(). Drains the gate-off queue.
    // Safe to call if _clock is null.
    void tick(uint32_t nowMicros);

    // -------- MidiSink overrides (from upstream router) --------
    void onNoteOn      (uint8_t channel, uint8_t note, uint8_t velocity) override;
    void onNoteOff     (uint8_t channel, uint8_t note, uint8_t velocity) override;
    void onPitchBend   (uint8_t channel, float semitones) override;
    void onTimbre      (uint8_t channel, float value)     override;
    void onPressure    (uint8_t channel, float value)     override;
    void onModWheel    (uint8_t channel, float value)     override;
    void onSustain     (uint8_t channel, bool on)         override;
    void onProgramChange(uint8_t channel, uint8_t program) override;
    void onAllNotesOff (uint8_t channel) override;
    void onSysEx       (const uint8_t *data, size_t length, bool last) override;
    void onClock()     override;
    void onStart()     override;
    void onContinue()  override;
    void onStop()      override;

    // -------- Parameter API --------
    void setEnabled(bool on);
    bool enabled()   const { return _enabled; }

    void setPattern(Pattern p);
    Pattern pattern() const { return _pattern; }

    void setRate(Rate r);
    Rate rate() const { return _rate; }

    // ticksPerStep for the current rate, before swing skew.
    uint16_t ticksPerStep() const;

    void setGate(float gate);              // [0.05, 1.5]
    float gate() const { return _gate; }

    void setSwing(float swing);            // [0.5, 0.85], 0.5 = straight
    float swing() const { return _swing; }

    void setOctaveRange(uint8_t n);        // [1, 4]
    uint8_t octaveRange() const { return _octaveRange; }

    void setOctaveMode(OctaveMode m);
    OctaveMode octaveMode() const { return _octaveMode; }

    void setLatch(bool latch);
    bool latch() const { return _latch; }

    void setHold(bool hold);
    bool hold() const { return _hold; }

    void setVelMode(VelMode m);
    VelMode velMode() const { return _velMode; }

    void setFixedVelocity(uint8_t v);
    uint8_t fixedVelocity() const { return _fixedVelocity; }

    void setAccentVelocity(uint8_t v);
    uint8_t accentVelocity() const { return _accentVelocity; }

    void setStepMask(uint32_t m);          // 1 bit per step, LSB=step 0
    uint32_t stepMask() const { return _stepMask; }

    void setStepLength(uint8_t len);       // [1, 32]
    uint8_t stepLength() const { return _stepLength; }

    void setMpeMode(MpeMode m);
    MpeMode mpeMode() const { return _mpeMode; }

    void setOutputChannel(uint8_t ch);
    uint8_t outputChannel() const { return _outputChannel; }

    void setScatterBaseChannel(uint8_t ch);
    uint8_t scatterBaseChannel() const { return _scatterBase; }

    void setScatterCount(uint8_t n);
    uint8_t scatterCount() const { return _scatterCount; }

    void setScale(Scale s);
    Scale scale() const { return _scale; }

    void setScaleRoot(uint8_t root);       // [0, 11]
    uint8_t scaleRoot() const { return _scaleRoot; }

    void setTranspose(int8_t semi);        // [-24, +24]
    int8_t transpose() const { return _transpose; }

    void setRepeat(uint8_t n);             // [1, 8] — repeat each step N times
    uint8_t repeat() const { return _repeat; }

    // Hard reset — all held notes released, all pending gate-offs flushed.
    void panic();

    // Diagnostic: how many notes are currently held (after latch).
    uint8_t heldCount() const { return _heldCount; }

private:
    // Downstream
    MidiSink *_downstream[kMaxDownstream] = {};
    int       _downstreamCount = 0;
    tdsp::Clock *_clock = nullptr;

    // Config
    bool       _enabled       = false;   // start bypassed → safe default
    bool       _latch         = false;
    bool       _hold          = false;
    Pattern    _pattern       = PatUp;
    Rate       _rate          = Rate_1_16;
    OctaveMode _octaveMode    = OctUp;
    uint8_t    _octaveRange   = 1;
    float      _gate          = 0.5f;
    float      _swing         = 0.5f;
    VelMode    _velMode       = VelFromSource;
    uint8_t    _fixedVelocity = 100;
    uint8_t    _accentVelocity = 127;
    uint32_t   _stepMask      = 0xFFFFFFFFu;
    uint8_t    _stepLength    = 16;
    MpeMode    _mpeMode       = MpeMono;
    uint8_t    _outputChannel = 1;
    uint8_t    _scatterBase   = 2;
    uint8_t    _scatterCount  = 4;
    Scale      _scale         = ScaleOff;
    uint8_t    _scaleRoot     = 0;
    int8_t     _transpose     = 0;
    uint8_t    _repeat        = 1;

public:
    // Held-note tracking (post-latch). Added on note-on, removed on
    // note-off unless latched. The set is what the pattern generator
    // traverses. Exposed publicly so the step-sort helper in the .cpp
    // can name the type — the value itself is inert.
    struct HeldNote {
        uint8_t  note;            // raw MIDI note
        uint8_t  velocity;        // velocity at press time
        uint8_t  channel;         // source channel (for per-note MPE)
        uint32_t order;           // press-time stamp (for PatPlayed)
        bool     physicallyDown;  // true while key is physically held
    };

private:
    HeldNote _held[kMaxHeldNotes];
    uint8_t  _heldCount  = 0;
    uint32_t _orderStamp = 0;
    bool     _awaitingFreshPress = false;  // after latch-release, first new press wipes latched set

    // Per-source-channel "freshest value" trackers for MpeExprFollow.
    // When an incoming bend/pressure/timbre arrives on a source channel
    // that has a held note, we re-emit it on the output channel.
    float _lastPitchBendSemi[17] = {};   // index 1..16, 0 unused
    float _lastPressure     [17] = {};
    float _lastTimbre       [17] = {};
    // Which source channel is currently "most recent"? For ExprFollow
    // we apply that channel's expression to emitted notes.
    uint8_t _mostRecentSourceChannel = 1;

    // Clock bookkeeping
    uint32_t _clockTickCount = 0;
    uint32_t _lastStepTick   = 0xFFFFFFFFu;  // 0xFFFF… = "no step fired yet"
    uint32_t _stepIndex      = 0;
    uint32_t _repeatIndex    = 0;
    uint32_t _octavePassIndex = 0;           // tracks which octave we're on per cycle
    bool     _patternStateAscending = true;  // used by up/down & octave up/down

    // Scatter output-channel pointer (round robin)
    uint8_t _scatterPos = 0;

    // Gate-off scheduler. Each emitted note-on enqueues one of these.
    // offAtTick is in 24-PPQN clock ticks (compared against _clockTickCount);
    // tick()'s nowMicros argument is unused today but kept on the API so a
    // future micros-based gate path doesn't need a signature change.
    struct PendingOff {
        uint8_t  note;
        uint8_t  channel;
        uint32_t offAtTick;
    };
    PendingOff _pending[kMaxPending] = {};
    uint8_t    _pendingCount = 0;

    // -------- Internal helpers --------

    // Forward passthrough — raw event to every downstream sink.
    void forwardNoteOn       (uint8_t ch, uint8_t note, uint8_t vel);
    void forwardNoteOff      (uint8_t ch, uint8_t note, uint8_t vel);
    void forwardPitchBend    (uint8_t ch, float semitones);
    void forwardTimbre       (uint8_t ch, float v);
    void forwardPressure     (uint8_t ch, float v);
    void forwardModWheel     (uint8_t ch, float v);
    void forwardSustain      (uint8_t ch, bool on);
    void forwardProgramChange(uint8_t ch, uint8_t program);
    void forwardAllNotesOff  (uint8_t ch);
    void forwardSysEx        (const uint8_t *d, size_t len, bool last);
    void forwardClock();
    void forwardStart();
    void forwardContinue();
    void forwardStop();

    // Emit an arp-synthesized note-on (+schedule its off) to selected
    // downstream channel. Velocity is the final (post-modulation) value.
    // `sourceCh` is the MIDI channel the source key was pressed on, used
    // for MpeExprFollow re-emission.
    void emitNoteOn (uint8_t channel, uint8_t note, uint8_t velocity, uint32_t gateTicks, uint8_t sourceCh);
    void emitNoteOffNow(uint8_t channel, uint8_t note);

    // Released-latch cleanup: send note-offs for anything we'd been
    // "virtually holding". Called on panic + when emulated-hold clears.
    void releaseAllEmitted();

    // Held-set mutation
    void addHeld   (uint8_t ch, uint8_t note, uint8_t vel);
    void removeHeld(uint8_t ch, uint8_t note);
    void clearAllHeld();

    // Step generation — decide what to play this step.
    // Returns count of notes; fills outNotes/outVels/outChans/outSrcChans.
    // outCap must be at least 4 for chord-mode safety.
    int nextStepNotes(uint8_t outNotes[], uint8_t outVels[],
                      uint8_t outChans[], uint8_t outSrcChans[], int outCap);

    // Given a step index + sorted/played-order arrays of held notes,
    // return which indices (into the sorted array) play this step.
    // May return multiple indices (chord mode).
    int patternSelect(uint32_t step,
                      const uint8_t *sortedAsc,  uint8_t countAsc,
                      const uint8_t *played,     uint8_t countPlayed,
                      uint8_t outIndices[], int outCap);

    // Given a step index and the selected held-note index, derive the
    // octave offset for the emitted note based on OctaveMode / range.
    int8_t octaveOffset(uint32_t step) const;

    // Compute velocity for this step emission.
    uint8_t stepVelocity(uint32_t step, uint8_t sourceVel) const;

    // Apply scale quantize to an emitted MIDI note.
    uint8_t quantizeToScale(uint8_t note) const;

    // Pick output channel for this emission in MpeScatter mode.
    uint8_t pickScatterChannel();

    // Effective MIDI channel for an emission given MPE mode + source.
    uint8_t resolveOutputChannel(uint8_t sourceChannel);

    // Is step N enabled by the step-mask?
    bool stepEnabled(uint32_t step) const;

    // --- Pending-off scheduling ---
    void enqueuePendingOff(uint8_t channel, uint8_t note, uint32_t offMicros);
    void drainPendingOffs(uint32_t nowMicros);
    void clearPendingOffs();
};

}  // namespace tdsp
