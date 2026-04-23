// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.
//
// TDspLooper — mono AudioStream looper for the T-DSP platform.
//
// Umbrella header. The sketch owns the sample buffer (DMAMEM for stock
// Teensy 4.1, EXTMEM once PSRAM is populated) and passes it into Looper
// at construction. See Looper.h for the state model and the thread-safety
// contract with the audio ISR.

#pragma once

#include "Looper.h"
