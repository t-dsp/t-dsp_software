// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.
//
// TDspTempo — umbrella include for the master-clock tempo authority.
//
//   Conductor       the single BPM/transport authority (owns a tdsp::Clock)
//   ITempoFollower  interface a module implements to follow the tempo
//   PlayerFollower  adapter that retimes a MidiFilePlayer to the master BPM
//
// See README.md for how each kind of module (song player, drum groove, arp,
// LFO, tempo-synced delay, external clock) consumes it.

#pragma once

#include "Conductor.h"
#include "TempoFollower.h"
#include "PlayerFollower.h"
