// MidiSink — virtual interface for consumers of routed MIDI events.
//
// The MidiRouter fans incoming MIDI (from USB Host, USB Device, or OSC)
// out to every registered MidiSink. Sinks implement only the events they
// care about; the defaults are no-ops.
//
// Event conventions
// -----------------
// * Channel numbers are 1-based MIDI channels (1..16) matching USBHost_t36.
// * For MPE-aware sinks, the channel identifies the voice: channel 1 is
//   the master channel (global messages), channels 2..16 carry one note
//   each with their own continuous controllers. Non-MPE sinks can filter
//   to a single channel.
// * pitchBend is in floating-point semitones, already scaled by the
//   router using the channel's current pitch bend range (updated via
//   RPN 0 or MidiRouter::setPitchBendRange).
// * timbre and pressure are normalized 0..1.
// * modWheel and sustain are emitted for any channel that carries them;
//   an MPE sink typically ignores everything except channel 1 (master).
//
// Latency
// -------
// Events are dispatched synchronously from handleXxx() on whatever
// context invoked the router (the USB host polling loop for hardware
// MIDI, the OSC handler for UI-originated notes). Sinks must not block.

#pragma once

#include <stdint.h>
#include <stddef.h>

namespace tdsp {

class MidiSink {
public:
    virtual ~MidiSink() = default;

    // Note events. `velocity` is 1..127 for noteOn; noteOff includes the
    // release velocity (often 0 or 64). A noteOn with velocity 0 is
    // normalized by the router to noteOff before reaching here.
    virtual void onNoteOn (uint8_t channel, uint8_t note, uint8_t velocity) { (void)channel; (void)note; (void)velocity; }
    virtual void onNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) { (void)channel; (void)note; (void)velocity; }

    // Per-channel continuous controllers. Called on every change.
    virtual void onPitchBend(uint8_t channel, float semitones) { (void)channel; (void)semitones; }
    virtual void onTimbre   (uint8_t channel, float value)     { (void)channel; (void)value; }  // 0..1, CC#74
    virtual void onPressure (uint8_t channel, float value)     { (void)channel; (void)value; }  // 0..1, channel pressure

    // Channel-1-in-MPE (or any channel in legacy mode) messages.
    virtual void onModWheel     (uint8_t channel, float value)   { (void)channel; (void)value; }  // 0..1, CC#1
    virtual void onSustain      (uint8_t channel, bool    on)    { (void)channel; (void)on; }     // CC#64
    virtual void onProgramChange(uint8_t channel, uint8_t program) { (void)channel; (void)program; }

    // All-notes-off / all-sound-off (CC#123 / CC#120). Sinks should
    // release every sounding voice for the given channel. channel == 0
    // means "panic" — release everything regardless of channel.
    virtual void onAllNotesOff(uint8_t channel) { (void)channel; }

    // SysEx passthrough. For Dexed VMEM bank loads and similar. `last`
    // is true on the final chunk of a multi-packet SysEx. Sinks that
    // don't need SysEx leave the default no-op.
    virtual void onSysEx(const uint8_t *data, size_t length, bool last) { (void)data; (void)length; (void)last; }
};

}  // namespace tdsp
