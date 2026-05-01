// T-DSP F32 Audio Shield — Phase 2.
//
// Fully F32 / 24-bit / 48 kHz port of the t-dsp_tac5212_audio_shield_adaptor
// project. Phase 2 layers an F32 DSP graph on top of the Phase 1 USB-passthrough
// path:
//
//   * AudioMixer4_F32 stereo input bus  — slot 0 = USB L/R, slots 1..3 reserved
//                                          for synths (Phase 3) and other inputs
//   * AudioEffectGain_F32 main fader     — driven by /main/st/mix/faderL/R
//   * AudioFilterBiquad_F32 high-shelf   — main-bus tone shaping, boots in
//                                          pass-through; engaged via /proc/shelf/...
//
// Out of scope (later phases):
//   * F32 limiter / waveshaper          — Phase 3 (TAC5212 on-chip limiter
//                                          already available via lib/TAC5212)
//   * Adc6140Panel + 4-ch XLR preamp    — later
//   * PDM mic                            — later
//   * Synths (Dexed via I16->F32 bridge) — Phase 3
//   * Arpeggiator + MIDI router          — Phase 4
//
// Audio graph (F32 throughout, no int16 conversion at any stage):
//
//   AudioInputUSB_F32 [0] ── (ch1) ──> AudioMixer4_F32 mixL [0]
//                     [1] ── (ch2) ──> AudioMixer4_F32 mixR [0]
//                                          |                  |
//                                          v                  v
//                              AudioEffectGain_F32     AudioEffectGain_F32
//                                  mainAmpL                 mainAmpR
//                                          |                  |
//                                          v                  v
//                              AudioFilterBiquad_F32  AudioFilterBiquad_F32
//                                  procShelfL              procShelfR
//                                          |                  |
//                                          v                  v
//                              AudioOutputTDM_F32 [slot 0] [slot 1]
//                                          v
//                              SAI1 TX -> TAC5212 DAC -> OUT1/OUT2 jack
//                                                              |
//                                                  (3.5mm cable loopback)
//                                                              v
//                              SAI1 RX <- TAC5212 ADC
//                                          |
//                              AudioInputTDM_F32 [slot 0] [slot 1]
//                                          |                  |
//                                          v                  v
//                              AudioOutputUSB_F32 [0]      [1]
//
// Hardware notes (carry-over from project_shdnz_pin and
// project_6140_buffer_contention memories):
//   * Pin 35 is SHDNZ for BOTH the TAC5212 and the on-board TLV320ADC6140.
//     We toggle it LOW->HIGH exactly once at boot. After that, codec
//     drivers must use SW_RESET via I2C only — never re-toggle the pin.
//   * The 6140's '125 buffer is always-on and would fight the TAC5212 on
//     TDM slots 0..3 if left awake. We send it a SW_RESET early in setup()
//     so it returns to POR sleep state and tri-states its TDM output.

#include <Arduino.h>
#include <Wire.h>

#include <OpenAudio_ArduinoLibrary.h>
#include <AudioInputUSB_F32.h>
#include <AudioOutputUSB_F32.h>

#include <OSCMessage.h>
#include <OSCBundle.h>

#include <USBHost_t36.h>

#include <Audio.h>
#include <SD.h>
#include <synth_dexed.h>
#include <TeensyVariablePlayback.h>

#include <TAC5212.h>
#include <TDspMixer.h>
#include <TDspMidi.h>
#include <TDspClock.h>
#include <TDspArp.h>
#include <TDspMPE.h>

#include "Tac5212Panel.h"
#include "DexedSink.h"
#include "DexedVoiceBank.h"
#include "osc/X32FaderLaw.h"
#include "synth/SynthSlot.h"
#include "synth/DexedSlot.h"
#include "synth/SilentSlot.h"
#include "synth/MultisampleSlot.h"
#include "synth/MpeSlot.h"
#include "synth/NeuroSlot.h"
#include "synth/SupersawSlot.h"
#include "synth/ChipSlot.h"
#include "synth/SynthSwitcher.h"

#include <TDspNeuro.h>
#include <TDspSupersaw.h>
#include <TDspChip.h>

// ============================================================================
// Hardware constants
// ============================================================================

// Shared SHDNZ pin for the TAC5212 + on-board TLV320ADC6140. Active-low.
constexpr int TAC5212_EN_PIN = 35;

// I2C addresses on this board.
constexpr uint8_t TAC5212_I2C_ADDRESS  = 0x51;
constexpr uint8_t ADC6140_I2C_ADDRESS  = 0x4C;

// ============================================================================
// Audio graph — F32 throughout
// ============================================================================

// Endpoints
AudioInputUSB_F32       usbIn;
AudioOutputUSB_F32      usbOut;
AudioOutputTDM_F32      tdmOut;
AudioInputTDM_F32       tdmIn;

// Stereo input mix bus. Slot 0 = USB host (channels 1/2). Slots 1..3 are
// reserved — Phase 3 lands synths there via the I16->F32 bridge, Phase 2.5
// will land Line / PDM / XLR sources once those pieces of the graph come
// online.
AudioMixer4_F32         mixL;
AudioMixer4_F32         mixR;

// Main fader. AudioEffectGain_F32 is mono; one instance per side. The
// /main/st/mix/on toggle multiplies into this gain (on=0 -> 0.0f).
AudioEffectGain_F32     mainAmpL;
AudioEffectGain_F32     mainAmpR;

// Host-volume amp — driven by the Windows playback slider (USB Audio
// Class FU 0x31, polled via AudioInputUSB_F32::volume() / mute()).
// Single audio block of latency vs WASAPI shared-mode soft-ramp.
// /main/st/hostvol/enable bypasses the stage by holding it at 1.0.
AudioEffectGain_F32     hostvolAmpL;
AudioEffectGain_F32     hostvolAmpR;

// Main-bus high-shelf processing. Boots in passthrough (numStagesUsed = 0,
// doBiquad = false) — applyProcShelf() loads coefficients and calls begin()
// the first time the dev surface engages it. While disabled, audio flows
// through unmodified.
AudioFilterBiquad_F32   procShelfL;
AudioFilterBiquad_F32   procShelfR;

// --- Dexed FM synth (int16 engine, dual-mono into the F32 mix bus) -----
//
// synth_dexed is a 6-op FM engine descended from MSFA / DX7. It outputs
// mono int16; we bridge to F32 with AudioConvert_I16toF32, scale through
// an F32 gain stage, and fan into mixL[1] AND mixR[1] for centered
// dual-mono. Sample rate is passed as AUDIO_SAMPLE_RATE_EXACT so Dexed's
// internal tuning aligns with Teensy Audio's actual update rate.
//
// 8 voices is the production default; gives polyphony without
// straining the int16 memory pool. /synth/dexed/voice picks bank/voice
// from the bundled Ritchie voice banks (10 banks × 32 voices, packed
// in dexed_banks_data.h).
AudioSynthDexed         g_dexed((uint8_t)8, (uint16_t)AUDIO_SAMPLE_RATE_EXACT);
AudioConvert_I16toF32   g_dexedToF32;
AudioEffectGain_F32     g_dexedGain;
DexedSink               g_dexedSink(&g_dexed);

// --- Sampler slot 1 (multisample SD streaming) ---
//
// 8-voice polyphonic sampler reading 48 kHz / 16-bit / stereo WAVs from
// the Teensy 4.1's BUILTIN_SDCARD via newdigate/teensy-variable-playback.
// Stage 1 mixers sum 4 voices each (envelopes feed into them); stage 2
// (mix[2]) sums the two stage-1 outputs to mono per side. Then bridge
// to F32 and route through the per-slot gain into the synth sub-mixer.
//
// File-scope so AudioConnection ctors can reference them at static init.
// MultisampleSlot stores pointers to these via the AudioContext struct.
AudioPlaySdResmp        g_smPlayer[8];
AudioEffectEnvelope     g_smEnvL[8];
AudioEffectEnvelope     g_smEnvR[8];
AudioMixer4             g_smMixL[3];
AudioMixer4             g_smMixR[3];
AudioConvert_I16toF32   g_smToF32L;
AudioConvert_I16toF32   g_smToF32R;
AudioEffectGain_F32     g_smGainL;
AudioEffectGain_F32     g_smGainR;

// --- MPE VA slot 3 (MPE-aware virtual-analog) ---
//
// 4-voice MPE-aware virtual-analog engine via lib/TDspMPE/MpeVaSink:
// each voice is `AudioSynthWaveform → AudioFilterStateVariable →
// AudioEffectEnvelope`. Per-voice CC#74 routes to that voice's filter
// cutoff, so each MPE finger steers brightness on its own note only.
// Mono int16 voice path; one I16->F32 bridge fans dual-mono into
// sub-mixer slot 3 (same shape Dexed uses).
//
// File-scope so AudioConnection ctors can reference them at static
// init. MpeSlot wraps these via the AudioContext + the MpeVaSink the
// library already provides.
AudioSynthWaveform        g_mpeOsc[4];
AudioFilterStateVariable  g_mpeFilter[4];
AudioEffectEnvelope       g_mpeEnv[4];
AudioMixer4               g_mpeMix;          // sums 4 voices to mono
AudioConvert_I16toF32     g_mpeToF32;
AudioEffectGain_F32       g_mpeGain;         // mono, post-bridge

// MpeVaSink voice ports — pointers into the file-scope arrays above.
// The sink itself is stateless w.r.t. ownership (it just steers the
// nodes), so the slot adapter holds a reference.
MpeVaSink::VoicePorts g_mpeVoices[4] = {
    { &g_mpeOsc[0], &g_mpeEnv[0], &g_mpeFilter[0] },
    { &g_mpeOsc[1], &g_mpeEnv[1], &g_mpeFilter[1] },
    { &g_mpeOsc[2], &g_mpeEnv[2], &g_mpeFilter[2] },
    { &g_mpeOsc[3], &g_mpeEnv[3], &g_mpeFilter[3] },
};
MpeVaSink g_mpeSink(g_mpeVoices, 4);

// --- Slot 4 (Neuro) ---
//
// Reese / neuro bass voice via lib/TDspNeuro/NeuroSink. Mono engine —
// 3 saws (osc1/2/3) + sub sine (oscSub) feed a 4-input mixer, into a
// state-variable LP filter, into an ADSR envelope. Last-note-priority
// note stack with portamento; LFO can target cutoff/pitch/amp. Mono
// int16 chain; one I16->F32 bridge fans dual-mono into sub-mixer slot
// 4 (g_synthSumLB[0] / g_synthSumRB[0]) — same shape Dexed/MPE/Supersaw
// use.
//
// Stink chain (multiband destruction) lives in the legacy project but
// is intentionally out of scope for this slot rebuild — see
// planning/synth-slot-rebuild/.
//
// NeuroSink owns the note stack, portamento glide, and parameter
// state; NeuroSlot just gates the per-slot F32 gain on active/on.
AudioSynthWaveform        g_neuroOsc1, g_neuroOsc2, g_neuroOsc3;  // saws
AudioSynthWaveform        g_neuroOscSub;                          // sub sine
AudioMixer4               g_neuroVoiceMix;
AudioFilterStateVariable  g_neuroFilt;
AudioEffectEnvelope       g_neuroEnv;
AudioConvert_I16toF32     g_neuroToF32;
AudioEffectGain_F32       g_neuroGain;       // mono, post-bridge

NeuroSink::VoicePorts g_neuroPorts = {
    &g_neuroOsc1, &g_neuroOsc2, &g_neuroOsc3, &g_neuroOscSub,
    &g_neuroVoiceMix, &g_neuroFilt, &g_neuroEnv,
};
NeuroSink g_neuroSink(g_neuroPorts);

// --- Slot 6 (Supersaw) ---
//
// JP-8000-style 5-voice detuned saw lead via lib/TDspSupersaw/SupersawSink.
// 5 saws (osc1..5) -> mixAB (sums osc1..4) -> mixFinal (mixAB + osc5) ->
// state-variable filter (LP) -> ADSR envelope. Mono int16 chain; one
// I16->F32 bridge fans dual-mono into sub-mixer slot 6 (g_synthSumLB[2] /
// g_synthSumRB[2]) — same shape Dexed/MPE use.
//
// SupersawSink owns the note stack, portamento glide, and parameter
// state; SupersawSlot just gates the per-slot F32 gain on active/on.
AudioSynthWaveform        g_supersawO1, g_supersawO2, g_supersawO3, g_supersawO4, g_supersawO5;
AudioMixer4               g_supersawMixAB;
AudioMixer4               g_supersawMixFinal;
AudioFilterStateVariable  g_supersawFilt;
AudioEffectEnvelope       g_supersawEnv;
AudioConvert_I16toF32     g_supersawToF32;
AudioEffectGain_F32       g_supersawGain;     // mono, post-bridge

SupersawSink::VoicePorts g_supersawPorts = {
    &g_supersawO1, &g_supersawO2, &g_supersawO3, &g_supersawO4, &g_supersawO5,
    &g_supersawMixAB, &g_supersawMixFinal,
    &g_supersawFilt, &g_supersawEnv,
};
SupersawSink g_supersawSink(g_supersawPorts);

// --- Slot 7 (Chip) ---
//
// NES/Gameboy-style monophonic chiptune voice via lib/TDspChip/ChipSink.
// Two pulse oscillators (duty-cycle selectable) + triangle sub + pink
// noise feed a 4-input AudioMixer4, which then runs through an ADSR
// envelope. Mono int16 chain; one I16->F32 bridge fans dual-mono into
// sub-mixer slot 7 (g_synthSumLB[3] / g_synthSumRB[3]) — same shape
// Dexed/MPE/Supersaw use.
//
// ChipSink owns the note stack, last-note priority, voicing/arpeggio
// state, and the ADSR shape; ChipSlot just gates the per-slot F32 gain
// on active/on.
AudioSynthWaveform        g_chipPulse1, g_chipPulse2;
AudioSynthWaveform        g_chipTriangle;
AudioSynthNoisePink       g_chipNoise;
AudioMixer4               g_chipMix;
AudioEffectEnvelope       g_chipEnv;
AudioConvert_I16toF32     g_chipToF32;
AudioEffectGain_F32       g_chipGain;       // mono, post-bridge

ChipSink::VoicePorts g_chipPorts = {
    &g_chipPulse1, &g_chipPulse2, &g_chipTriangle, &g_chipNoise,
    &g_chipMix,    &g_chipEnv,
};
ChipSink g_chipSink(g_chipPorts);

// --- Synth sub-mixer (8-slot chain) ---
//
// Sums per-slot outputs (slot 0 = Dexed, slot 1 = sampler, slots 2..7 =
// future engines) into the synth bus. AudioMixer4_F32 has 4 inputs, so
// 8 slots need a chain: two stage-A mixers (slots 0..3 and 4..7) feed
// a stage-B mixer that produces the final sum per channel. Slot
// active/inactive is gated by each engine's own gain stage, so all
// sub-mixer input gains stay at 1.0.
//
// Wiring:
//   slot 0..3 outputs -> g_synthSumLA[0..3] / g_synthSumRA[0..3]
//   slot 4..7 outputs -> g_synthSumLB[0..3] / g_synthSumRB[0..3]
//   g_synthSumLA  -> g_synthSumLC[0]   (final stage)
//   g_synthSumLB  -> g_synthSumLC[1]
//   g_synthSumLC  -> g_synthBusL       (existing path through main mix)
AudioMixer4_F32         g_synthSumLA;
AudioMixer4_F32         g_synthSumLB;
AudioMixer4_F32         g_synthSumLC;
AudioMixer4_F32         g_synthSumRA;
AudioMixer4_F32         g_synthSumRB;
AudioMixer4_F32         g_synthSumRC;

// Synth-bus group fader — sits downstream of the sub-mixer and upstream
// of the main mix bus. The dev surface's Mixer view shows a single
// "synth" strip backed by /synth/bus/volume + /synth/bus/on; this stage
// is what those leaves drive.
AudioEffectGain_F32     g_synthBusL;
AudioEffectGain_F32     g_synthBusR;

