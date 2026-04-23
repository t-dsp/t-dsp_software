// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.
//
// TDspClock — shared musical-time clock for all synths, loopers, and
// tempo-synced modules on the device.
//
// Umbrella header. See Clock.h for the clock itself and ClockSink.h for
// the MidiSink glue that drives the clock from routed MIDI Real-Time
// messages (0xF8 / 0xFA / 0xFB / 0xFC).

#pragma once

#include "Clock.h"
#include "ClockSink.h"
