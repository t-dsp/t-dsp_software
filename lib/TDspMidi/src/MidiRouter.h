// MidiRouter — MPE-aware MIDI fan-out.
//
// Receives raw MIDI events from any source (USBHost_t36 MIDIDevice
// callbacks, usbMIDI device-mode callbacks, UI-originated OSC notes)
// and dispatches normalized events to a fixed set of MidiSink consumers.
//
// What "normalized" means
// -----------------------
// * Pitch bend is scaled from the raw -8192..+8191 range into floating-
//   point semitones using the per-channel pitch bend range. That range
//   defaults to 48 semi (LinnStrument factory default) and auto-updates
//   on RPN 0 (Registered Parameter Number 0 = Pitch Bend Sensitivity).
// * CC#74 becomes `onTimbre(channel, 0..1)`.
// * Channel pressure becomes `onPressure(channel, 0..1)`.
// * Note-on with velocity 0 is folded into note-off before dispatch,
//   per standard MIDI running-status idiom.
// * CC#1 → onModWheel. CC#64 → onSustain. CC#120/123 → onAllNotesOff.
//
// MPE model
// ---------
// The router itself is not MPE-exclusive — it dispatches events with the
// source channel intact. MPE-aware sinks interpret channel 1 as master
// and channels 2..16 as per-note members. Legacy sinks can filter to a
// single channel (e.g. Dexed listens only to channel 1) and ignore the
// rest. Channel state (pitch bend, timbre, pressure, PB range) is cached
// per-channel so late-registered sinks can sync immediately via the
// `current*()` accessors.
//
// Performance
// -----------
// Fixed-size storage: up to kMaxSinks consumers, 16 channel state slots.
// No dynamic allocation, no locking. Hot paths are straight-line
// arithmetic plus one virtual call per registered sink. Safe to call
// from the USB host polling context in loop() — the router does not
// block or yield.

#pragma once

#include <stdint.h>
#include <stddef.h>

namespace tdsp {

class MidiSink;

class MidiRouter {
public:
    static constexpr int     kMaxSinks       = 8;
    static constexpr int     kNumChannels    = 16;

    // MPE / RPN defaults. LinnStrument ships with 48-semi pitch bend
    // range on member channels. It announces this via RPN 0 at startup;
    // we also seed the default so the first bend arriving before any
    // RPN traffic is scaled correctly.
    static constexpr float   kDefaultPitchBendRange = 48.0f;

    MidiRouter();

    // Register a sink. Returns false if the sink is null, already
    // registered, or kMaxSinks is reached. Sinks are called in
    // registration order; order is not otherwise guaranteed.
    bool addSink(MidiSink *sink);
    void removeSink(MidiSink *sink);

    // Per-channel pitch bend range in semitones. Set explicitly to
    // override the default or the auto-detected RPN 0 value. Reading
    // returns the currently-active range (auto-updated or explicit).
    // `channel` is 1-based; out-of-range is silently ignored.
    void  setPitchBendRange(uint8_t channel, float semitones);
    float pitchBendRange   (uint8_t channel) const;

    // Cached current values. Useful for a sink that registers after
    // the router has been receiving events — it can pull current state
    // on register instead of waiting for the next change. All returns
    // are 0 / 0.5 / etc. when nothing has arrived yet.
    float currentPitchBend(uint8_t channel) const;  // semitones, signed
    float currentTimbre   (uint8_t channel) const;  // 0..1
    float currentPressure (uint8_t channel) const;  // 0..1

    // Raw MIDI input — call from USBHost_t36 MIDIDevice callbacks,
    // usbMIDI device callbacks, or OSC-bridge handlers. Channels are
    // 1-based (matching USBHost_t36 convention).
    void handleNoteOn         (uint8_t channel, uint8_t note, uint8_t velocity);
    void handleNoteOff        (uint8_t channel, uint8_t note, uint8_t velocity);
    void handleControlChange  (uint8_t channel, uint8_t cc,   uint8_t value);
    void handlePitchBend      (uint8_t channel, int16_t raw);    // -8192..+8191
    void handleChannelPressure(uint8_t channel, uint8_t pressure);
    void handleProgramChange  (uint8_t channel, uint8_t program);
    void handleSysEx          (const uint8_t *data, size_t length, bool last);

    // System Real-Time (channelless). Forwarded from USB host / device
    // MIDI callbacks. Fan out to every sink — the clock sink consumes
    // them to drive tempo/phase; other sinks typically no-op.
    void handleClock();
    void handleStart();
    void handleContinue();
    void handleStop();

private:
    struct ChannelState {
        float   pitchBendRange  = kDefaultPitchBendRange;
        float   pitchBend       = 0.0f;   // semitones, last dispatched value
        float   timbre          = 0.5f;   // CC#74, centered neutral
        float   pressure        = 0.0f;   // channel pressure, 0..1

        // RPN state machine. Setting CC#101 then CC#100 selects an RPN;
        // CC#6 writes the selected RPN's MSB value. 0x7F,0x7F = null RPN
        // (nothing selected). We only act on RPN 0,0 (pitch bend range)
        // but track the selection so Data Entry to other RPNs is ignored
        // rather than misapplied.
        uint8_t rpnMsb = 0x7F;
        uint8_t rpnLsb = 0x7F;
    };

    MidiSink    *_sinks[kMaxSinks] = {};
    int          _sinkCount        = 0;
    ChannelState _channels[kNumChannels];

    // Bounds-check and return 0-based channel index, or -1 if invalid.
    static int channelIndex(uint8_t channel) {
        return (channel >= 1 && channel <= kNumChannels) ? (int)(channel - 1) : -1;
    }

    // Data Entry (CC#6) handler: applies to the currently selected RPN.
    // Called from handleControlChange.
    void applyDataEntry(int chIdx, uint8_t value);
};

}  // namespace tdsp