// USB chans 0/1 (F32) -> per-side mix bus slot 0.
AudioConnection_F32  c_usbL_mix    (usbIn, 0, mixL, 0);
AudioConnection_F32  c_usbR_mix    (usbIn, 1, mixR, 0);

// Mix -> main fader -> hostvol -> shelf -> TDM out.
AudioConnection_F32  c_mixL_main    (mixL,        0, mainAmpL,    0);
AudioConnection_F32  c_mixR_main    (mixR,        0, mainAmpR,    0);
AudioConnection_F32  c_mainL_host   (mainAmpL,    0, hostvolAmpL, 0);
AudioConnection_F32  c_mainR_host   (mainAmpR,    0, hostvolAmpR, 0);
AudioConnection_F32  c_hostL_shelf  (hostvolAmpL, 0, procShelfL,  0);
AudioConnection_F32  c_hostR_shelf  (hostvolAmpR, 0, procShelfR,  0);
AudioConnection_F32  c_shelfL_tdm   (procShelfL,  0, tdmOut,      0);
AudioConnection_F32  c_shelfR_tdm   (procShelfR,  0, tdmOut,      1);

// TDM slots 0/1 -> USB chans 0/1 (F32). Capture path stays direct in
// Phase 2 — adding meter taps / processing on capture is later work.
AudioConnection_F32  c_cap_L (tdmIn, 0, usbOut, 0);
AudioConnection_F32  c_cap_R (tdmIn, 1, usbOut, 1);

// PDM mics (TAC5212 channels 3 & 4 — TX_CH3_SLOT -> slot 2, TX_CH4_SLOT
// -> slot 3 are configured in setupCodec). The F32 TDM input gives us
// each slot directly as a 32-bit float channel; no high/low-half split
// + recombine like the int16 production path needs.
//
// /ch/05/mix/on 1 unmutes Mic L; /ch/06/mix/on 1 unmutes Mic R. Default
// mixL[2]/mixR[2] gain is 0 (both faders forced through g_chOn[5/6] =
// false at boot in applyChannelFader).
AudioConnection_F32  c_micL_mix (tdmIn, 2, mixL, 2);
AudioConnection_F32  c_micR_mix (tdmIn, 3, mixR, 2);

// Dexed (int16) -> bridge -> F32 per-synth gain -> sub-mixer slot 0.
// Two gain stages: per-synth gain (g_dexedGain, /synth/dexed/volume)
// and group bus gain (g_synthBus*, /synth/bus/volume). The dev
// surface's Mixer view drives the bus gain; the Synth tab drives the
// per-engine gain. Sub-mixer dual-mono fan-out: Dexed feeds slot 0
// of both g_synthSumL and g_synthSumR (centered).
AudioConnection      c_dexed_to_conv  (g_dexed,       0, g_dexedToF32,  0);
AudioConnection_F32  c_conv_to_gain   (g_dexedToF32,  0, g_dexedGain,   0);
AudioConnection_F32  c_dexed_to_sumL  (g_dexedGain,   0, g_synthSumLA,  0);
AudioConnection_F32  c_dexed_to_sumR  (g_dexedGain,   0, g_synthSumRA,  0);

// --- Sampler slot 1 audio graph ---
//
// Players (stereo int16) -> per-channel envelopes -> stage-1 4-into-1
// mixers (voices 0..3 in [0], 4..7 in [1]) -> stage-2 2-into-1 final
// mixer (sum of stage-1 outputs in inputs 0+1 of [2]) -> I16->F32
// bridge -> per-slot F32 gain -> sub-mixer slot 1.
AudioConnection c_sm_p0_L(g_smPlayer[0], 0, g_smEnvL[0], 0);
AudioConnection c_sm_p0_R(g_smPlayer[0], 1, g_smEnvR[0], 0);
AudioConnection c_sm_p1_L(g_smPlayer[1], 0, g_smEnvL[1], 0);
AudioConnection c_sm_p1_R(g_smPlayer[1], 1, g_smEnvR[1], 0);
AudioConnection c_sm_p2_L(g_smPlayer[2], 0, g_smEnvL[2], 0);
AudioConnection c_sm_p2_R(g_smPlayer[2], 1, g_smEnvR[2], 0);
AudioConnection c_sm_p3_L(g_smPlayer[3], 0, g_smEnvL[3], 0);
AudioConnection c_sm_p3_R(g_smPlayer[3], 1, g_smEnvR[3], 0);
AudioConnection c_sm_p4_L(g_smPlayer[4], 0, g_smEnvL[4], 0);
AudioConnection c_sm_p4_R(g_smPlayer[4], 1, g_smEnvR[4], 0);
AudioConnection c_sm_p5_L(g_smPlayer[5], 0, g_smEnvL[5], 0);
AudioConnection c_sm_p5_R(g_smPlayer[5], 1, g_smEnvR[5], 0);
AudioConnection c_sm_p6_L(g_smPlayer[6], 0, g_smEnvL[6], 0);
AudioConnection c_sm_p6_R(g_smPlayer[6], 1, g_smEnvR[6], 0);
AudioConnection c_sm_p7_L(g_smPlayer[7], 0, g_smEnvL[7], 0);
AudioConnection c_sm_p7_R(g_smPlayer[7], 1, g_smEnvR[7], 0);

// Stage 1 — voices 0..3 envelopes into mix[0]; voices 4..7 into mix[1].
AudioConnection c_sm_eL0_m1(g_smEnvL[0], 0, g_smMixL[0], 0);
AudioConnection c_sm_eL1_m1(g_smEnvL[1], 0, g_smMixL[0], 1);
AudioConnection c_sm_eL2_m1(g_smEnvL[2], 0, g_smMixL[0], 2);
AudioConnection c_sm_eL3_m1(g_smEnvL[3], 0, g_smMixL[0], 3);
AudioConnection c_sm_eL4_m2(g_smEnvL[4], 0, g_smMixL[1], 0);
AudioConnection c_sm_eL5_m2(g_smEnvL[5], 0, g_smMixL[1], 1);
AudioConnection c_sm_eL6_m2(g_smEnvL[6], 0, g_smMixL[1], 2);
AudioConnection c_sm_eL7_m2(g_smEnvL[7], 0, g_smMixL[1], 3);
AudioConnection c_sm_eR0_m1(g_smEnvR[0], 0, g_smMixR[0], 0);
AudioConnection c_sm_eR1_m1(g_smEnvR[1], 0, g_smMixR[0], 1);
AudioConnection c_sm_eR2_m1(g_smEnvR[2], 0, g_smMixR[0], 2);
AudioConnection c_sm_eR3_m1(g_smEnvR[3], 0, g_smMixR[0], 3);
AudioConnection c_sm_eR4_m2(g_smEnvR[4], 0, g_smMixR[1], 0);
AudioConnection c_sm_eR5_m2(g_smEnvR[5], 0, g_smMixR[1], 1);
AudioConnection c_sm_eR6_m2(g_smEnvR[6], 0, g_smMixR[1], 2);
AudioConnection c_sm_eR7_m2(g_smEnvR[7], 0, g_smMixR[1], 3);

// Stage 2 — sum the two stage-1 mixer outputs to a single mono per side.
AudioConnection c_sm_mL0_final(g_smMixL[0], 0, g_smMixL[2], 0);
AudioConnection c_sm_mL1_final(g_smMixL[1], 0, g_smMixL[2], 1);
AudioConnection c_sm_mR0_final(g_smMixR[0], 0, g_smMixR[2], 0);
AudioConnection c_sm_mR1_final(g_smMixR[1], 0, g_smMixR[2], 1);

// Bridge int16 -> F32, per-slot gain stage, then into the sub-mixer slot 1.
AudioConnection      c_sm_finalL_toF32(g_smMixL[2], 0, g_smToF32L, 0);
AudioConnection      c_sm_finalR_toF32(g_smMixR[2], 0, g_smToF32R, 0);
AudioConnection_F32  c_sm_toF32L_gainL(g_smToF32L,  0, g_smGainL,  0);
AudioConnection_F32  c_sm_toF32R_gainR(g_smToF32R,  0, g_smGainR,  0);
AudioConnection_F32  c_sm_gainL_sumL  (g_smGainL,   0, g_synthSumLA, 1);
AudioConnection_F32  c_sm_gainR_sumR  (g_smGainR,   0, g_synthSumRA, 1);

// --- MPE VA slot 3 audio graph ---
//
// Per voice: osc -> filter (LP) -> env. All four envelopes sum into a
// single 4-into-1 mono mixer (g_mpeMix), then I16->F32 bridge ->
// per-slot mono F32 gain -> dual-mono fan-out into sub-mixer slot 3.
//
// MpeVaSink only reads the SVF's lowpass output (index 0). Bandpass /
// highpass paths are unused by this engine; the SVF is voice-local
// anyway because per-voice CC#74 modulates the cutoff per finger.
AudioConnection c_mpe_o0_f0(g_mpeOsc[0], 0, g_mpeFilter[0], 0);
AudioConnection c_mpe_o1_f1(g_mpeOsc[1], 0, g_mpeFilter[1], 0);
AudioConnection c_mpe_o2_f2(g_mpeOsc[2], 0, g_mpeFilter[2], 0);
AudioConnection c_mpe_o3_f3(g_mpeOsc[3], 0, g_mpeFilter[3], 0);

AudioConnection c_mpe_f0_e0(g_mpeFilter[0], 0, g_mpeEnv[0], 0);
AudioConnection c_mpe_f1_e1(g_mpeFilter[1], 0, g_mpeEnv[1], 0);
AudioConnection c_mpe_f2_e2(g_mpeFilter[2], 0, g_mpeEnv[2], 0);
AudioConnection c_mpe_f3_e3(g_mpeFilter[3], 0, g_mpeEnv[3], 0);

AudioConnection c_mpe_e0_mix(g_mpeEnv[0], 0, g_mpeMix, 0);
AudioConnection c_mpe_e1_mix(g_mpeEnv[1], 0, g_mpeMix, 1);
AudioConnection c_mpe_e2_mix(g_mpeEnv[2], 0, g_mpeMix, 2);
AudioConnection c_mpe_e3_mix(g_mpeEnv[3], 0, g_mpeMix, 3);

// Bridge int16 -> F32, per-slot gain, then fan-out into sub-mixer
// slot 3 (input 3 of the stage-A mixer per side, centered dual-mono).
AudioConnection      c_mpe_mix_toF32 (g_mpeMix,    0, g_mpeToF32, 0);
AudioConnection_F32  c_mpe_toF32_gain(g_mpeToF32,  0, g_mpeGain,  0);
AudioConnection_F32  c_mpe_gain_sumL (g_mpeGain,   0, g_synthSumLA, 3);
AudioConnection_F32  c_mpe_gain_sumR (g_mpeGain,   0, g_synthSumRA, 3);

// --- Slot 4 (Neuro) audio graph ---
//
// 3 saws + sub sine -> voiceMix -> SVF lowpass -> ADSR envelope ->
// I16->F32 bridge -> per-slot mono F32 gain -> dual-mono fan-out into
// sub-mixer slot 4 (input 0 of the stage-B mixer per side).
AudioConnection      c_neuro_o1_mix  (g_neuroOsc1,    0, g_neuroVoiceMix, 0);
AudioConnection      c_neuro_o2_mix  (g_neuroOsc2,    0, g_neuroVoiceMix, 1);
AudioConnection      c_neuro_o3_mix  (g_neuroOsc3,    0, g_neuroVoiceMix, 2);
AudioConnection      c_neuro_os_mix  (g_neuroOscSub,  0, g_neuroVoiceMix, 3);
AudioConnection      c_neuro_mix_f   (g_neuroVoiceMix,0, g_neuroFilt,     0);
// SVF lowpass output (channel 0). Bandpass / highpass unused.
AudioConnection      c_neuro_f_env   (g_neuroFilt,    0, g_neuroEnv,      0);
AudioConnection      c_neuro_env_toF32(g_neuroEnv,    0, g_neuroToF32,    0);
AudioConnection_F32  c_neuro_toF32_gain(g_neuroToF32, 0, g_neuroGain,     0);
AudioConnection_F32  c_neuro_gain_sumL(g_neuroGain,   0, g_synthSumLB,    0);
AudioConnection_F32  c_neuro_gain_sumR(g_neuroGain,   0, g_synthSumRB,    0);

// --- Slot 6 (Supersaw) audio graph ---
//
// 5 oscs -> mixAB (osc1..osc4) -> mixFinal (mixAB + osc5) -> SVF -> ADSR
// envelope -> I16->F32 bridge -> per-slot mono F32 gain -> dual-mono
// fan-out into sub-mixer slot 6 (input 2 of the stage-B mixer per side).
AudioConnection      c_ss_o1_mab   (g_supersawO1,       0, g_supersawMixAB,    0);
AudioConnection      c_ss_o2_mab   (g_supersawO2,       0, g_supersawMixAB,    1);
AudioConnection      c_ss_o3_mab   (g_supersawO3,       0, g_supersawMixAB,    2);
AudioConnection      c_ss_o4_mab   (g_supersawO4,       0, g_supersawMixAB,    3);
AudioConnection      c_ss_mab_mf   (g_supersawMixAB,    0, g_supersawMixFinal, 0);
AudioConnection      c_ss_o5_mf    (g_supersawO5,       0, g_supersawMixFinal, 1);
AudioConnection      c_ss_mf_filt  (g_supersawMixFinal, 0, g_supersawFilt,     0);
// SVF lowpass output (channel 0). Bandpass / highpass unused.
AudioConnection      c_ss_filt_env (g_supersawFilt,     0, g_supersawEnv,      0);
AudioConnection      c_ss_env_toF32(g_supersawEnv,      0, g_supersawToF32,    0);
AudioConnection_F32  c_ss_toF32_gain(g_supersawToF32,   0, g_supersawGain,     0);
AudioConnection_F32  c_ss_gain_sumL(g_supersawGain,     0, g_synthSumLB,       2);
AudioConnection_F32  c_ss_gain_sumR(g_supersawGain,     0, g_synthSumRB,       2);

// --- Synth sub-mixer chain (8 slots) ---
//
// Slots 0..3 sum into g_synthSumLA / g_synthSumRA (slot N -> input N).
// Slots 4..7 sum into g_synthSumLB / g_synthSumRB (slot N -> input N-4).
// Stage A and B feed the final stage C, which feeds g_synthBusL/R.
AudioConnection_F32  c_synthsumLA_LC  (g_synthSumLA,  0, g_synthSumLC, 0);
AudioConnection_F32  c_synthsumLB_LC  (g_synthSumLB,  0, g_synthSumLC, 1);
AudioConnection_F32  c_synthsumRA_RC  (g_synthSumRA,  0, g_synthSumRC, 0);
AudioConnection_F32  c_synthsumRB_RC  (g_synthSumRB,  0, g_synthSumRC, 1);
AudioConnection_F32  c_synthsumL_busL (g_synthSumLC,  0, g_synthBusL,  0);
AudioConnection_F32  c_synthsumR_busR (g_synthSumRC,  0, g_synthBusR,  0);
AudioConnection_F32  c_synthbusL_mix  (g_synthBusL,  0, mixL,        1);
AudioConnection_F32  c_synthbusR_mix  (g_synthBusR,  0, mixR,        1);

// ============================================================================
// Mixer state — driven by OSC handlers below
// ============================================================================
//
// These are the single source of truth for the mixer's runtime values.
// applyChannelFader / applyMain / applyProcShelf push them into the audio
// graph. broadcastSnapshot reads them when a fresh dev-surface client
// asks for /snapshot.
//
// Channel fader range: 0..1 linear (matches the dispatcher's existing
// /ch/NN/mix/fader convention). Main fader range: same. Shelf parameters
// match the production project's defaults.

