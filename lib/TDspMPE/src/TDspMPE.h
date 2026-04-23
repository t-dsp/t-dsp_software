// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.
//
// TDspMPE — MPE-native virtual-analog synth engine for the T-DSP platform.
//
// Umbrella header. Pulls in MpeVaSink, which owns voice allocation and
// adapts tdsp::MidiSink into stock Teensy Audio primitives (oscillators
// + envelopes) passed in at construction. The audio graph lives in the
// sketch, not the library — this keeps TDspMPE free of dependencies on
// the final output chain and makes it easy to lift into another project.

#pragma once

#include "MpeVaSink.h"
