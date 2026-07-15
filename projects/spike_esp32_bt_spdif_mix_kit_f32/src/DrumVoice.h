// DrumVoice.h — optional PARALLEL drum voice (build flag: TDSP_DRUM_VOICE).
//
// A dedicated OPLL (YM2413) used ONLY for its channel-10 rhythm section, mixed into
// the DAC ALONGSIDE the melodic synth. This is what lets a NON-GM engine (Dexed /
// Plaits / Rings / VA) get a drum backing: those engines have no channel-10 drum map,
// so instead of routing the groove THROUGH them, we route it to this separate voice.
//
//   g_drumPlayer (masked to ch10)  ->  g_drumVoiceSink  ->  g_drumOpll (rhythm)  --\
//   melodic synth  ->  outL/outR slot 3  (untouched)  -----------------------------+--> finalL/R -> DAC
//
// Cost: one small OPLL instance (few KB state, int16, NO PSRAM) + one convert + one
// F32 mixer stage. Zero cost when the flag is off. GM builds (TSF/SF2/OPL3/OPLL)
// already render ch10 themselves and should NOT set this flag (it would double the
// drums). Included by main.cpp AFTER outL/outR, tdmOut, g_player, and the backend.
#pragma once
#ifdef TDSP_DRUM_VOICE
#include <AudioSynthYmfmOPLL.h>
#include "OpllSink.h"

AudioSynthYmfmOPLL    g_drumOpll;                    // int16 mono (out 0 == out 1)
AudioConvert_I16toF32 g_drumVoiceToF32;              // int16 -> F32 bridge
AudioConnection       c_dv_conv(g_drumOpll, 0, g_drumVoiceToF32, 0);
// Final stage: sum the main mix bus (outL/outR) + the drum voice, then to the DAC.
// (main.cpp omits its own outL/outR -> tdmOut connections when this flag is set.)
AudioMixer4_F32       g_finalL, g_finalR;
AudioConnection_F32   c_dv_mainL(outL, 0, g_finalL, 0);
AudioConnection_F32   c_dv_mainR(outR, 0, g_finalR, 0);
AudioConnection_F32   c_dv_drumL(g_drumVoiceToF32, 0, g_finalL, 1);
AudioConnection_F32   c_dv_drumR(g_drumVoiceToF32, 0, g_finalR, 1);
AudioConnection_F32   c_dv_outL(g_finalL, 0, tdmOut, 0);
AudioConnection_F32   c_dv_outR(g_finalR, 0, tdmOut, 1);
OpllSink              g_drumVoiceSink(&g_drumOpll);  // g_drumPlayer feeds this (ch10 only)

static void drumVoiceBegin() {
    g_drumOpll.begin();
    g_drumOpll.setGain(5.5f);                        // match the OPLL backend's level
    g_finalL.gain(0, 1.0f); g_finalL.gain(1, 1.0f);  // main bus + drum voice at unity
    g_finalR.gain(0, 1.0f); g_finalR.gain(1, 1.0f);
    Serial.println("[drumvoice] parallel OPLL rhythm voice ready (ch10) — drums under any synth");
}
#endif