// Channel index map (matches the production project's mixer view):
//   ch1 = USB L          → mixL[0]
//   ch2 = USB R          → mixR[0]
//   ch3 = Line L         (TAC5212 ADC CH1, not yet wired in F32 path)
//   ch4 = Line R         (TAC5212 ADC CH2, not yet wired in F32 path)
//   ch5 = Mic L          → mixL[2] (PDM, defaults muted)
//   ch6 = Mic R          → mixR[2] (PDM, defaults muted)
//   ch7..ch10 = XLR 1..4 (TLV320ADC6140, not yet wired)
//
// Mute-by-default for ch5/ch6 because the on-board PDM mics are LIVE
// the moment their channels are powered — we don't want unintended
// monitoring at boot. Dev surface unmutes via /ch/05/mix/on 1.
//
// Faders default to 0.75 — that's X32 unity (0 dB) under the
// 4-segment fader law; 1.0 would mean +10 dB and slam the DAC.
static float g_chFader[7]    = {0.0f, 0.75f, 0.75f, 0.75f, 0.75f, 0.75f, 0.75f};
static bool  g_chOn[7]       = {false, true, true, true, true, false, false};

// Per-channel display name (12-char limit per X32 convention). Index 0
// unused. /ch/NN/config/name does NOT mirror across linked pairs (the
// spec lets a stereo pair have different L/R labels).
static char  g_chName[7][16] = {"", "USB L", "USB R", "Line L", "Line R", "Mic L", "Mic R"};
static float g_mainFaderL    = 0.75f;
static float g_mainFaderR    = 0.75f;
static bool  g_mainOn        = true;

// Playback hostvol — Windows speaker slider, USB FU 0x31. Engaged by
// default: dragging the Windows slider always attenuates the master
// output. The DAC unmute in setup() runs AFTER pollHostVolume() so
// audio goes from silent straight to the host-attenuated level rather
// than briefly outputting at unity. /main/st/hostvol/enable 0 disables
// the stage (holds the amp at 1.0) for cases where the user wants the
// dev surface fader to be the only volume control.
static bool  g_hostvolEnable = true;
static float g_hostvolValue  = 1.0f;

// Capture hostvol — Windows recording slider, USB FU 0x30. Drives
// listenback monitor amps (added later, alongside Line/PDM/XLR
// channels). For now we just track it so the dev surface can render
// a CAP HOST strip; nothing in the audio graph consumes it yet.
static float g_capHostvolValue = 1.0f;
static bool  g_capHostvolMute  = false;

// High-shelf "Dull" preset on at boot — 3 kHz / -12 dB cut tames FM
// synth sizzle and harshness without changing the broad tonal balance.
// /proc/shelf/enable 0 disables the stage (collapses biquad to
// passthrough); the freq/gain values can be reshaped from the dev
// surface for milder presets.
static bool  g_procShelfEnable = true;
static float g_procShelfFreqHz = 3000.0f;
static float g_procShelfGainDb = -12.0f;    // matches production "Dull" preset

// Dexed synth — volume / on / current voice. Mirrors the production
// project's /synth/dexed/... contract so the dev surface populates
// without changes. ON by default with -3 dB volume so a USB MIDI key
// press produces sound out of the box.
static float g_dexedVolume = 0.7f;
static bool  g_dexedOn     = true;
static int   g_dexedBank   = 0;
static int   g_dexedVoice  = 0;

// Synth-bus group fader — what the dev surface's Mixer "synth" strip
// drives via /synth/bus/volume + /synth/bus/on. Sits downstream of
// every per-synth volume and upstream of the mix bus. Defaults to
// X32 unity (0.75 = 0 dB).
static float g_synthBusVolume = 0.75f;
static bool  g_synthBusOn     = true;

// --- Synth slots / switcher ---
//
// The slot abstraction lets the firmware host multiple synth engines
// while only one is audible at a time (per project memory
// feedback_synth_switch_not_layer — switch UX, not layer UX). Slot 0 =
// Dexed (existing engine, wrapped by DexedSlot which observes g_dexedOn
// / g_dexedVolume via const pointers). Slot 1 = MultisampleSlot
// (SD-streaming sampler — Phase 2). Slot 3 = MpeSlot (MPE-aware VA).
// Slot 2 (Plaits) and slots 4..7 are SilentSlot placeholders until
// later phases / per-slot agents bring more engines online.
//
// SynthSwitcher is registered as the sole downstream of g_arpFilter.
// It fans MIDI to whichever slot is active. Switching a slot calls
// setActive(true/false) on the engines so the outgoing slot panics
// held notes + zeroes its gain stage and the incoming slot restores
// its stored fader.
tdsp_synth::DexedSlot     g_dexedSlot(&g_dexed, &g_dexedSink, &g_dexedGain,
                                      &g_dexedOn, &g_dexedVolume);

static const tdsp_synth::MultisampleSlot::AudioContext kSamplerCtx{
    g_smPlayer, g_smEnvL, g_smEnvR,
    g_smMixL,   g_smMixR,
    &g_smGainL, &g_smGainR,
};
tdsp_synth::MultisampleSlot g_multisampleSlot(kSamplerCtx);

// Slot 3 — MPE-aware virtual-analog. Wraps the MpeVaSink already
// instantiated above (g_mpeSink), gating its output through g_mpeGain
// based on slot-active state. Per-MPE-voice CC#74 routes per-voice
// inside the sink; the slot just owns the volume/on/master-channel
// surface and the active flag.
static const tdsp_synth::MpeSlot::AudioContext kMpeCtx{
    &g_mpeSink,
    &g_mpeGain,
};
tdsp_synth::MpeSlot       g_mpeSlot(kMpeCtx);

// Slot 4 — Neuro (reese / DnB neuro bass). Wraps the NeuroSink
// instantiated above (g_neuroSink), gating its output through
// g_neuroGain based on slot-active state. Engine knobs (detune /
// sub / cutoff / ADSR / LFO / portamento) round-trip through OSC
// directly into the sink.
static const tdsp_synth::NeuroSlot::AudioContext kNeuroCtx{
    &g_neuroSink,
    &g_neuroGain,
};
tdsp_synth::NeuroSlot     g_neuroSlot(kNeuroCtx);

// Slot 6 — Supersaw (JP-8000-style 5-osc detuned-saw lead). Wraps the
// SupersawSink instantiated above (g_supersawSink), gating its output
// through g_supersawGain based on slot-active state. Engine knobs
// (detune / mix / cutoff / ADSR / portamento) round-trip through OSC
// directly into the sink.
static const tdsp_synth::SupersawSlot::AudioContext kSupersawCtx{
    &g_supersawSink,
    &g_supersawGain,
};
tdsp_synth::SupersawSlot  g_supersawSlot(kSupersawCtx);

tdsp_synth::SilentSlot    g_silentSlot2("silent", "(empty)");
tdsp_synth::SilentSlot    g_silentSlot4("silent", "(empty)");
tdsp_synth::SilentSlot    g_silentSlot5("silent", "(empty)");
tdsp_synth::SilentSlot    g_silentSlot7("silent", "(empty)");
tdsp_synth::SynthSwitcher g_synthSwitcher;

// Stereo-link state — global config, X32 convention. Index 0 = pair
// (1,2), index 1 = pair (3,4), index 2 = pair (5,6). Set via
// /config/chlink/{1-2,3-4,5-6}; when on, server-side mirrors writes
// across the linked pair (see mirrorPartner / linkApplyChannelFader
// below). Not per-channel state — link is a property of the pair.
//
// Default: all three pairs linked. The hardware lays out USB, Line,
// and Mic as natural stereo pairs — USB host audio is L+R, the line
// input jack is a stereo TRS, and the on-board PDM mics are a
// stereo pair. Linking them by default means a fader/mute gesture
// on either side of a pair affects both, which is what most users
// expect on first touch. The dev surface's link button can break
// the pair (set chlink to 0) for cases where independent control
// is needed.
static bool g_chLink[3] = {true, true, true};

// If `ch` is the L or R member of a currently-linked pair, returns the
// partner channel number; otherwise 0. Pair (1,2) is index 0, pair
// (3,4) is index 1, pair (5,6) is index 2.
static int linkPartner(int ch) {
    if (ch < 1 || ch > 6) return 0;
    int idx = (ch - 1) / 2;        // pair index 0..2
    if (!g_chLink[idx]) return 0;
    return (ch & 1) ? ch + 1 : ch - 1;  // odd -> +1, even -> -1
}

// Map a /config/chlink/N-M address to its g_chLink index. Returns -1
// for an unrecognized pair token.
static int chlinkPairIndex(const char *pair) {
    if (strcmp(pair, "1-2") == 0) return 0;
    if (strcmp(pair, "3-4") == 0) return 1;
    if (strcmp(pair, "5-6") == 0) return 2;
    return -1;
}

// ============================================================================
// Control plane — codec driver + OSC dispatcher + dev-surface transport
// ============================================================================

tac5212::TAC5212          g_codec(Wire);
Tac5212Panel              g_codecPanel(g_codec);

tdsp::MixerModel          g_model;       // unbound: we drive the F32 graph directly
tdsp::OscDispatcher       g_dispatcher;  // routes /codec/tac5212/... to panel
tdsp::SlipOscTransport    g_transport;

// USB host stack for an external MIDI keyboard plugged into the Teensy
// 4.1's USB host port. Hubs are declared so a powered hub between the
// Teensy and keyboard works transparently. MIDIDevice exposes the
// alex6679-style note / CC / pitch-bend / sysex / clock callbacks the
// router needs.
USBHost      g_usbHost;
USBHub       g_usbHub1(g_usbHost);
USBHub       g_usbHub2(g_usbHost);
MIDIDevice   g_midiIn(g_usbHost);

// MIDI router fans events from any source (USB host, OSC) to all
// registered sinks. Phase 3a registers only the viz sink; Phase 3b
// adds DexedSink (and later other engines) downstream of an arpeggiator
// filter once the synth path is wired.
tdsp::MidiRouter g_midiRouter;

// Shared musical clock — drives the arpeggiator when external MIDI
// clock is present, OR fans an internal 24-PPQN tick stream into the
// router via the internal-tick hook so the arp runs without an
// external source. ClockSink bridges 0xF8 / 0xFA / 0xFB / 0xFC from
// the router into clock state.
tdsp::Clock     g_clock;
tdsp::ClockSink g_clockSink(&g_clock);

// MIDI arpeggiator — sits between router and synth sinks. setEnabled
// (false) is pure pass-through (every event forwarded verbatim);
// setEnabled(true) re-synthesizes note events at clock-aligned rates
// from the held-keys set. Defaults to disabled — keyboard plays
// straight Dexed until /arp/on 1 from the dev surface engages it.
tdsp::ArpFilter g_arpFilter;

// MidiVizSink — broadcasts every note-on / note-off as /midi/note for
// the dev surface's piano-roll display. Subscription-gated by the dev
// surface (defaults to off, dev surface flips it on when the panel is
// open). Defined inline because it's a small specialized OSC bridge.
class MidiVizSink : public tdsp::MidiSink {
public:
    void enable(bool on) { _enabled = on; }
    bool enabled() const { return _enabled; }

    void onNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) override {
        if (!_enabled) return;
        broadcastNote(note, velocity, channel);
    }
    void onNoteOff(uint8_t channel, uint8_t note, uint8_t /*velocity*/) override {
        if (!_enabled) return;
        // Wire format uses velocity == 0 as note-off so the UI can treat
        // (velocity > 0) as "held".
        broadcastNote(note, 0, channel);
    }

private:
    void broadcastNote(uint8_t note, uint8_t velocity, uint8_t channel) {
        OSCMessage m("/midi/note");
        m.add((int)note);
        m.add((int)velocity);
        m.add((int)channel);
        OSCBundle b;
        b.add(m);
        g_transport.broadcastBundle(b);
    }
    bool _enabled = false;
};

MidiVizSink g_midiVizSink;

// Forward-decls.
static void onOscMessage(OSCMessage &msg, void *userData);
static void setupCodec();

// USB host -> router shims. USBHost_t36 takes plain function pointers
// (not std::function), so each handler is a one-liner that fans into
// g_midiRouter. The router normalizes velocity-0 noteOn to noteOff and
// scales pitch bend by the per-channel range.
static void onUsbHostNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    g_midiRouter.handleNoteOn(channel, note, velocity);
}
static void onUsbHostNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) {
    g_midiRouter.handleNoteOff(channel, note, velocity);
}
static void onUsbHostControlChange(uint8_t channel, uint8_t cc, uint8_t value) {
    g_midiRouter.handleControlChange(channel, cc, value);
}
static void onUsbHostPitchChange(uint8_t channel, int pitch) {
    // USBHost_t36 delivers pitch bend as plain int in the range
    // -8192..+8191. Narrow to int16_t (the value range fits).
    g_midiRouter.handlePitchBend(channel, (int16_t)pitch);
}
static void onUsbHostAfterTouch(uint8_t channel, uint8_t pressure) {
    g_midiRouter.handleChannelPressure(channel, pressure);
}
static void onUsbHostProgramChange(uint8_t channel, uint8_t program) {
    g_midiRouter.handleProgramChange(channel, program);
}
static void onUsbHostSysEx(const uint8_t *data, uint16_t length, bool last) {
    g_midiRouter.handleSysEx(data, (size_t)length, last);
}

// ============================================================================
// Apply functions — push state into the F32 graph
// ============================================================================

// Channel -> (mixer side, slot) mapping. Channels 3/4 (Line) and 7..10
// (XLR) reserve their slots for later phases — calling applyChannelFader
// on them is a no-op until those audio paths are wired.
struct ChMap { AudioMixer4_F32 *mix; int slot; };
static ChMap chToMix(int ch) {
    switch (ch) {
        case 1:  return { &mixL, 0 };  // USB L
        case 2:  return { &mixR, 0 };  // USB R
        case 5:  return { &mixL, 2 };  // Mic L (PDM)
        case 6:  return { &mixR, 2 };  // Mic R (PDM)
        default: return { nullptr, 0 };
    }
}

// Channel fader -> linear gain via X32 4-segment fader law. The fader
// values stored in g_chFader[] are in 0..1 X32 form (already snapped to
// the 1024-step grid by the OSC handler); converting to linear gain is
// the chip's job.
static void applyChannelFader(int ch) {
    if (ch < 1 || ch >= (int)(sizeof(g_chOn) / sizeof(g_chOn[0]))) return;
    ChMap m = chToMix(ch);
    if (!m.mix) return;  // channel reserved for a future audio path
    const float g = g_chOn[ch] ? tdsp::x32::faderToLinear(g_chFader[ch]) : 0.0f;
    m.mix->gain(m.slot, g);
}

// Main fader -> linear gain via X32 fader law. Same convention as the
// channel fader: g_mainFaderL/R are X32-shape 0..1 floats; conversion
// to linear gain happens here.
static void applyMain() {
    const float gL = g_mainOn ? tdsp::x32::faderToLinear(g_mainFaderL) : 0.0f;
    const float gR = g_mainOn ? tdsp::x32::faderToLinear(g_mainFaderR) : 0.0f;
    mainAmpL.setGain(gL);
    mainAmpR.setGain(gR);
}

// Push g_hostvolValue (or 1.0 when disabled) into both hostvol amps.
static void applyHostvol() {
    const float g = g_hostvolEnable ? g_hostvolValue : 1.0f;
    hostvolAmpL.setGain(g);
    hostvolAmpR.setGain(g);
}

