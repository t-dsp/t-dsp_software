// TDspArp — MIDI arpeggiator filter for T-DSP firmware.
//
// Sits between a tdsp::MidiRouter (upstream) and a set of synth MidiSink
// consumers (downstream). In bypass the filter is a transparent fan-out;
// in active mode it captures note-on/off into a held-note set and emits
// its own arpeggio stream to the downstream sinks, driven by 24-PPQN
// MIDI clock pulses fed through the router.
//
// MPE-aware: four output modes (Mono, Scatter, ExprFollow, PerNote) let
// the arp coexist with MPE controllers, including per-MPE-member-channel
// arpeggios that preserve live expression.

#pragma once

#include "ArpFilter.h"
