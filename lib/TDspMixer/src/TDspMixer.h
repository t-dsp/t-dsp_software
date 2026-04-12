// TDspMixer — umbrella header.
//
// Include this from the project's main.cpp to pull in everything at once.
// Or include individual headers (MixerModel.h, OscDispatcher.h, etc.) for
// finer-grained dependencies.
//
// See planning/osc-mixer-foundation/01-architecture.md for the four-layer
// architecture, and ~/.claude/memory/decisions_mvp_v1_scope.md for what's
// in / out of MVP v1.

#pragma once

#include "MixerModel.h"
#include "SignalGraphBinding.h"
#include "SlipOscTransport.h"
#include "OscDispatcher.h"
#include "CodecPanel.h"
#include "MeterEngine.h"