// Poll Windows playback Feature Unit. Reads via the F32 USB class's
// volume()/mute() accessors so this file doesn't need <usb_audio.h>.
// Square-law taper matches the production project's "log-pot feel".
//
// Always applies the polled value (no change=0 gate) — this is the
// authoritative source for master-out attenuation. Cold boot reads the
// FEATURE_MAX_VOLUME/2 default = 50% slider = -12 dB scaled, which is
// what the chip outputs until Windows pushes a SET_CUR with the actual
// slider position (typically immediately after enumeration). When
// g_hostvolEnable is false the polled value is still tracked (echoed
// via OSC) but applyHostvol() holds the amp at 1.0.
static void pollHostVolume() {
    static float s_lastRaw  = -1.0f;
    static int   s_lastMute = -1;

    const float raw   = AudioInputUSB_F32::volume();   // 0..1, mute-folded
    const int   muted = AudioInputUSB_F32::mute() ? 1 : 0;

    if (raw == s_lastRaw && muted == s_lastMute) return;
    s_lastRaw  = raw;
    s_lastMute = muted;

    float scaled = raw * raw;
    if (scaled > 1.0f) scaled = 1.0f;
    if (scaled < 0.0f) scaled = 0.0f;

    g_hostvolValue = scaled;
    applyHostvol();

    // Broadcast to dev surface so its hostvol display tracks Windows.
    OSCBundle reply;
    OSCMessage m("/main/st/hostvol/value");
    m.add(g_hostvolValue);
    reply.add(m);
    g_transport.sendBundle(reply);
}

// Dexed gain — X32 fader law applied to g_dexedVolume (0..1 fader form),
// gated by the per-engine on/off flag AND the slot active flag. With
// the slot model, /synth/dexed/mix/fader and /synth/dexed/mix/on
// continue to mutate g_dexedVolume / g_dexedOn here so the dev surface
// contract is unchanged; the slot's applyGain() reads those + the
// active-slot state and pushes the combined value into the gain stage.
// Inactive Dexed: gain held at 0 regardless of fader/on values, so
// stored state survives a switch-out / switch-in round trip.
static void applyDexedGain() {
    g_dexedSlot.applyGain();
}

static void applyDexedVolume(float v) {
    g_dexedVolume = tdsp::x32::quantizeFader(v);
    applyDexedGain();
}

static void applyDexedOn(bool on) {
    g_dexedOn = on;
    applyDexedGain();
}

// Synth-bus group fader. X32 fader law applied to g_synthBusVolume.
// on=false holds the bus at 0 without disturbing the stored fader value
// — toggle returns the bus to its previous level.
static void applySynthBusGain() {
    const float g = g_synthBusOn ? tdsp::x32::faderToLinear(g_synthBusVolume) : 0.0f;
    g_synthBusL.setGain(g);
    g_synthBusR.setGain(g);
}

// Load bank/voice into the Dexed engine. Clamps invalid indices and
// panics held notes before swapping (decodeVoice() would auto-panic
// anyway, but doing it explicitly here survives library churn).
static void applyDexedVoice(int bank, int voice) {
    if (bank < 0)                                 bank  = 0;
    if (bank >= tdsp::dexed::kNumBanks)           bank  = tdsp::dexed::kNumBanks - 1;
    if (voice < 0)                                voice = 0;
    if (voice >= tdsp::dexed::kVoicesPerBank)     voice = tdsp::dexed::kVoicesPerBank - 1;
    g_dexed.panic();
    if (tdsp::dexed::loadVoice(g_dexed, bank, voice)) {
        g_dexedBank  = bank;
        g_dexedVoice = voice;
    }
}

// Poll Windows recording-side Feature Unit (FU 0x30). Tracks the
// slider's value + mute and broadcasts both to the dev surface. No
// audio-graph consumer in this phase — that lands when listenback
// monitor amps are added alongside Line / PDM / XLR channels.
static void pollCaptureHostVolume() {
    const float v   = AudioOutputUSB_F32::volume();
    const bool  mut = AudioOutputUSB_F32::mute();

    bool changed = false;
    if (v != g_capHostvolValue) { g_capHostvolValue = v;   changed = true; }
    if (mut != g_capHostvolMute){ g_capHostvolMute  = mut; changed = true; }
    if (!changed) return;

    OSCBundle reply;
    { OSCMessage m("/-cap/hostvol/value"); m.add(g_capHostvolValue);          reply.add(m); }
    { OSCMessage m("/-cap/hostvol/mute");  m.add((int)(g_capHostvolMute?1:0)); reply.add(m); }
    g_transport.sendBundle(reply);
}

static void applyProcShelf() {
    if (g_procShelfEnable) {
        // setHighShelf signature: (stage, frequency, gain_dB, slope).
        // slope=1.0 gives a Q ≈ 0.7 shelf — the same behavior the int16
        // production project uses (its setHighShelf takes slope=1.0).
        procShelfL.setHighShelf(0, g_procShelfFreqHz, g_procShelfGainDb, 1.0f);
        procShelfR.setHighShelf(0, g_procShelfFreqHz, g_procShelfGainDb, 1.0f);
        // setCoefficients flips doBiquad=true; begin() compiles the float
        // coefficient table for the CMSIS DF1 cascade. Per the F32
        // biquad header's note: "If your INO is broken, ... add
        // myBiquad.begin(); to your INO after the coefficients have been
        // set."
        procShelfL.begin();
        procShelfR.begin();
    } else {
        // Drop back to passthrough by restoring the do-nothing init state.
        // doClassInit() resets numStagesUsed = 0 and doBiquad = false; the
        // class's update() checks doBiquad and short-circuits to a copy.
        // end() flips doBiquad without touching coefficients.
        procShelfL.end();
        procShelfR.end();
    }
}

// ============================================================================
// /snapshot — Phase 2 reply
// ============================================================================
//
// Now includes mixer state alongside codec panel state. /ch/.../mix/fader,
// /main/st/..., /proc/shelf/... echoes match the addresses the dev surface
// already knows from the production project, so the existing UI populates
// without any client-side changes.

static void broadcastSnapshot(OSCBundle &reply) {
    // Per-channel state — ch1/2 (USB), ch3/4 (Line, audio path TBD),
    // ch5/6 (PDM mic). XLR 7..10 land when the ADC6140 wires in.
    for (int ch = 1; ch <= 6; ++ch) {
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "/ch/%02d/mix/fader", ch);
            OSCMessage mm(buf);
            mm.add(g_chFader[ch]);
            reply.add(mm);
        }
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "/ch/%02d/mix/on", ch);
            OSCMessage mm(buf);
            mm.add((int)(g_chOn[ch] ? 1 : 0));
            reply.add(mm);
        }
    }

    // Stereo-link state — pairs (1,2), (3,4), (5,6).
    { OSCMessage m("/config/chlink/1-2"); m.add((int)(g_chLink[0] ? 1 : 0)); reply.add(m); }
    { OSCMessage m("/config/chlink/3-4"); m.add((int)(g_chLink[1] ? 1 : 0)); reply.add(m); }
    { OSCMessage m("/config/chlink/5-6"); m.add((int)(g_chLink[2] ? 1 : 0)); reply.add(m); }

    // Main bus.
    { OSCMessage m("/main/st/mix/faderL"); m.add(g_mainFaderL); reply.add(m); }
    { OSCMessage m("/main/st/mix/faderR"); m.add(g_mainFaderR); reply.add(m); }
    { OSCMessage m("/main/st/mix/on");     m.add((int)(g_mainOn ? 1 : 0)); reply.add(m); }

    // Host volume — playback (FU 0x31, /main/st/hostvol) and capture
    // (FU 0x30, /-cap/hostvol housekeeping branch).
    { OSCMessage m("/main/st/hostvol/on");    m.add((int)(g_hostvolEnable ? 1 : 0)); reply.add(m); }
    { OSCMessage m("/main/st/hostvol/value"); m.add(g_hostvolValue);                 reply.add(m); }
    { OSCMessage m("/-cap/hostvol/value");    m.add(g_capHostvolValue);              reply.add(m); }
    { OSCMessage m("/-cap/hostvol/mute");     m.add((int)(g_capHostvolMute ? 1 : 0)); reply.add(m); }

    // Master EQ — band 1 mapped to today's high-shelf processing
    // biquad. Bands 2..4 reserved per OSC.md.
    { OSCMessage m("/main/st/eq/on");    m.add((int)(g_procShelfEnable ? 1 : 0)); reply.add(m); }
    { OSCMessage m("/main/st/eq/1/type"); m.add((int)4);              reply.add(m); }  // HShlv
    { OSCMessage m("/main/st/eq/1/f");    m.add(g_procShelfFreqHz);   reply.add(m); }
    { OSCMessage m("/main/st/eq/1/g");    m.add(g_procShelfGainDb);   reply.add(m); }
    { OSCMessage m("/main/st/eq/1/q");    m.add(0.707f);              reply.add(m); }

    // Arp.
    { OSCMessage m("/arp/on"); m.add((int)(g_arpFilter.enabled() ? 1 : 0)); reply.add(m); }

    // Synth bus (mixer's "synth" strip).
    { OSCMessage m("/synth/bus/mix/fader"); m.add(g_synthBusVolume);            reply.add(m); }
    { OSCMessage m("/synth/bus/mix/on");    m.add((int)(g_synthBusOn ? 1 : 0)); reply.add(m); }

    // Slot 1 — multisample sampler.
    { OSCMessage m("/synth/sampler/mix/fader"); m.add(g_multisampleSlot.volume());            reply.add(m); }
    { OSCMessage m("/synth/sampler/mix/on");    m.add((int)(g_multisampleSlot.on() ? 1 : 0)); reply.add(m); }
    { OSCMessage m("/synth/sampler/midi/ch");   m.add((int)g_multisampleSlot.listenChannel()); reply.add(m); }

    // Slot 3 — MPE VA. /synth/mpe/volume + /synth/mpe/on are the
    // legacy paths the dispatcher already binds to. The engine knobs
    // (waveform / attack / release / filter / LFO) round-trip through
    // OSC; including them in the snapshot means a fresh-connect
    // dev surface populates without an extra round of queries.
    { OSCMessage m("/synth/mpe/volume");          m.add(g_mpeSlot.volume());                  reply.add(m); }
    { OSCMessage m("/synth/mpe/on");              m.add((int)(g_mpeSlot.on() ? 1 : 0));       reply.add(m); }
    { OSCMessage m("/synth/mpe/midi/master");     m.add((int)g_mpeSlot.masterChannel());      reply.add(m); }
    { OSCMessage m("/synth/mpe/waveform");        m.add((int)g_mpeSlot.sink()->waveform());   reply.add(m); }
    { OSCMessage m("/synth/mpe/attack");          m.add(g_mpeSlot.sink()->attack());          reply.add(m); }
    { OSCMessage m("/synth/mpe/release");         m.add(g_mpeSlot.sink()->release());         reply.add(m); }
    { OSCMessage m("/synth/mpe/filter/cutoff");   m.add(g_mpeSlot.sink()->filterCutoff());    reply.add(m); }
    { OSCMessage m("/synth/mpe/filter/resonance");m.add(g_mpeSlot.sink()->filterResonance()); reply.add(m); }
    { OSCMessage m("/synth/mpe/lfo/rate");        m.add(g_mpeSlot.sink()->lfoRate());         reply.add(m); }
    { OSCMessage m("/synth/mpe/lfo/depth");       m.add(g_mpeSlot.sink()->lfoDepth());        reply.add(m); }
    { OSCMessage m("/synth/mpe/lfo/dest");        m.add((int)g_mpeSlot.sink()->lfoDest());    reply.add(m); }
    { OSCMessage m("/synth/mpe/lfo/waveform");    m.add((int)g_mpeSlot.sink()->lfoWaveform());reply.add(m); }

    // Slot 4 — Neuro. /synth/neuro/* paths match the legacy
    // dispatcher (set via dispatcher.setNeuro*). Stink-chain leaves
    // are intentionally omitted — that part of the engine is out of
    // scope for the slot rebuild and will be re-added later.
    { OSCMessage m("/synth/neuro/volume");           m.add(g_neuroSlot.volume());                       reply.add(m); }
    { OSCMessage m("/synth/neuro/on");               m.add((int)(g_neuroSlot.on() ? 1 : 0));            reply.add(m); }
    { OSCMessage m("/synth/neuro/midi/ch");          m.add((int)g_neuroSlot.listenChannel());           reply.add(m); }
    { OSCMessage m("/synth/neuro/attack");           m.add(g_neuroSlot.sink()->attack());               reply.add(m); }
    { OSCMessage m("/synth/neuro/release");          m.add(g_neuroSlot.sink()->release());              reply.add(m); }
    { OSCMessage m("/synth/neuro/detune");           m.add(g_neuroSlot.sink()->detuneCents());          reply.add(m); }
    { OSCMessage m("/synth/neuro/sub");              m.add(g_neuroSlot.sink()->subLevel());             reply.add(m); }
    { OSCMessage m("/synth/neuro/osc3");             m.add(g_neuroSlot.sink()->osc3Level());            reply.add(m); }
    { OSCMessage m("/synth/neuro/filter/cutoff");    m.add(g_neuroSlot.sink()->filterCutoff());         reply.add(m); }
    { OSCMessage m("/synth/neuro/filter/resonance"); m.add(g_neuroSlot.sink()->filterResonance());      reply.add(m); }
    { OSCMessage m("/synth/neuro/lfo/rate");         m.add(g_neuroSlot.sink()->lfoRate());              reply.add(m); }
    { OSCMessage m("/synth/neuro/lfo/depth");        m.add(g_neuroSlot.sink()->lfoDepth());             reply.add(m); }
    { OSCMessage m("/synth/neuro/lfo/dest");         m.add((int)g_neuroSlot.sink()->lfoDest());         reply.add(m); }
    { OSCMessage m("/synth/neuro/lfo/waveform");     m.add((int)g_neuroSlot.sink()->lfoWaveform());     reply.add(m); }
    { OSCMessage m("/synth/neuro/portamento");       m.add(g_neuroSlot.sink()->portamentoMs());         reply.add(m); }

    // Slot 6 — Supersaw. /synth/supersaw/* paths match the legacy
    // dispatcher (set via dispatcher.setSupersaw*). Engine knobs
    // round-trip through OSC; including them in the snapshot means a
    // fresh-connect dev surface populates without per-leaf reads.
    { OSCMessage m("/synth/supersaw/volume");      m.add(g_supersawSlot.volume());                    reply.add(m); }
    { OSCMessage m("/synth/supersaw/on");          m.add((int)(g_supersawSlot.on() ? 1 : 0));         reply.add(m); }
    { OSCMessage m("/synth/supersaw/midi/ch");     m.add((int)g_supersawSlot.listenChannel());        reply.add(m); }
    { OSCMessage m("/synth/supersaw/detune");      m.add(g_supersawSlot.sink()->detuneCents());       reply.add(m); }
    { OSCMessage m("/synth/supersaw/mix_center");  m.add(g_supersawSlot.sink()->mixCenter());         reply.add(m); }
    { OSCMessage m("/synth/supersaw/cutoff");      m.add(g_supersawSlot.sink()->cutoff());            reply.add(m); }
    { OSCMessage m("/synth/supersaw/resonance");   m.add(g_supersawSlot.sink()->resonance());         reply.add(m); }
    { OSCMessage m("/synth/supersaw/attack");      m.add(g_supersawSlot.sink()->attack());            reply.add(m); }
    { OSCMessage m("/synth/supersaw/decay");       m.add(g_supersawSlot.sink()->decay());             reply.add(m); }
    { OSCMessage m("/synth/supersaw/sustain");     m.add(g_supersawSlot.sink()->sustain());           reply.add(m); }
    { OSCMessage m("/synth/supersaw/release");     m.add(g_supersawSlot.sink()->release());           reply.add(m); }
    { OSCMessage m("/synth/supersaw/portamento");  m.add(g_supersawSlot.sink()->portamentoMs());      reply.add(m); }

    {
        OSCMessage m("/synth/sampler/info");
        m.add(g_multisampleSlot.bankName());
        m.add((int)g_multisampleSlot.numSamples());
        m.add((int)g_multisampleSlot.numReleaseSamples());
        reply.add(m);
    }

    // Slot model: /synth/active picks one of N slots; /synth/slots
    // enumerates them so the dev surface can render the picker.
    { OSCMessage m("/synth/active"); m.add(g_synthSwitcher.active()); reply.add(m); }
    {
        OSCMessage m("/synth/slots");
        for (int i = 0; i < tdsp_synth::SynthSwitcher::kMaxSlots; ++i) {
            auto *s = g_synthSwitcher.slot(i);
            char buf[48];
            if (s) snprintf(buf, sizeof(buf), "%s|%s", s->id(), s->displayName());
            else   snprintf(buf, sizeof(buf), "silent|(empty)");
            m.add(buf);
        }
        reply.add(m);
    }

    // Synth — Dexed.
    { OSCMessage m("/synth/dexed/mix/fader"); m.add(g_dexedVolume);                 reply.add(m); }
    { OSCMessage m("/synth/dexed/mix/on");    m.add((int)(g_dexedOn ? 1 : 0));      reply.add(m); }
    { OSCMessage m("/synth/dexed/midi/ch");   m.add((int)g_dexedSink.listenChannel()); reply.add(m); }
    {
        char name[tdsp::dexed::kVoiceNameBufBytes] = {0};
        tdsp::dexed::copyVoiceName(g_dexedBank, g_dexedVoice, name, sizeof(name));
        OSCMessage m("/synth/dexed/voice");
        m.add(g_dexedBank);
        m.add(g_dexedVoice);
        m.add(name);
        reply.add(m);
    }
    // Bank + voice name lists. These are normally fetched via separate
    // /synth/dexed/bank/names and /synth/dexed/voice/names queries on
    // dev surface connect; including them in /snapshot means a fresh
    // /snapshot request also re-populates the dropdowns without a
    // reconnect cycle.
    {
        OSCMessage m("/synth/dexed/bank/names");
        for (int b = 0; b < tdsp::dexed::kNumBanks; ++b) {
            m.add(tdsp::dexed::bankName(b));
        }
        reply.add(m);
    }
    {
        OSCMessage m("/synth/dexed/voice/names");
        m.add(g_dexedBank);
        for (int v = 0; v < tdsp::dexed::kVoicesPerBank; ++v) {
            char name[tdsp::dexed::kVoiceNameBufBytes] = {0};
            tdsp::dexed::copyVoiceName(g_dexedBank, v, name, sizeof(name));
            m.add(name);
        }
        reply.add(m);
    }

    // Codec panel state (TAC5212 register read-backs).
    g_codecPanel.snapshot(reply);
}

