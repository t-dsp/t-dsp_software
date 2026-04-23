// TDspMidi — MIDI routing library for T-DSP firmware.
//
// Provides a MidiRouter that fans raw MIDI from any source (USBHost_t36
// MIDIDevice callbacks, usbMIDI device-mode callbacks, UI-originated
// OSC /midi/note/in messages) out to registered MidiSink consumers.
// MPE-aware: handles per-channel pitch bend range (via RPN 0), CC#74
// timbre, channel pressure, and the master/member channel distinction.
//
// This umbrella header is a convenience — individual users can include
// only the declarations they need (MidiSink.h for sink implementors,
// MidiRouter.h for the router wiring).

#pragma once

#include "MidiSink.h"
#include "MidiRouter.h"
