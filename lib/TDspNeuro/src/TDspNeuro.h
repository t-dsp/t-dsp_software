// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.
//
// TDspNeuro — monophonic "neuro / reese" bass engine for the T-DSP platform.
//
// Umbrella header. Pulls in NeuroSink, which owns the mono voice
// allocation (last-note priority, legato portamento) and adapts
// tdsp::MidiSink into stock Teensy Audio primitives passed in at
// construction. The audio graph lives in the sketch.

#pragma once

#include "NeuroSink.h"