// ============================================================================
// Codec init — typed lib/TAC5212 only, no writeRegister fallback
// ============================================================================

// Bring the TLV320ADC6140 down. It shares SHDNZ with the TAC5212 so it
// powers up alongside us; left awake, its always-on '125 buffer fights the
// TAC5212 on TDM slots 0..3 and produces full-scale white noise on the
// headphones (project_6140_buffer_contention memory). A bare SW_RESET
// returns it to POR sleep state, where it tri-states the TDM output.
static void shutdownAdc6140() {
    Wire.beginTransmission(ADC6140_I2C_ADDRESS);
    if (Wire.endTransmission() != 0) {
        // No 6140 on this board variant — that's fine, nothing to silence.
        return;
    }
    Wire.beginTransmission(ADC6140_I2C_ADDRESS);
    Wire.write(0x01);  // SW_RESET register
    Wire.write(0x01);  // self-clearing reset trigger
    Wire.endTransmission();
    delay(20);
}

// Pulse SHDNZ low, then high, exactly once. After this, codec drivers may
// only use SW_RESET via I2C — re-toggling SHDNZ at runtime would also reset
// the 6140 (shared pin) which we want to stay quiet (see shutdownAdc6140).
static void hardResetCodecPower() {
    pinMode(TAC5212_EN_PIN, OUTPUT);
    digitalWrite(TAC5212_EN_PIN, LOW);
    delay(5);  // > 100 µs SHDNZ low spec, generous
    digitalWrite(TAC5212_EN_PIN, HIGH);
    delay(10); // let internal supplies settle
}

// Configure the TAC5212 entirely through the typed lib. The sequence
// mirrors what setupCodecHandRolled() did in the production project, but
// every step is a typed method call now.
//
// Boot gate: DAC volume is left at mute (0) at the end of this function.
// setup() releases the gate by calling g_codecPanel.unmuteOutput() after
// the audio graph has had time to settle.
static void setupCodec() {
    Serial.println("Initializing TAC5212 (typed lib path)...");

    // begin() probes I2C, runs SW_RESET, and writes SLEEP_CFG = wake. If
    // I2C NACKs we surface the error and bail — there's nothing useful
    // we can do without the codec.
    tac5212::Result r = g_codec.begin(TAC5212_I2C_ADDRESS);
    if (r.isError()) {
        Serial.print("  begin failed: ");
        Serial.println(r.message ? r.message : "(unknown)");
        return;
    }

    // Audio serial interface: TDM, 32-bit slots, FSYNC normal, BCLK
    // inverted, bus-error recovery on. setSerialFormat() also writes
    // INTF_CFG1 (DOUT carries PASI TX) and INTF_CFG2 (DIN enabled), so
    // this single call sets up the entire serial path.
    tac5212::TAC5212::SerialFormat sf;
    // (defaults already match what we need: TDM/32/normal/inverted/recover)
    g_codec.setSerialFormat(sf);

    // Slot offsets — 1 BCLK after FSYNC. Standard for TDM with 1-bit
    // FSYNC width, matches the SAI1 master config.
    g_codec.setRxSlotOffset(1);
    g_codec.setTxSlotOffset(1);

    // Slot routing:
    //   RX CH1 (DAC L1 -> OUT1) <- slot 0
    //   RX CH2 (DAC L2 -> OUT2) <- slot 1
    //   TX CH1 (ADC CH1)        -> slot 0
    //   TX CH2 (ADC CH2)        -> slot 1
    g_codec.setRxChannelSlot(1, 0);
    g_codec.setRxChannelSlot(2, 1);
    g_codec.setTxChannelSlot(1, 0);
    g_codec.setTxChannelSlot(2, 1);

    // ADC inputs: single-ended on INxP only. INxM is "don't care" — this
    // avoids cross-bleed when the TRS ring on a stereo cable ties INxM to
    // the other channel's INxP.
    g_codec.adc(1).setMode(tac5212::AdcMode::SingleEndedInp);
    g_codec.adc(2).setMode(tac5212::AdcMode::SingleEndedInp);

    // On-board stereo PDM mic. Lib's pdm().setEnable() configures
    // GPIO1 = PDM clock out, GPI1 = PDM data input, INTF_CFG4 routes
    // GPI1 -> PDM channels 3+4, and CH_EN flips IN_CH3 + IN_CH4 on.
    // The TX channel slot mapping has to be set explicitly so the mic
    // data lands on the TDM slots our F32 audio graph expects.
    g_codec.setTxChannelSlot(3, 2);   // PDM mic L  -> TDM slot 2
    g_codec.setTxChannelSlot(4, 3);   // PDM mic R  -> TDM slot 3
    g_codec.pdm().setEnable(true);

    // DAC outputs: headphone driver, mono single-ended on OUTxP. Matches
    // the loopback test rig's known-good config and keeps the headphone
    // jack at safe POR-compatible 0 dB level control.
    g_codec.out(1).setMode(tac5212::OutMode::HpDriver);
    g_codec.out(2).setMode(tac5212::OutMode::HpDriver);

    // Boot gate: bring DAC sub-channels up muted. The Out::setDvol path
    // writes 0 (mute code) when dB <= -100.5, which is exactly what the
    // chip wants. setup() releases the gate via panel.unmuteOutput()
    // once the audio graph is up.
    g_codec.out(1).setDvol(-128.0f);
    g_codec.out(2).setDvol(-128.0f);

    // Channel enable + power-up. inMask = IN_CH1+IN_CH2 (analog ADC) +
    // IN_CH3+IN_CH4 (PDM mic L+R) = nibble 0xF. outMask = OUT_CH1+OUT_CH2.
    //
    // Note: pdm().setEnable(true) above ALSO sets IN_CH3+IN_CH4 via
    // read-modify-write, but setChannelEnable() does a full-byte write
    // that would clobber those bits if we passed 0xC. Including them
    // in the mask here is what actually gates whether the on-board
    // PDM mics produce audio on TDM slots 2/3.
    g_codec.setChannelEnable(/*inMask=*/0xF, /*outMask=*/0xC);
    g_codec.powerAdc(true);
    g_codec.powerDac(true);
    // MICBIAS stays OFF: on this hardware the on-board PDM mics use
    // their own digital interface and don't need the analog mic-bias
    // rail. Production happened to set MICBIAS via the catch-all
    // PWR_CFG = 0xE0 write but it wasn't load-bearing for PDM audio
    // — what was actually missing was IN_CH3 / IN_CH4 in CH_EN
    // (fixed in 5b7dc32). External condenser mics on a future XLR
    // path will engage MICBIAS via /codec/tac5212/micbias/enable.
    delay(100);  // let analog blocks settle before any audio hits the DAC

    // Arm DSP_AVDD_SEL before any DSP-resident block (limiter, BOP, DRC)
    // can be enabled. Required HIGH before turning any of those on; the
    // POR value is reserved and produces a squeal if a DSP block is
    // engaged in that state.
    g_codec.setDspAvddSelect(true);

    // ----- ADC + PDM DSP -----
    //
    // The chip has one DSP stage shared by all input channels — both
    // analog ADC (CH1, CH2) and PDM mics (CH3, CH4) feed through the
    // same decimation, HPF, and biquad chain inside the codec.
    //
    // HPF: 12 Hz cutoff, on by default. Cuts room rumble and any DC
    //      offset before it reaches the F32 graph. Free, no CPU cost.
    // Decimation filter: linear phase. Cleanest passband; the chip
    //      offers low-latency / ultra-low-latency modes for live
    //      monitoring paths but linear phase is the audio-quality
    //      default.
    // Biquads: 3 slots per channel, all initialized to bypass. The
    //      chip's BQ_CFG default is 2 — we bump to 3 for the maximum
    //      slot count the dev surface can address. BQ_BASE = 0x08
    //      per references/tac5212.csv §8.2.7 (the TI register table).
    g_codec.setAdcHpf(true);
    g_codec.setAdcDecimationFilter(tac5212::AdcDecimationFilter::LinearPhase);

    // clearBiquad writes BiquadCoeffs::bypass() which is unity
    // passthrough and matches chip POR — audio flows unchanged but
    // the slots are explicitly programmed so the dev surface can
    // drive real EQ coefs without inheriting garbage from a prior boot.
    for (uint8_t ch = 1; ch <= 2; ++ch) {
        for (uint8_t idx = 1; idx <= 3; ++idx) {
            g_codec.adc(ch).clearBiquad(idx);
        }
    }
    g_codec.setAdcBiquadsPerChannel(3);

    // ----- DAC DSP -----
    g_codec.setDacInterpolationFilter(tac5212::InterpFilter::LinearPhase);
    for (uint8_t out = 1; out <= 2; ++out) {
        for (uint8_t idx = 1; idx <= 3; ++idx) {
            g_codec.out(out).clearBiquad(idx);
        }
    }
    g_codec.setDacBiquadsPerChannel(3);

    // DAC distortion limiter intentionally OFF.
    //
    // Datasheet §8.2 says "TI recommends using the PPC3 GUI for
    // configuring the programmable coefficients" and never publishes
    // the Q-format encoding for the limiter coefs. The Table 8-219
    // POR defaults are loaded in coef RAM at boot but are NOT safe
    // to engage on real audio — interpreted as Q1.31 the release TC
    // lands around 24 kHz, putting the gain-modulation loop in the
    // audio band, and the chip squeals at its low (~-39 dBFS)
    // threshold whenever EN_DISTORTION is flipped. Empirically this
    // also bootlooped the Teensy when enabled in setupCodec().
    //
    // Re-enabling requires importing PPC3-generated coefs into a new
    // LimiterCoeffs preset (see memory: TAC5212 Limiter needs PPC3).

    Serial.print("DEV_STS0: 0x");
    Serial.println(g_codec.readRegister(0, 0x79), HEX);
    Serial.println("Codec ready: ADC HPF on, ADC + DAC biquads (3/ch, bypass coefs), DAC limiter off");
}

// ============================================================================
// OSC ingress — F32 mixer handlers + delegate to dispatcher for codec routes
// ============================================================================

// Helper: parse "/ch/NN/..." and return the channel number (1..2 expected
// for Phase 2) or 0 if the pattern doesn't match.
static int parseChannelN(const char *address, const char *suffix) {
    // address is e.g. "/ch/01/mix/fader"; suffix is "/mix/fader"
    if (strncmp(address, "/ch/", 4) != 0) return 0;
    const char *p = address + 4;
    if (!isdigit(*p) || !isdigit(*(p + 1))) return 0;
    const int n = (p[0] - '0') * 10 + (p[1] - '0');
    if (strcmp(p + 2, suffix) != 0) return 0;
    return n;
}

