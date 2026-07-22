// DrumVoice.h — optional PARALLEL OPLL drum voice (build flag: TDSP_DRUM_VOICE).
//
// The small / no-PSRAM sibling of DrumTsf.h. A dedicated OPLL (YM2413) rhythm section
// on the free mix slot 2 (S/PDIF-in dropped via TDSP_NO_SPDIF_IN) so a NON-GM melodic
// engine (Dexed / Dexed pool / Plaits / Rings / VA) gets a drum backing without a
// resident SF2 font — it runs on ANY board (no PSRAM needed). The trade-off: 5 fixed
// rhythm sounds (kick / snare / hat / tom / cymbal) vs the TSF voice's full sampled GM
// kit. Pick per board: OPLL for no-PSRAM, TSF for PSRAM. MUTUALLY EXCLUSIVE with
// TDSP_DRUM_TSF (both own mix slot 2) — main.cpp #errors if both are defined.
//
//   g_drumPlayer (ch10) -> g_drumVoiceSink -> g_drumOpll (int16 mono) -> F32 -> slot 2
//
// Cost ~9 KB, no PSRAM. Included by main.cpp AFTER outL/outR + g_player + the backend.
#pragma once
#ifdef TDSP_DRUM_VOICE
#include <AudioSynthYmfmOPLL.h>
#include "OpllSink.h"

// The output mix slot the drum voice lands on. Default 2 (S/PDIF-in dropped frees it). A build that
// puts another engine on slot 2 (e.g. HeteroPlaits) overrides this — e.g. TDSP_DRUM_SLOT=1, the slot
// the self-test tone would use but which the metronome vacates on a TDSP_METRONOME build.
#ifndef TDSP_DRUM_SLOT
#define TDSP_DRUM_SLOT 2
#endif

AudioSynthYmfmOPLL    g_drumOpll;                    // int16 mono (out 0 == out 1)
AudioConvert_I16toF32 g_drumVoiceToF32;
AudioConnection       c_dv_conv(g_drumOpll, 0, g_drumVoiceToF32, 0);
AudioConnection_F32   c_dv_outL(g_drumVoiceToF32, 0, outL, TDSP_DRUM_SLOT);
AudioConnection_F32   c_dv_outR(g_drumVoiceToF32, 0, outR, TDSP_DRUM_SLOT);
OpllSink              g_drumVoiceSink(&g_drumOpll);  // g_drumPlayer feeds this (ch10 only)

// Bring up the drum OPLL and open its mix slot. Returns true (OPLL needs no font, so it
// always succeeds) — matches drumTsfBegin()'s shape so setup() wires them the same way.
static bool drumVoiceBegin() {
    g_drumOpll.begin();
    g_drumOpll.setGain(5.5f);                        // match the OPLL backend's level
    outL.gain(TDSP_DRUM_SLOT, 0.62f); outR.gain(TDSP_DRUM_SLOT, 0.62f);   // drum bus make-up (mirrors the synth slot / TSF)
    Serial.printf("[drumvoice] parallel OPLL rhythm voice ready (ch10) -> mix slot %d\n", TDSP_DRUM_SLOT);
    return true;
}
#endif
