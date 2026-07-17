// TDspMidiLoop.h — umbrella include for the beat-aware MIDI loop recorder.
//
// One MidiLooper per voice: register it downstream of that voice's arp to
// capture the baked note stream, give it the voice's synth sink for playback.
// See MidiLooper.h for the recording/looping model.
#pragma once
#include "LoopEvent.h"
#include "LoopPlayer.h"
#include "MidiLooper.h"
#include "LoopClipIo.h"   // clip <-> wire-bytes codec for the note editor (@RECDUMP/@RECLOAD)