static void onOscMessage(OSCMessage &msg, void *userData) {
    (void)userData;

    char address[64];
    int addrLen = msg.getAddress(address, 0, sizeof(address) - 1);
    if (addrLen < 0) addrLen = 0;
    address[addrLen] = '\0';

    // ---- /snapshot ----
    if (strcmp(address, "/snapshot") == 0) {
        OSCBundle reply;
        broadcastSnapshot(reply);
        if (reply.size() > 0) g_transport.sendBundle(reply);
        return;
    }

    // ---- /info ---- (X32-shape: product, phase, model, fw version)
    if (strcmp(address, "/info") == 0) {
        OSCBundle reply;
        OSCMessage m("/info");
        m.add("t-dsp_f32_audio_shield");
        m.add("phase3");
        m.add("TAC5212-Teensy41");
        m.add("fw 0.2.0");
        reply.add(m);
        g_transport.sendBundle(reply);
        return;
    }
    // ---- /status ---- (state, transport, protocol version)
    if (strcmp(address, "/status") == 0) {
        OSCBundle reply;
        OSCMessage m("/status");
        m.add("active");
        m.add("USB:CDC");
        m.add("1.0.0");
        reply.add(m);
        g_transport.sendBundle(reply);
        return;
    }

    // Channel range covered by g_chFader / g_chOn arrays (1..6 today —
    // USB L/R + Line L/R + Mic L/R). XLR 7..10 plug in once the
    // ADC6140 is wired.
    constexpr int kMaxChannel = 6;

    // ---- /config/chlink/N-M i ---- (X32 stereo-link convention)
    //
    // Toggling link-on does NOT retroactively mirror current values —
    // it only affects subsequent writes (X32 OSC PDF, p. 25). The echo
    // is just the stored flag.
    if (strncmp(address, "/config/chlink/", 15) == 0) {
        int idx = chlinkPairIndex(address + 15);
        if (idx >= 0) {
            OSCBundle reply;
            if (msg.size() > 0 && msg.isInt(0)) {
                g_chLink[idx] = msg.getInt(0) != 0;
            }
            char buf[32];
            snprintf(buf, sizeof(buf), "/config/chlink/%s", address + 15);
            OSCMessage echo(buf);
            echo.add((int)(g_chLink[idx] ? 1 : 0));
            reply.add(echo);
            g_transport.sendBundle(reply);
            return;
        }
    }

    // ---- /ch/NN/config/name s ----  (does NOT mirror; per X32 convention)
    {
        int ch = parseChannelN(address, "/config/name");
        if (ch >= 1 && ch <= kMaxChannel) {
            OSCBundle reply;
            if (msg.size() > 0 && msg.isString(0)) {
                msg.getString(0, g_chName[ch], sizeof(g_chName[ch]) - 1);
            }
            char buf[32];
            snprintf(buf, sizeof(buf), "/ch/%02d/config/name", ch);
            OSCMessage echo(buf); echo.add(g_chName[ch]); reply.add(echo);
            g_transport.sendBundle(reply);
            return;
        }
    }

    // ---- /ch/NN/mix/fader f ----
    //
    // Input is X32-shape 0..1 fader value. We snap to the 1024-step
    // grid before applying so the echo reports the value the audio
    // graph is actually using (clients can't tell their float diverged
    // from the chip's quantization step).
    //
    // Stereo-link mirror: if /config/chlink/N-M is on for this pair,
    // the partner channel gets the same write applied + echoed back
    // server-side. Both echoes ride in the same reply bundle so the
    // dev surface sees them atomically.
    {
        int ch = parseChannelN(address, "/mix/fader");
        if (ch >= 1 && ch <= kMaxChannel) {
            OSCBundle reply;
            if (msg.size() > 0 && msg.isFloat(0)) {
                float v = tdsp::x32::quantizeFader(msg.getFloat(0));
                g_chFader[ch] = v;
                applyChannelFader(ch);
                int partner = linkPartner(ch);
                if (partner > 0) {
                    g_chFader[partner] = v;
                    applyChannelFader(partner);
                }
            }
            char buf[32];
            snprintf(buf, sizeof(buf), "/ch/%02d/mix/fader", ch);
            OSCMessage echo(buf); echo.add(g_chFader[ch]); reply.add(echo);
            int partner = linkPartner(ch);
            if (partner > 0) {
                snprintf(buf, sizeof(buf), "/ch/%02d/mix/fader", partner);
                OSCMessage echo2(buf); echo2.add(g_chFader[partner]); reply.add(echo2);
            }
            g_transport.sendBundle(reply);
            return;
        }
    }

    // ---- /ch/NN/mix/on i ---- (mute, mirrored across linked pair)
    {
        int ch = parseChannelN(address, "/mix/on");
        if (ch >= 1 && ch <= kMaxChannel) {
            OSCBundle reply;
            if (msg.size() > 0 && msg.isInt(0)) {
                bool on = msg.getInt(0) != 0;
                g_chOn[ch] = on;
                applyChannelFader(ch);
                int partner = linkPartner(ch);
                if (partner > 0) {
                    g_chOn[partner] = on;
                    applyChannelFader(partner);
                }
            }
            char buf[32];
            snprintf(buf, sizeof(buf), "/ch/%02d/mix/on", ch);
            OSCMessage echo(buf); echo.add((int)(g_chOn[ch] ? 1 : 0)); reply.add(echo);
            int partner = linkPartner(ch);
            if (partner > 0) {
                snprintf(buf, sizeof(buf), "/ch/%02d/mix/on", partner);
                OSCMessage echo2(buf); echo2.add((int)(g_chOn[partner] ? 1 : 0)); reply.add(echo2);
            }
            g_transport.sendBundle(reply);
            return;
        }
    }

    // ---- /main/st/mix/fader f ----  (linked-mode master, writes both L and R)
    //
    // X32 convention: when /main/st/mix/link is on (always on this
    // hardware — there's a single stereo bus), this is the canonical
    // "master fader" leaf. /main/st/mix/faderL and /faderR are for
    // unlinked control. Snap on input, mirror to both sides.
    if (strcmp(address, "/main/st/mix/fader") == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isFloat(0)) {
            float v = tdsp::x32::quantizeFader(msg.getFloat(0));
            g_mainFaderL = v;
            g_mainFaderR = v;
            applyMain();
        }
        OSCMessage echo("/main/st/mix/fader"); echo.add(g_mainFaderL); reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }
    // ---- /main/st/mix/link i ----  (X32 leaf; always 1 on this hardware)
    if (strcmp(address, "/main/st/mix/link") == 0) {
        OSCBundle reply;
        OSCMessage echo("/main/st/mix/link"); echo.add((int)1); reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }

    // ---- /main/st/mix/faderL f ----
    if (strcmp(address, "/main/st/mix/faderL") == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isFloat(0)) {
            g_mainFaderL = tdsp::x32::quantizeFader(msg.getFloat(0));
            applyMain();
        }
        OSCMessage echo("/main/st/mix/faderL"); echo.add(g_mainFaderL); reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }
    if (strcmp(address, "/main/st/mix/faderR") == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isFloat(0)) {
            g_mainFaderR = tdsp::x32::quantizeFader(msg.getFloat(0));
            applyMain();
        }
        OSCMessage echo("/main/st/mix/faderR"); echo.add(g_mainFaderR); reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }
    if (strcmp(address, "/main/st/mix/on") == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isInt(0)) {
            g_mainOn = msg.getInt(0) != 0;
            applyMain();
        }
        OSCMessage echo("/main/st/mix/on"); echo.add((int)(g_mainOn ? 1 : 0)); reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }

    // ---- /midi/viz/on i ----  (engage /midi/note broadcast)
    if (strcmp(address, "/midi/viz/on") == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isInt(0)) {
            g_midiVizSink.enable(msg.getInt(0) != 0);
        }
        OSCMessage echo("/midi/viz/on");
        echo.add((int)(g_midiVizSink.enabled() ? 1 : 0));
        reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }

    // ---- /synth/bus/mix/fader f ----  (mixer's "synth" strip; X32 law)
    if (strcmp(address, "/synth/bus/mix/fader") == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isFloat(0)) {
            g_synthBusVolume = tdsp::x32::quantizeFader(msg.getFloat(0));
            applySynthBusGain();
        }
        OSCMessage echo("/synth/bus/mix/fader"); echo.add(g_synthBusVolume); reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }
    // ---- /synth/bus/mix/on i ----
    if (strcmp(address, "/synth/bus/mix/on") == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isInt(0)) {
            g_synthBusOn = msg.getInt(0) != 0;
            applySynthBusGain();
        }
        OSCMessage echo("/synth/bus/mix/on"); echo.add((int)(g_synthBusOn ? 1 : 0)); reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }

    // ---- /synth/active i ---- (0..3, picks the audible engine)
    //
    // Switching is exclusive: setActive() panics + zero-gains the
    // outgoing slot, then restores gain on the incoming slot via its
    // observed on/volume state. Out-of-range or unset-slot indices
    // are rejected (the active index doesn't change).
    if (strcmp(address, "/synth/active") == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isInt(0)) {
            int idx = msg.getInt(0);
            g_synthSwitcher.setActive(idx);
        }
        OSCMessage echo("/synth/active");
        echo.add(g_synthSwitcher.active());
        reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }

    // ---- /synth/slots ---- (read-only enumeration of slot ids + display names)
    //
    // Returns one packed "id|displayName" string per slot so the dev
    // surface can render the picker without per-slot follow-up reads.
    // Empty slots return "silent|(empty)".
    if (strcmp(address, "/synth/slots") == 0) {
        OSCBundle reply;
        OSCMessage echo("/synth/slots");
        for (int i = 0; i < tdsp_synth::SynthSwitcher::kMaxSlots; ++i) {
            auto *s = g_synthSwitcher.slot(i);
            char buf[48];
            if (s) snprintf(buf, sizeof(buf), "%s|%s", s->id(), s->displayName());
            else   snprintf(buf, sizeof(buf), "silent|(empty)");
            echo.add(buf);
        }
        reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }

    // ---- /synth/sampler/mix/fader f ----  (per-slot fader, X32 law)
    if (strcmp(address, "/synth/sampler/mix/fader") == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isFloat(0)) {
            g_multisampleSlot.setVolume(msg.getFloat(0));
        }
        OSCMessage echo("/synth/sampler/mix/fader");
        echo.add(g_multisampleSlot.volume());
        reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }
    // ---- /synth/sampler/mix/on i ----
    if (strcmp(address, "/synth/sampler/mix/on") == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isInt(0)) {
            g_multisampleSlot.setOn(msg.getInt(0) != 0);
        }
        OSCMessage echo("/synth/sampler/mix/on");
        echo.add((int)(g_multisampleSlot.on() ? 1 : 0));
        reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }
    // ---- /synth/sampler/midi/ch i ----  (0 = omni, 1..16 = single)
    if (strcmp(address, "/synth/sampler/midi/ch") == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isInt(0)) {
            int ch = msg.getInt(0);
            if (ch < 0 || ch > 16) ch = 0;
            g_multisampleSlot.panic();  // release notes on the old channel
            g_multisampleSlot.setListenChannel((uint8_t)ch);
        }
        OSCMessage echo("/synth/sampler/midi/ch");
        echo.add((int)g_multisampleSlot.listenChannel());
        reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }
    // ---- /synth/sampler/info ----  (read-only: bank name + sample count)
    if (strcmp(address, "/synth/sampler/info") == 0) {
        OSCBundle reply;
        OSCMessage echo("/synth/sampler/info");
        echo.add(g_multisampleSlot.bankName());
        echo.add((int)g_multisampleSlot.numSamples());
        echo.add((int)g_multisampleSlot.numReleaseSamples());
        reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }

    // ---- /synth/mpe/* — MPE-aware VA engine (slot 3) -----------------
    //
    // Read/write/echo pattern, ported from the legacy
    // projects/t-dsp_tac5212_audio_shield_adaptor wiring. Existing
    // dispatcher.ts (state.mpe.*, dispatcher.setMpe*) already targets
    // these addresses, so the dev surface populates without changes.
    if (strcmp(address, "/synth/mpe/mix/fader") == 0 ||
        strcmp(address, "/synth/mpe/volume")    == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isFloat(0)) {
            g_mpeSlot.setVolume(msg.getFloat(0));
        }
        OSCMessage echo("/synth/mpe/volume");
        echo.add(g_mpeSlot.volume());
        reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }
    if (strcmp(address, "/synth/mpe/mix/on") == 0 ||
        strcmp(address, "/synth/mpe/on")     == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isInt(0)) {
            g_mpeSlot.setOn(msg.getInt(0) != 0);
        }
        OSCMessage echo("/synth/mpe/on");
        echo.add((int)(g_mpeSlot.on() ? 1 : 0));
        reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }
    if (strcmp(address, "/synth/mpe/midi/master") == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isInt(0)) {
            int ch = msg.getInt(0);
            if (ch < 0 || ch > 16) ch = 0;
            g_mpeSlot.setMasterChannel((uint8_t)ch);
        }
        OSCMessage echo("/synth/mpe/midi/master");
        echo.add((int)g_mpeSlot.masterChannel());
        reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }
    if (strcmp(address, "/synth/mpe/attack") == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isFloat(0)) {
            g_mpeSlot.sink()->setAttack(msg.getFloat(0));
        }
        OSCMessage echo("/synth/mpe/attack");
        echo.add(g_mpeSlot.sink()->attack());
        reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }
    if (strcmp(address, "/synth/mpe/release") == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isFloat(0)) {
            g_mpeSlot.sink()->setRelease(msg.getFloat(0));
        }
        OSCMessage echo("/synth/mpe/release");
        echo.add(g_mpeSlot.sink()->release());
        reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }
    if (strcmp(address, "/synth/mpe/waveform") == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isInt(0)) {
            int w = msg.getInt(0);
            if (w < 0) w = 0;
            if (w > 3) w = 3;
            g_mpeSlot.sink()->setWaveform((uint8_t)w);
        }
        OSCMessage echo("/synth/mpe/waveform");
        echo.add((int)g_mpeSlot.sink()->waveform());
        reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }
    if (strcmp(address, "/synth/mpe/filter/cutoff") == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isFloat(0)) {
            g_mpeSlot.sink()->setFilterCutoff(msg.getFloat(0));
        }
        OSCMessage echo("/synth/mpe/filter/cutoff");
        echo.add(g_mpeSlot.sink()->filterCutoff());
        reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }
    if (strcmp(address, "/synth/mpe/filter/resonance") == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isFloat(0)) {
            g_mpeSlot.sink()->setFilterResonance(msg.getFloat(0));
        }
        OSCMessage echo("/synth/mpe/filter/resonance");
        echo.add(g_mpeSlot.sink()->filterResonance());
        reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }
    if (strcmp(address, "/synth/mpe/lfo/rate") == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isFloat(0)) {
            g_mpeSlot.sink()->setLfoRate(msg.getFloat(0));
        }
        OSCMessage echo("/synth/mpe/lfo/rate");
        echo.add(g_mpeSlot.sink()->lfoRate());
        reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }
    if (strcmp(address, "/synth/mpe/lfo/depth") == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isFloat(0)) {
            g_mpeSlot.sink()->setLfoDepth(msg.getFloat(0));
        }
        OSCMessage echo("/synth/mpe/lfo/depth");
        echo.add(g_mpeSlot.sink()->lfoDepth());
        reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }
    if (strcmp(address, "/synth/mpe/lfo/dest") == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isInt(0)) {
            g_mpeSlot.sink()->setLfoDest((uint8_t)msg.getInt(0));
        }
        OSCMessage echo("/synth/mpe/lfo/dest");
        echo.add((int)g_mpeSlot.sink()->lfoDest());
        reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }
    if (strcmp(address, "/synth/mpe/lfo/waveform") == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isInt(0)) {
            g_mpeSlot.sink()->setLfoWaveform((uint8_t)msg.getInt(0));
        }
        OSCMessage echo("/synth/mpe/lfo/waveform");
        echo.add((int)g_mpeSlot.sink()->lfoWaveform());
        reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }

    // ---- /synth/neuro/* — reese / DnB neuro bass (slot 4) -----------
    //
    // Read/write/echo pattern, ported from the legacy
    // projects/t-dsp_tac5212_audio_shield_adaptor wiring. Existing
    // dispatcher.ts (state.neuro.*, dispatcher.setNeuro*) already
    // targets these paths, so the dev surface populates without changes.
    // /mix/fader and /mix/on aliases mirror the slot housekeeping
    // convention used by the sampler/MPE/Supersaw slots.
    //
    // /synth/neuro/stink/* (multiband destruction chain) is intentionally
    // out of scope for this slot rebuild — see planning/synth-slot-rebuild/.
    if (strcmp(address, "/synth/neuro/mix/fader") == 0 ||
        strcmp(address, "/synth/neuro/volume")    == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isFloat(0)) {
            g_neuroSlot.setVolume(msg.getFloat(0));
        }
        OSCMessage echo("/synth/neuro/volume");
        echo.add(g_neuroSlot.volume());
        reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }
    if (strcmp(address, "/synth/neuro/mix/on") == 0 ||
        strcmp(address, "/synth/neuro/on")     == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isInt(0)) {
            g_neuroSlot.setOn(msg.getInt(0) != 0);
        }
        OSCMessage echo("/synth/neuro/on");
        echo.add((int)(g_neuroSlot.on() ? 1 : 0));
        reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }
    if (strcmp(address, "/synth/neuro/midi/ch") == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isInt(0)) {
            int ch = msg.getInt(0);
            if (ch < 0 || ch > 16) ch = 0;
            g_neuroSlot.setListenChannel((uint8_t)ch);
        }
        OSCMessage echo("/synth/neuro/midi/ch");
        echo.add((int)g_neuroSlot.listenChannel());
        reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }
#define NEURO_FLOAT_HANDLER(addr, setFn, getExpr)                         \
    if (strcmp(address, addr) == 0) {                                     \
        OSCBundle reply;                                                  \
        if (msg.size() > 0 && msg.isFloat(0)) {                           \
            g_neuroSlot.sink()->setFn(msg.getFloat(0));                   \
        }                                                                 \
        OSCMessage echo(addr);                                            \
        echo.add(getExpr);                                                \
        reply.add(echo);                                                  \
        g_transport.sendBundle(reply);                                    \
        return;                                                           \
    }
    NEURO_FLOAT_HANDLER("/synth/neuro/attack",           setAttack,          g_neuroSlot.sink()->attack())
    NEURO_FLOAT_HANDLER("/synth/neuro/release",          setRelease,         g_neuroSlot.sink()->release())
    NEURO_FLOAT_HANDLER("/synth/neuro/detune",           setDetuneCents,     g_neuroSlot.sink()->detuneCents())
    NEURO_FLOAT_HANDLER("/synth/neuro/sub",              setSubLevel,        g_neuroSlot.sink()->subLevel())
    NEURO_FLOAT_HANDLER("/synth/neuro/osc3",             setOsc3Level,       g_neuroSlot.sink()->osc3Level())
    NEURO_FLOAT_HANDLER("/synth/neuro/filter/cutoff",    setFilterCutoff,    g_neuroSlot.sink()->filterCutoff())
    NEURO_FLOAT_HANDLER("/synth/neuro/filter/resonance", setFilterResonance, g_neuroSlot.sink()->filterResonance())
    NEURO_FLOAT_HANDLER("/synth/neuro/lfo/rate",         setLfoRate,         g_neuroSlot.sink()->lfoRate())
    NEURO_FLOAT_HANDLER("/synth/neuro/lfo/depth",        setLfoDepth,        g_neuroSlot.sink()->lfoDepth())
    NEURO_FLOAT_HANDLER("/synth/neuro/portamento",       setPortamentoMs,    g_neuroSlot.sink()->portamentoMs())
