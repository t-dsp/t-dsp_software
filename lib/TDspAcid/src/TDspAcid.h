// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.
//
// TDspAcid — TB-303-style monophonic acid bass engine for the T-DSP
// platform.
//
// Umbrella header. Pulls in AcidSink, which owns mono voice logic
// (last-note priority, slide-only-when-overlapping, accent) and
// adapts tdsp::MidiSink into stock Teensy Audio primitives. Audio
// graph lives in the sketch.

#pragma once

#include "AcidSink.h"
