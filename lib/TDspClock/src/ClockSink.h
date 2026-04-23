// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.
//
// ClockSink — glue between MidiRouter and Clock.
//
// Register this sink with a MidiRouter to drive a Clock from routed
// MIDI Real-Time messages. Lives in TDspClock rather than TDspMidi so
// the clock is the only consumer that needs to know about both.

#pragma once

#include <MidiSink.h>

#include "Clock.h"

namespace tdsp {

class ClockSink : public MidiSink {
public:
    explicit ClockSink(Clock *clock) : _clock(clock) {}

    void onClock()    override { if (_clock) _clock->onMidiTick();     }
    void onStart()    override { if (_clock) _clock->onMidiStart();    }
    void onContinue() override { if (_clock) _clock->onMidiContinue(); }
    void onStop()     override { if (_clock) _clock->onMidiStop();     }

private:
    Clock *_clock;
};

}  // namespace tdsp