#undef NEURO_FLOAT_HANDLER
    if (strcmp(address, "/synth/neuro/lfo/dest") == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isInt(0)) {
            g_neuroSlot.sink()->setLfoDest((uint8_t)msg.getInt(0));
        }
        OSCMessage echo("/synth/neuro/lfo/dest");
        echo.add((int)g_neuroSlot.sink()->lfoDest());
        reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }
    if (strcmp(address, "/synth/neuro/lfo/waveform") == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isInt(0)) {
            g_neuroSlot.sink()->setLfoWaveform((uint8_t)msg.getInt(0));
        }
        OSCMessage echo("/synth/neuro/lfo/waveform");
        echo.add((int)g_neuroSlot.sink()->lfoWaveform());
        reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }

    // ---- /synth/supersaw/* — JP-8000 lead (slot 6) ------------------
    //
    // Read/write/echo pattern, ported from the legacy
    // projects/t-dsp_tac5212_audio_shield_adaptor wiring. Existing
    // dispatcher.ts (state.supersaw.*, dispatcher.setSupersaw*) already
    // targets these paths, so the dev surface populates without changes.
    // /mix/fader and /mix/on aliases mirror the slot housekeeping
    // convention used by the sampler/MPE slots.
    if (strcmp(address, "/synth/supersaw/mix/fader") == 0 ||
        strcmp(address, "/synth/supersaw/volume")    == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isFloat(0)) {
            g_supersawSlot.setVolume(msg.getFloat(0));
        }
        OSCMessage echo("/synth/supersaw/volume");
        echo.add(g_supersawSlot.volume());
        reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }
    if (strcmp(address, "/synth/supersaw/mix/on") == 0 ||
        strcmp(address, "/synth/supersaw/on")     == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isInt(0)) {
            g_supersawSlot.setOn(msg.getInt(0) != 0);
        }
        OSCMessage echo("/synth/supersaw/on");
        echo.add((int)(g_supersawSlot.on() ? 1 : 0));
        reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }
    if (strcmp(address, "/synth/supersaw/midi/ch") == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isInt(0)) {
            int ch = msg.getInt(0);
            if (ch < 0 || ch > 16) ch = 0;
            g_supersawSlot.setListenChannel((uint8_t)ch);
        }
        OSCMessage echo("/synth/supersaw/midi/ch");
        echo.add((int)g_supersawSlot.listenChannel());
        reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }
    // Engine knobs — every leaf is float, set-and-echo, mutating
    // SupersawSink directly (it clamps and re-applies internally).
#define SS_FLOAT_HANDLER(addr, setFn, getExpr)                            \
    if (strcmp(address, addr) == 0) {                                     \
        OSCBundle reply;                                                  \
        if (msg.size() > 0 && msg.isFloat(0)) {                           \
            g_supersawSlot.sink()->setFn(msg.getFloat(0));                \
        }                                                                 \
        OSCMessage echo(addr);                                            \
        echo.add(getExpr);                                                \
        reply.add(echo);                                                  \
        g_transport.sendBundle(reply);                                    \
        return;                                                           \
    }
    SS_FLOAT_HANDLER("/synth/supersaw/detune",      setDetuneCents,  g_supersawSlot.sink()->detuneCents())
    SS_FLOAT_HANDLER("/synth/supersaw/mix_center",  setMixCenter,    g_supersawSlot.sink()->mixCenter())
    SS_FLOAT_HANDLER("/synth/supersaw/cutoff",      setCutoff,       g_supersawSlot.sink()->cutoff())
    SS_FLOAT_HANDLER("/synth/supersaw/resonance",   setResonance,    g_supersawSlot.sink()->resonance())
    SS_FLOAT_HANDLER("/synth/supersaw/attack",      setAttack,       g_supersawSlot.sink()->attack())
    SS_FLOAT_HANDLER("/synth/supersaw/decay",       setDecay,        g_supersawSlot.sink()->decay())
    SS_FLOAT_HANDLER("/synth/supersaw/sustain",     setSustain,      g_supersawSlot.sink()->sustain())
    SS_FLOAT_HANDLER("/synth/supersaw/release",     setRelease,      g_supersawSlot.sink()->release())
    SS_FLOAT_HANDLER("/synth/supersaw/portamento",  setPortamentoMs, g_supersawSlot.sink()->portamentoMs())
#undef SS_FLOAT_HANDLER
    // /chorus_depth has no firmware FX yet — keep a stateful echo so the
    // dispatcher's setSupersawChorusDepth round-trips and the panel's
    // slider position survives a reconnect.
    if (strcmp(address, "/synth/supersaw/chorus_depth") == 0) {
        static float s_supersawChorus = 0.5f;
        OSCBundle reply;
        if (msg.size() > 0 && msg.isFloat(0)) {
            float v = msg.getFloat(0);
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            s_supersawChorus = v;
        }
        OSCMessage echo("/synth/supersaw/chorus_depth");
        echo.add(s_supersawChorus);
        reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }

    // ---- /synth/dexed/bank/names ----  (dev surface populates the bank dropdown)
    if (strcmp(address, "/synth/dexed/bank/names") == 0) {
        OSCMessage m("/synth/dexed/bank/names");
        for (int b = 0; b < tdsp::dexed::kNumBanks; ++b) {
            m.add(tdsp::dexed::bankName(b));
        }
        OSCBundle reply;
        reply.add(m);
        g_transport.sendBundle(reply);
        return;
    }
    // ---- /synth/dexed/voice/names i ----  (32 voice names for the given bank)
    if (strcmp(address, "/synth/dexed/voice/names") == 0 && msg.size() >= 1 && msg.isInt(0)) {
        int bank = msg.getInt(0);
        if (bank < 0)                       bank = 0;
        if (bank >= tdsp::dexed::kNumBanks) bank = tdsp::dexed::kNumBanks - 1;
        OSCMessage m("/synth/dexed/voice/names");
        m.add(bank);
        for (int v = 0; v < tdsp::dexed::kVoicesPerBank; ++v) {
            char name[tdsp::dexed::kVoiceNameBufBytes] = {0};
            tdsp::dexed::copyVoiceName(bank, v, name, sizeof(name));
            m.add(name);
        }
        OSCBundle reply;
        reply.add(m);
        g_transport.sendBundle(reply);
        return;
    }

    // ---- /arp/on i ----
    if (strcmp(address, "/arp/on") == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isInt(0)) {
            g_arpFilter.setEnabled(msg.getInt(0) != 0);
        }
        OSCMessage echo("/arp/on");
        echo.add((int)(g_arpFilter.enabled() ? 1 : 0));
        reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }

    // ---- /synth/dexed/mix/fader f ---- (X32-shape engine fader)
    if (strcmp(address, "/synth/dexed/mix/fader") == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isFloat(0)) applyDexedVolume(msg.getFloat(0));
        OSCMessage echo("/synth/dexed/mix/fader"); echo.add(g_dexedVolume); reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }
    // ---- /synth/dexed/mix/on i ----
    if (strcmp(address, "/synth/dexed/mix/on") == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isInt(0)) applyDexedOn(msg.getInt(0) != 0);
        OSCMessage echo("/synth/dexed/mix/on"); echo.add((int)(g_dexedOn ? 1 : 0)); reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }
    // ---- /synth/dexed/voice i i ----  (bank, voice)
    if (strcmp(address, "/synth/dexed/voice") == 0) {
        OSCBundle reply;
        if (msg.size() >= 2 && msg.isInt(0) && msg.isInt(1)) {
            applyDexedVoice(msg.getInt(0), msg.getInt(1));
        }
        char name[tdsp::dexed::kVoiceNameBufBytes] = {0};
        tdsp::dexed::copyVoiceName(g_dexedBank, g_dexedVoice, name, sizeof(name));
        OSCMessage echo("/synth/dexed/voice");
        echo.add(g_dexedBank);
        echo.add(g_dexedVoice);
        echo.add(name);
        reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }
    // ---- /synth/dexed/midi/ch i ----  (0 = omni, 1..16 = single)
    if (strcmp(address, "/synth/dexed/midi/ch") == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isInt(0)) {
            int ch = msg.getInt(0);
            if (ch < 0 || ch > 16) ch = 0;
            g_dexed.panic();  // release notes on the old channel
            g_dexedSink.setListenChannel((uint8_t)ch);
        }
        OSCMessage echo("/synth/dexed/midi/ch");
        echo.add((int)g_dexedSink.listenChannel());
        reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }

    // ---- /midi/note/in i i i ----
    // UI-originated note from the dev surface (on-screen keyboard). Feeds
    // the router the same way the USB host keyboard does, so downstream
    // sinks see UI notes and hardware notes identically.
    if (strcmp(address, "/midi/note/in") == 0 && msg.size() >= 3
        && msg.isInt(0) && msg.isInt(1) && msg.isInt(2)) {
        uint8_t note     = (uint8_t)msg.getInt(0);
        uint8_t velocity = (uint8_t)msg.getInt(1);
        uint8_t channel  = (uint8_t)msg.getInt(2);
        if (velocity > 0) g_midiRouter.handleNoteOn(channel, note, velocity);
        else              g_midiRouter.handleNoteOff(channel, note, 0);
        return;
    }

    // ---- /midi/cc/in i i i ----  (cc, value, channel)
    //
    // UI-originated control change from the dev surface — currently used
    // for the spacebar-as-sustain-pedal binding (CC#64). Same router
    // path as a hardware MIDI keyboard, so downstream synths react
    // identically (the active slot's MidiSink::onSustain fires on CC#64
    // for sampler / Dexed / any future engine).
    if (strcmp(address, "/midi/cc/in") == 0 && msg.size() >= 3
        && msg.isInt(0) && msg.isInt(1) && msg.isInt(2)) {
        uint8_t cc      = (uint8_t)msg.getInt(0);
        uint8_t value   = (uint8_t)msg.getInt(1);
        uint8_t channel = (uint8_t)msg.getInt(2);
        g_midiRouter.handleControlChange(channel, cc, value);
        return;
    }

    // ---- /main/st/hostvol/on, /main/st/hostvol/value ----
    if (strcmp(address, "/main/st/hostvol/on") == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isInt(0)) {
            g_hostvolEnable = msg.getInt(0) != 0;
            applyHostvol();
        }
        OSCMessage echo("/main/st/hostvol/on");
        echo.add((int)(g_hostvolEnable ? 1 : 0));
        reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }
    if (strcmp(address, "/main/st/hostvol/value") == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isFloat(0)) {
            float v = msg.getFloat(0);
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            g_hostvolValue = v;
            applyHostvol();
        }
        OSCMessage echo("/main/st/hostvol/value");
        echo.add(g_hostvolValue);
        reply.add(echo);
        g_transport.sendBundle(reply);
        return;
    }

    // ---- /main/st/eq/on, /eq/1/{type,f,g,q} ----
    //
    // X32 main bus EQ namespace. Today only band 1 is wired (high-shelf,
    // matches the production "Dull" preset 3 kHz / -12 dB). Bands 2..4
    // are reserved by the spec and would be added behind the same biquad
    // stage with .setCoefficients on additional stages.
    if (strcmp(address, "/main/st/eq/on") == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isInt(0)) {
            g_procShelfEnable = msg.getInt(0) != 0;
            applyProcShelf();
        }
        OSCMessage echo("/main/st/eq/on"); echo.add((int)(g_procShelfEnable ? 1 : 0));
        reply.add(echo); g_transport.sendBundle(reply); return;
    }
    if (strcmp(address, "/main/st/eq/1/f") == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isFloat(0)) {
            float f = msg.getFloat(0);
            if (f < 20.0f)    f = 20.0f;
            if (f > 20000.0f) f = 20000.0f;
            g_procShelfFreqHz = f;
            applyProcShelf();
        }
        OSCMessage echo("/main/st/eq/1/f"); echo.add(g_procShelfFreqHz);
        reply.add(echo); g_transport.sendBundle(reply); return;
    }
    if (strcmp(address, "/main/st/eq/1/g") == 0) {
        OSCBundle reply;
        if (msg.size() > 0 && msg.isFloat(0)) {
            float g = msg.getFloat(0);
            if (g < -15.0f) g = -15.0f;     // X32 EQ band gain range
            if (g >  15.0f) g =  15.0f;
            g_procShelfGainDb = g;
            applyProcShelf();
        }
        OSCMessage echo("/main/st/eq/1/g"); echo.add(g_procShelfGainDb);
        reply.add(echo); g_transport.sendBundle(reply); return;
    }
    // /main/st/eq/1/type and /q are accepted (write echoed) but pinned
    // to high-shelf (type 4) and Q ~0.7 (slope=1.0 in setHighShelf) for
    // now — the full PEQ/LCut/etc tree lands when more bands are added.
    if (strcmp(address, "/main/st/eq/1/type") == 0) {
        OSCBundle reply;
        OSCMessage echo("/main/st/eq/1/type"); echo.add((int)4);  // 4 = HShlv (X32 enum)
        reply.add(echo); g_transport.sendBundle(reply); return;
    }
    if (strcmp(address, "/main/st/eq/1/q") == 0) {
        OSCBundle reply;
        OSCMessage echo("/main/st/eq/1/q"); echo.add(0.707f);
        reply.add(echo); g_transport.sendBundle(reply); return;
    }

    // ---- /-cap/hostvol/{value,mute} ---- (read-only echo)
    if (strcmp(address, "/-cap/hostvol/value") == 0) {
        OSCBundle reply;
        OSCMessage echo("/-cap/hostvol/value"); echo.add(g_capHostvolValue);
        reply.add(echo); g_transport.sendBundle(reply); return;
    }
    if (strcmp(address, "/-cap/hostvol/mute") == 0) {
        OSCBundle reply;
        OSCMessage echo("/-cap/hostvol/mute"); echo.add((int)(g_capHostvolMute ? 1 : 0));
        reply.add(echo); g_transport.sendBundle(reply); return;
    }

    // ---- /-stat/* ---- (system status, read-only)
    //
    // CPU usage and audio block-pool stats refresh every read; the
    // dev surface should /subscribe for streaming or just poll
    // /-stat/cpu directly. AudioStream::cpu_usage_max returns scaled
    // percent * 100 — we emit it as a 0..100 float.
    if (strcmp(address, "/-stat/cpu") == 0) {
        OSCBundle reply;
        OSCMessage echo("/-stat/cpu");
        echo.add(AudioProcessorUsageMax());  // float, percent 0..100
        reply.add(echo); g_transport.sendBundle(reply); return;
    }
    if (strcmp(address, "/-stat/audio/blocks/f32") == 0) {
        OSCBundle reply;
        OSCMessage echo("/-stat/audio/blocks/f32");
        echo.add((int)AudioStream_F32::f32_memory_used);
        echo.add((int)AudioStream_F32::f32_memory_used_max);
        reply.add(echo); g_transport.sendBundle(reply); return;
    }
    if (strcmp(address, "/-stat/audio/blocks/i16") == 0) {
        OSCBundle reply;
        OSCMessage echo("/-stat/audio/blocks/i16");
        echo.add((int)AudioStream::memory_used);
        echo.add((int)AudioStream::memory_used_max);
        reply.add(echo); g_transport.sendBundle(reply); return;
    }
    if (strcmp(address, "/-stat/fw/version") == 0) {
        OSCBundle reply;
        OSCMessage echo("/-stat/fw/version"); echo.add("0.2.0");
        reply.add(echo); g_transport.sendBundle(reply); return;
    }
    if (strcmp(address, "/-stat/fw/phase") == 0) {
        OSCBundle reply;
        OSCMessage echo("/-stat/fw/phase"); echo.add("phase3");
        reply.add(echo); g_transport.sendBundle(reply); return;
    }
    if (strcmp(address, "/-stat/boot/timeMs") == 0) {
        OSCBundle reply;
        OSCMessage echo("/-stat/boot/timeMs"); echo.add((int)millis());
        reply.add(echo); g_transport.sendBundle(reply); return;
    }

    // ---- /xremote, /subscribe, /renew, /unsubscribe ----
    //
    // X32 subscription primitives. On this hardware there's only one
    // CDC client at a time, so every echo already goes to that client
    // — these accept-stubs let X32-ecosystem tools (Mixing Station,
    // X32 Edit, the test harness) connect and complete the subscription
    // dance even though no per-rate / per-TTL bookkeeping is needed.
    // When we eventually grow multi-client (UDP) transport, the same
    // entry points get a real subscriber-list backing.
    if (strcmp(address, "/xremote") == 0 ||
        strcmp(address, "/subscribe") == 0 ||
        strcmp(address, "/renew") == 0 ||
        strcmp(address, "/unsubscribe") == 0) {
        return;  // accept silently; firmware always pushes to the connected client
    }

    // ---- /node ,s "<path>" ---- (X32 group read)
    //
    // Returns one packed text line per subtree, ending in '\n'. The
    // dev surface uses this on connect to populate the UI in a few
    // round-trips instead of issuing per-leaf reads.
    //
    // Currently implemented for the channel strip subtrees; other
    // subtrees fall back to a "(empty)" reply rather than triggering
    // /snapshot. The dev surface should mix /node for known subtrees
    // with /snapshot for the still-unported ones.
    if (strcmp(address, "/node") == 0 && msg.size() > 0 && msg.isString(0)) {
        char path[64] = {0};
        msg.getString(0, path, sizeof(path) - 1);
        char line[256] = {0};
        // "/ch/01" => "ch01 fader-value on-flag\n"  (matches X32 packed form)
        if (strncmp(path, "ch/", 3) == 0 && strlen(path) >= 5) {
            int ch = (path[3] - '0') * 10 + (path[4] - '0');
            if (ch >= 1 && ch <= 6) {
                snprintf(line, sizeof(line),
                         "/ch/%02d %.4f %d\n",
                         ch, g_chFader[ch], g_chOn[ch] ? 1 : 0);
            }
        } else if (strcmp(path, "main/st") == 0) {
            snprintf(line, sizeof(line),
                     "/main/st %.4f %.4f %d %.4f %d\n",
                     g_mainFaderL, g_mainFaderR, g_mainOn ? 1 : 0,
                     g_hostvolValue, g_hostvolEnable ? 1 : 0);
        } else if (strcmp(path, "synth/dexed") == 0) {
            char vname[tdsp::dexed::kVoiceNameBufBytes] = {0};
            tdsp::dexed::copyVoiceName(g_dexedBank, g_dexedVoice, vname, sizeof(vname));
            snprintf(line, sizeof(line),
                     "/synth/dexed %.4f %d %d %d \"%s\"\n",
                     g_dexedVolume, g_dexedOn ? 1 : 0, g_dexedBank, g_dexedVoice, vname);
        } else if (strcmp(path, "synth/bus") == 0) {
            snprintf(line, sizeof(line),
                     "/synth/bus %.4f %d\n",
                     g_synthBusVolume, g_synthBusOn ? 1 : 0);
        } else {
            snprintf(line, sizeof(line), "/%s\n", path);  // empty subtree
        }
        OSCBundle reply;
        OSCMessage m("/node"); m.add(line);
        reply.add(m);
        g_transport.sendBundle(reply);
        return;
    }

    // Everything else: delegate to OscDispatcher. With g_model set but no
    // SignalGraphBinding, /codec/tac5212/... routes correctly to the panel
    // and any /ch/... or /main/... messages we didn't handle above mutate
    // the (unused) model harmlessly — no audio side effect because nothing's
    // bound to it.
    OSCBundle reply;
    g_dispatcher.route(msg, reply);
    if (reply.size() > 0) g_transport.sendBundle(reply);
}

// ============================================================================
// setup / loop
// ============================================================================

void setup() {
    // SHDNZ pulse — exactly once, before Wire.begin() so I2C transactions
    // don't race the analog domain coming up.
    hardResetCodecPower();

    Serial.begin(115200);
    pinMode(LED_BUILTIN, OUTPUT);
    while (!Serial && millis() < 3000) {
        digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
        delay(100);
    }

    Serial.println("================================");
    Serial.println("  T-DSP F32 Audio Shield");
    Serial.println("  Phase 2: F32 mixer + main fader + shelf");
    Serial.println("================================");

    Wire.begin();

    // Quiet the 6140 first. The TAC5212 begin() also probes via I2C and we
    // want a clean bus, plus we want the 6140's TDM output tri-stated
    // before any audio data hits the bus.
    shutdownAdc6140();

    // Bring up the TAC5212 — typed-driver path, no writeRegister fallback.
    setupCodec();

    // Audio block pools. Sized generously — block starvation in either
    // pool surfaces as cumulative noise floor degradation and tiny
    // frequency drift in the captured audio (the chain backs up across
    // update ticks). The production project uses 320 for the int16
    // side; we mirror that headroom now that Dexed is in the graph.
    //
    // F32 pool: USB ring buffers (~10) + 6 in-graph stages + 2 hostvol
    // + 2 Dexed F32 nodes + Dexed dual-mono fan-out depth. 128 leaves
    // ample slack for the 6 additional synth engines coming later.
    AudioMemory_F32(128);

    // int16 pool: 8-voice Dexed engine internals + the bridge's int16
    // input slot + 8-voice multisample sampler (per-voice players,
    // envelopes, mixer chain — ~50 audio_block_t held simultaneously
    // at peak polyphony). 320 leaves headroom for the additional
    // sampler load on top of Dexed.
    AudioMemory(320);

    // --- SD card init (sampler slot needs SD before scanBank) ----------
    //
    // BUILTIN_SDCARD on Teensy 4.1 = 4-bit SDIO. The audio shield's SD
    // slot (1-bit SPI) is too slow for polyphonic streaming. If no SD
    // card is inserted the sampler stays silent — the rest of the
    // firmware (USB audio, Dexed, mixer) continues normally.
    if (!SD.begin(BUILTIN_SDCARD)) {
        Serial.println("[sampler] SD card init failed - sampler slot will stay silent");
    } else {
        Serial.println("[sampler] SD card initialized");
        g_multisampleSlot.begin();
        // Default bank path. User drops 48 kHz / 16-bit / stereo WAVs
        // named after their root note (e.g., C4.wav, F#3.wav) under this
        // directory. scanBank() is silent if the directory doesn't exist.
        g_multisampleSlot.scanBank("/samples/piano");
    }

    // MPE slot 3 — initialise oscillators / filters / envelopes to a
    // defined silent state. Doesn't depend on SD; safe to call
    // unconditionally.
    g_mpeSlot.begin();

    // Push initial mixer state into the audio graph. ch3/4 reserve
    // their slots (no audio path yet); applyChannelFader is a no-op
    // for those today.
    for (int ch = 1; ch <= 6; ++ch) applyChannelFader(ch);
    applyMain();
    applyHostvol();
    applySynthBusGain();
    applyProcShelf();   // disabled at boot -> passthrough on the biquad

    // No host volume control on the F32 USB classes (they don't expose
    // Feature Unit volume); leave the TDM output at unity gain.
    tdmOut.setGain(1.0f);

    // --- Dev surface wiring -----------------------------------------------
    //
    // OscDispatcher.route() short-circuits on null _model (line 57 of
    // OscDispatcher.cpp), which would silently drop every /codec/... echo.
    // Setting an unbound model satisfies that gate; the model itself is
    // harmless because no SignalGraphBinding is attached.
    g_dispatcher.setModel(&g_model);
    g_dispatcher.registerCodecPanel(&g_codecPanel);

    // Wire panel "Initialize" action to re-run the typed setup() flow so
    // the user can re-init from the System tab without rebooting the Teensy.
    g_codecPanel.setInitializeCallback(&setupCodec);

    g_transport.begin(115200);
    g_transport.setOscMessageHandler(&onOscMessage, nullptr);

    // --- USB MIDI host (external keyboard) -------------------------------
    //
    // Bring up the USB host stack and wire the keyboard's note / CC /
    // pitch-bend / sysex callbacks to the router. Default pitch-bend
    // range is seeded for every channel up front so the very first
    // bend that arrives (before any RPN traffic) is scaled correctly.
    // The viz sink is registered last so the dev surface can subscribe
    // to /midi/note events and render a piano-roll without needing to
    // know about the router's other consumers.
    g_usbHost.begin();
    g_midiIn.setHandleNoteOn        (onUsbHostNoteOn);
    g_midiIn.setHandleNoteOff       (onUsbHostNoteOff);
    g_midiIn.setHandleControlChange (onUsbHostControlChange);
    g_midiIn.setHandlePitchChange   (onUsbHostPitchChange);
    g_midiIn.setHandleAfterTouch    (onUsbHostAfterTouch);
    g_midiIn.setHandleProgramChange (onUsbHostProgramChange);
    g_midiIn.setHandleSysEx         (onUsbHostSysEx);

    for (uint8_t ch = 1; ch <= tdsp::MidiRouter::kNumChannels; ++ch) {
        g_midiRouter.setPitchBendRange(ch, tdsp::MidiRouter::kDefaultPitchBendRange);
    }
    g_midiRouter.addSink(&g_midiVizSink);
    g_midiVizSink.enable(true);  // dev-surface consumers can disable via /midi/viz/enable 0

    // ClockSink bridges 0xF8 / 0xFA / 0xFB / 0xFC from the router into
    // g_clock so the arpeggiator (and any future onClock-driven sink)
    // sees a consistent tempo even when the external source is silent.
    g_midiRouter.addSink(&g_clockSink);

    // ArpFilter as the third router sink — viz + clock always see raw
    // MIDI regardless of arp state. Synth sinks register downstream of
    // the arp instead of directly with the router; when the arp is
    // disabled it forwards every event verbatim.
    g_midiRouter.addSink(&g_arpFilter);
    g_arpFilter.setClock(&g_clock);

    // When the shared Clock is in Internal mode, fan each synthetic
    // 24-PPQN tick back through the router so the arp steps at the
    // slider BPM. Without this hook Internal mode would only update
    // the Clock's own counters and the arp would never advance unless
    // an external 0xF8 source happened to be connected.
    g_clock.setInternalTickHook(
        +[](void *) { g_midiRouter.handleClock(); },
        nullptr);

    // --- Dexed engine init ------------------------------------------------
    //
    // synth_dexed is silent after construction — needs a voice loaded and
    // a few controllers seeded before keydown produces sound. Bank 0 voice
    // 0 ("FM-Rhodes") is the canonical "first sound" demo. Listen channel
    // = 0 means omni so any keyboard plays Dexed regardless of its
    // configured channel; users can narrow via /synth/dexed/midi/ch.
    applyDexedVoice(g_dexedBank, g_dexedVoice);
    g_dexed.setPitchbendRange(1);
    g_dexed.setPitchbend((int16_t)0);
    g_dexed.setModWheel(0);
    g_dexed.setSustain(false);
    applyDexedVolume(g_dexedVolume);

    g_dexedSink.setListenChannel(0);

    // Wire slots into the switcher and route the switcher downstream
    // of the arp filter. Engines register through the switcher rather
    // than directly with the arp so the active-slot model controls
    // which engine hears the keyboard. Slot 0 = Dexed, slot 1 = sampler,
    // slot 3 = MPE; slot 2 (Plaits) and 4..7 are SilentSlot placeholders
    // until later phases / per-slot agents bring more engines online.
    g_synthSwitcher.setSlot(0, &g_dexedSlot);
    g_synthSwitcher.setSlot(1, &g_multisampleSlot);
    g_synthSwitcher.setSlot(2, &g_silentSlot2);  // placeholder until Plaits agent wires slot 2
    g_synthSwitcher.setSlot(3, &g_mpeSlot);
    g_synthSwitcher.setSlot(4, &g_neuroSlot);  // was: &g_silentSlot4
    g_neuroSlot.begin();
    g_synthSwitcher.setSlot(5, &g_silentSlot5);
    g_synthSwitcher.setSlot(6, &g_supersawSlot);  // was: &g_silentSlot6
    g_supersawSlot.begin();
    g_synthSwitcher.setSlot(7, &g_silentSlot7);
    g_arpFilter.addDownstream(&g_synthSwitcher);
    g_synthSwitcher.setActive(0);  // Dexed audible at boot

    // --- Boot gate release ------------------------------------------------
    //
    // Apply Windows playback Feature Unit value to the hostvol amps
    // BEFORE unmuting the DAC. On cold boot (before Windows has pushed
    // a SET_CUR) this reads the FEATURE_MAX_VOLUME/2 default — 50%
    // slider, ~-12 dB attenuation — and the DAC turns on at that level.
    // Once Windows pushes the actual slider position, the next poll
    // tick in loop() applies it. This sequencing means audio never goes
    // to the speakers at higher gain than the host has authorized.
    pollHostVolume();
    pollCaptureHostVolume();
    g_codecPanel.unmuteOutput();

    Serial.println("\nReady!");
    Serial.println("  USB host audio: 24-bit / 48 kHz, F32 in graph");
    Serial.println("  Mixer: USB ch1/ch2 -> mixL/R -> mainAmp -> hostvol -> shelf -> DAC");
    Serial.println("  USB MIDI host: keyboard -> router -> viz sink (/midi/note)");
    Serial.println("  Codec panel: /codec/tac5212/... over SLIP-OSC on USB CDC");
    Serial.println("  Dev surface: projects/t-dsp_web_dev/");
}

void loop() {
    // Drain SLIP frames + CLI bytes from the USB CDC stream. onOscMessage()
    // dispatches whatever lands.
    g_transport.poll();

    // Track Windows playback / recording volume sliders. Cheap when
    // nothing's changed (early-return on raw==last).
    pollHostVolume();
    pollCaptureHostVolume();

    // Advance the shared musical clock. Stamps a reference for external
    // tick-interval math and, when Source = Internal, emits catch-up
    // 24-PPQN ticks so the arp's onClock fires even without external
    // MIDI. Called BEFORE draining MIDI so the external-clock path
    // sees a fresh _lastUpdateMicros if 0xF8 folds in via g_midiIn.read().
    g_clock.update(micros());

    // USB host: service enumeration + drain queued MIDI events.
    g_usbHost.Task();
    while (g_midiIn.read()) {}

    // Arp filter: drain the pending gate-off queue. Note step
    // triggering itself runs on onClock through the router; this tick
    // only handles the gate-off timing that closes notes after their
    // gate window expires.
    g_arpFilter.tick(micros());

    // Sampler slot: stop SD streams whose envelopes have decayed. Cheap
    // when nothing's playing (early-return per voice on idle state).
    g_multisampleSlot.pollVoices();

    // Supersaw slot: advance portamento glide. No-op when not held / no
    // glide in progress (early-return inside SupersawSink::tick).
    g_supersawSink.tick(millis());

    // Neuro slot: advance portamento glide + LFO. No-op when no key is
    // held and the LFO is off (early-return inside NeuroSink::tick).
    g_neuroSink.tick(millis());

    // 1 Hz LED heartbeat — proves the main loop hasn't wedged.
    static uint32_t s_lastBlinkMs = 0;
    const uint32_t now = millis();
    if (now - s_lastBlinkMs >= 500) {
        s_lastBlinkMs = now;
        digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
    }
}
