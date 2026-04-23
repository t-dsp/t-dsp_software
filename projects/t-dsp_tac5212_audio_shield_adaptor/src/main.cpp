// T-DSP TAC5212 Audio Shield Adaptor — small mixer MVP v1.
//
// This is the "thin wiring" main.cpp per the roadmap M12 plan, scoped
// to MVP v1 per ~/.claude/memory/decisions_mvp_v1_scope.md. It instantiates
// the TDspMixer framework, wires the existing stock I16 audio graph
// (USB in + line in + stereo PDM mic → main mixer → DAC + USB capture),
// registers a Tac5212Panel for the /codec/tac5212/... OSC subtree, and
// runs the framework in loop().
//
// 6 input channels, mapped 1:1 to the small mixer hardware:
//   /ch/01 — USB L           (host USB input, left)
//   /ch/02 — USB R           (host USB input, right)
//   /ch/03 — Line L          (TAC5212 ADC CH1)
//   /ch/04 — Line R          (TAC5212 ADC CH2)
//   /ch/05 — Mic L           (onboard PDM mic, left channel)
//   /ch/06 — Mic R           (onboard PDM mic, right channel)
//
// Main output goes to the headphone DAC (TAC5212 OUT1/OUT2 in HP driver
// mode, locked in during Phase 1) AND to USB capture. The USB capture
// bus has four switchable sources, each gated by its own unity/zero amp:
//   slot 0: Line L/R   (Channel.recSend, default ON)
//   slot 1: Mic L/R    (Channel.recSend, default ON)
//   slot 2: USB L/R    (Channel.recSend, default OFF — feedback risk
//                       when a DAW monitors input)
//   slot 3: Loop tap   (Main.loopEnable, default OFF) — post-mainAmp /
//                       pre-hostvolAmp, so what's in the headphones
//                       (minus Windows-volume attenuation) becomes the
//                       recording. When engaged, all four per-channel
//                       rec sends are forced to 0 in the binding so
//                       nothing double-counts.
//
// Codec initialization still uses the hand-rolled setupCodec() flow from
// Phase 1. Migrating to lib/TAC5212's typed API is a follow-on refactor
// (M11 Part A). The Tac5212Panel uses lib/TAC5212 at runtime for OSC-
// driven changes, so the two coexist on the same I2C bus.

#include <Arduino.h>
#include <Audio.h>
#include <Wire.h>
#include <USBHost_t36.h>

#include <OSCMessage.h>
#include <OSCBundle.h>

#include "tac5212_regs.h"
#include <TAC5212.h>
#include <TDspMixer.h>
#include <TDspMidi.h>
#include <TDspMPE.h>
#include <synth_dexed.h>

#include "Tac5212Panel.h"
#include "DexedSink.h"
#include "DexedVoiceBank.h"

// Teensy pin driving EN_HELD_HIGH on the TAC5212 module → enables the LDO
const int TAC5212_EN_PIN = 35;

// ============================================================================
// Stock I16 Audio Library objects — the signal graph
// ============================================================================

AudioInputUSB        usbIn;
AudioInputTDM        tdmIn;

// Per-channel peak/RMS taps for meters. 6 channels, 2 analyzers each.
AudioAnalyzePeak     peakCh1;  // USB L
AudioAnalyzePeak     peakCh2;  // USB R
AudioAnalyzePeak     peakCh3;  // Line L
AudioAnalyzePeak     peakCh4;  // Line R
AudioAnalyzePeak     peakCh5;  // Mic L
AudioAnalyzePeak     peakCh6;  // Mic R
AudioAnalyzeRMS      rmsCh1;
AudioAnalyzeRMS      rmsCh2;
AudioAnalyzeRMS      rmsCh3;
AudioAnalyzeRMS      rmsCh4;
AudioAnalyzeRMS      rmsCh5;
AudioAnalyzeRMS      rmsCh6;

// Main bus stereo meter taps — post-main-fader, PRE-hostvol. This is
// the X32-style "main LR meter" reading: reflects the engineer's fader
// moves but NOT the Windows volume slider attenuation. Pre-hostvol is
// the correct tap because the Windows slider is an end-user output
// trim, not a mix-engine change.
AudioAnalyzePeak     peakMainL;
AudioAnalyzePeak     peakMainR;
AudioAnalyzeRMS      rmsMainL;
AudioAnalyzeRMS      rmsMainR;

// Host (Windows-volume) meter taps — post-hostvol, so they show the
// actual level headed to the DAC (main fader × hostvol × hostvolEnable).
// The engineer can compare these against the main-bus meters to see
// how much Windows volume is attenuating things.
AudioAnalyzePeak     peakHostL;
AudioAnalyzePeak     peakHostR;
AudioAnalyzeRMS      rmsHostL;
AudioAnalyzeRMS      rmsHostR;

// Main bus stereo FFT taps — sit on the same pre-hostvol node as the
// peak/RMS meter taps, so the spectrum analyzer view reflects the mix
// engineer's faders but NOT Windows volume attenuation. Default 1024-
// point gives 512 bins at ~86 Hz resolution (44.1 kHz / 1024 * 2),
// new frame every ~11.6 ms. SpectrumEngine polls them at 10 Hz.
tdsp::AnalyzeFFT_F32 fftMainL;
tdsp::AnalyzeFFT_F32 fftMainR;

// PDM mic combiners — 32-bit PDM split across two 16-bit TDM slots, so
// each PDM mic (L, R) needs a 2-input mixer that re-combines the high
// and low halves with correct scaling.
AudioMixer4          pdmMixL;
AudioMixer4          pdmMixR;

// Main stereo mixer. Slot assignments:
//   mixL[0] = ch/01 USB L    mixR[0] = ch/02 USB R
//   mixL[1] = ch/05 Mic L    mixR[1] = ch/06 Mic R
//   mixL[2] = ch/03 Line L   mixR[2] = ch/04 Line R
//   slot 3 unused
AudioMixer4          mixL;
AudioMixer4          mixR;

// Pre-main summing bus. Slot 0 is the input-channel mix; slots 1-3 are
// reserved for synth buses (each synth sums its stereo output into the
// next free slot). All slots are unity gain — per-source level control
// happens upstream (channel faders on the input side, synth faders on
// the synth side). Adding this stage here — between mixL/R and mainAmpL/R,
// rather than at a later point in the chain — means the main fader /
// mute / meters / FFT / loopback tap see the combined mix of inputs AND
// synths, which matches the "main bus is everything you hear" mental
// model. Cost: one extra audio block of latency per side (~2.9 ms at
// 44.1 kHz).
AudioMixer4          preMixL;
AudioMixer4          preMixR;

// Dexed FM synth engine — 8-voice polyphonic, 6-operator FM. Dexed's
// audio output is mono; g_dexedGain sits between the engine and the
// preMix bus so /synth/dexed/volume can trim the synth's level without
// reaching into Dexed's internal voice levels. The same gain amp feeds
// both preMixL[1] and preMixR[1] (dual-mono), which places Dexed in the
// center of the stereo field. Sample rate is passed as AUDIO_SAMPLE_RATE
// so Dexed's internal tuning aligns with Teensy Audio's actual update
// rate (44.117.6 Hz, not nominal 44.1 kHz) — a ~0.04% pitch correction
// that avoids a subtle sharp tuning offset.
AudioSynthDexed      g_dexed((uint8_t)8, (uint16_t)AUDIO_SAMPLE_RATE_EXACT);
AudioAmplifier       g_dexedGain;

// Per-synth FX send amp — scales the tap that feeds the shared FX
// send bus. Sits between g_dexedGain and fxSendBus (below). TDspMPE
// has its own g_mpeSend on slot 1 of the same bus.
AudioAmplifier       g_dexedSend;

// MPE-native virtual-analog engine (Phase 2d). Four voices of stock
// Teensy Audio primitives: osc → state-variable filter → amp
// envelope. Each voice is dual-mono into mpeMixL/R; the summed mix
// trims through mpeGainL/R into preMix slot 2. The per-voice filter
// is what makes MPE expression actually useful — CC#74 routes to
// each voice's own cutoff, so a LinnStrument finger steers brightness
// on its own note only.
//
// g_mpeSend taps post-volume (L side only; MPE is dual-mono so L
// carries the full content) into the shared FX bus on slot 1 —
// matching the slot reservation in g_dexedSend's comment above.
AudioSynthWaveform       mpeOsc0, mpeOsc1, mpeOsc2, mpeOsc3;
AudioFilterStateVariable mpeFilt0, mpeFilt1, mpeFilt2, mpeFilt3;
AudioEffectEnvelope      mpeEnv0, mpeEnv1, mpeEnv2, mpeEnv3;
AudioMixer4              mpeMixL, mpeMixR;
AudioAmplifier           mpeGainL, mpeGainR;
AudioAmplifier           g_mpeSend;

// ---- Shared FX bus -------------------------------------------------
//
// Every synth taps its post-volume signal through its own send amp
// into this bus, which summed-feeds a single chorus → reverb chain.
// The wet stereo output returns into preMix slot 3 (left+right) via
// two "return" amps that control overall wet level.
//
// Signal flow (mono until fxReverb, stereo after):
//   g_dexedGain → g_dexedSend → fxSendBus[0] ┐
//   g_mpeSend   (future)      → fxSendBus[1] ┤
//                                            ├→ fxChorus → fxReverb ┬→ fxReturnL → preMixL[3]
//                                                                   └→ fxReturnR → preMixR[3]
//
// Chorus uses the Teensy Audio library's n-voice chorus which needs
// an externally-allocated int16 delay line. 4096 samples ≈ 93 ms at
// 44.1 kHz — more than enough for any usable chorus depth.
// AudioEffectFreeverbStereo is naturally mono-in / stereo-out, so
// the mono send bus gets a free stereo widening through the reverb.
//
// "Disable" on each stage means setting the downstream amp (or the
// chorus voice count) to a transparent / muted value rather than
// unwiring anything; the graph shape stays constant at runtime.
constexpr int kFxChorusDelayLen = 4096;
short          g_fxChorusDelayLine[kFxChorusDelayLen];

AudioMixer4              fxSendBus;
AudioEffectChorus        fxChorus;
AudioEffectFreeverbStereo fxReverb;
AudioAmplifier           fxReturnL;
AudioAmplifier           fxReturnR;

// Four-stage main bus chain:
//   preMixL → mainAmpL (faderL × on) → hostvolAmpL (hostvol bypass)
//           → procShelfL (tone)      → procLimiterL (peak ceiling) → DAC
// The meter taps sit at the output of mainAmpL/R, so they see the
// post-fader / pre-hostvol signal. SignalGraphBinding drives mainAmp/
// hostvolAmp stages from MixerModel via applyMain().
//
// procShelfL/R and procLimiterL/R are the "Processing" tab stages: a
// high-shelf EQ to tame FM harshness and a soft-clip waveshaper to
// cap peak level for ear safety. Both sit AFTER the hostvol amp, so
// the USB-capture path (which tees off after mainAmp, pre-hostvol)
// is unaffected — recordings stay unprocessed while the DAC / head-
// phone output is protected. When the user toggles either off, the
// stage is reconfigured to a transparent response (0 dB shelf /
// identity waveshape) rather than being physically bypassed — the
// graph shape stays constant at runtime.
AudioAmplifier       mainAmpL;
AudioAmplifier       mainAmpR;
AudioAmplifier       hostvolAmpL;
AudioAmplifier       hostvolAmpR;
AudioFilterBiquad    procShelfL;
AudioFilterBiquad    procShelfR;
AudioEffectWaveshaper procLimiterL;
AudioEffectWaveshaper procLimiterR;

// Capture mixers — USB out (host recording). Four source slots gated by
// per-source amps (see the header block above):
//   slot 0: recLineL/R  → Line
//   slot 1: recMicL/R   → PDM mic
//   slot 2: recUsbL/R   → USB playback (default off)
//   slot 3: loopL/R     → main-mix loopback (default off)
AudioMixer4          captureL;
AudioMixer4          captureR;

// Per-source capture-send amplifiers. Gain is 0.0 or 1.0, driven by
// SignalGraphBinding::applyChannelRec() from Channel.recSend (overridden
// to 0 when Main.loopEnable is true so loop + direct sends don't
// double-count in the USB recording).
AudioAmplifier       recUsbL;
AudioAmplifier       recUsbR;
AudioAmplifier       recLineL;
AudioAmplifier       recLineR;
AudioAmplifier       recMicL;
AudioAmplifier       recMicR;

// Main-bus loopback tap amps. Fed from mainAmpL/R output (post-main-fader,
// pre-hostvol) so the recording tracks the fader but not Windows volume.
// Gain is 1.0 when Main.loopEnable is true, else 0.0.
AudioAmplifier       loopL;
AudioAmplifier       loopR;

// Listenback monitor attenuators — driven by the Windows recording-device
// volume slider via the FU 0x30 capture-side Feature Unit. Inserted on the
// MONITOR branch of each capture source (line + mic) so the user can pull
// down what they hear in their headphones without affecting the record
// level the DAW receives. USB-in monitoring is intentionally NOT routed
// through these (its level is controlled by the Windows playback slider
// via hostvolAmpL/R), and the captureL/R → usbOut record path stays at
// unity (record send is independent).
//
// Each tdmIn / pdmMix source tees into THREE branches:
//   1. captureL/R (record send, unity, untouched)
//   2. peak/rms taps (meter, unity, untouched)
//   3. monLine* / monMic* → mixL/R (THIS branch, attenuated by capHostVol)
//
// Default gain is unity (1.0); pollCaptureHostVolume() applies the live
// value once Windows pushes its first SET_CUR.
AudioAmplifier       monLineL;
AudioAmplifier       monLineR;
AudioAmplifier       monMicL;
AudioAmplifier       monMicR;

// Mono cross-feed: carries tdmIn ch0 (Line L / ADC CH1) into the R side
// of the main mixer. In stereo mode gain=0 (silent); in mono/differential
// mode gain=1 and the binding mirrors ch3's effective gain to mixR[3].
AudioAmplifier       monoXfeed;

AudioOutputTDM       tdmOut;
AudioOutputUSB       usbOut;

// ============================================================================
// AudioConnections — wire the graph
// ============================================================================

// USB input → main mixer slot 0 + peak/RMS taps + capture-send branch
// (through recUsb* → captureL/R slot 2). The record-send branch is
// gated by an amp so the USB playback signal normally doesn't loop
// back into USB capture (feedback risk when a DAW monitors its input);
// Channel.recSend flips it on if the user explicitly arms it.
AudioConnection      c_usbL_mix    (usbIn, 0, mixL, 0);
AudioConnection      c_usbL_peak   (usbIn, 0, peakCh1, 0);
AudioConnection      c_usbL_rms    (usbIn, 0, rmsCh1, 0);
AudioConnection      c_usbL_rec    (usbIn, 0, recUsbL, 0);
AudioConnection      c_recUsbL_cap (recUsbL, 0, captureL, 2);
AudioConnection      c_usbR_mix    (usbIn, 1, mixR, 0);
AudioConnection      c_usbR_peak   (usbIn, 1, peakCh2, 0);
AudioConnection      c_usbR_rms    (usbIn, 1, rmsCh2, 0);
AudioConnection      c_usbR_rec    (usbIn, 1, recUsbR, 0);
AudioConnection      c_recUsbR_cap (recUsbR, 0, captureR, 2);

// Line input (ADC) → record send (through recLine* amp → captureL/R
// slot 0) + peak/RMS taps + listenback monitor branch (through monLine*
// attenuator → main mixer slot 2). The record send goes through an
// on/off amp gated by Channel.recSend so it can be muted when the user
// doesn't want that source in USB capture (e.g. when Main.loopEnable is
// on and the line signal is already in the loop tap). The meter taps
// draw directly from tdmIn so they're unaffected by rec state.
AudioConnection      c_lineL_rec   (tdmIn, 0, recLineL, 0);
AudioConnection      c_recLineL_cap(recLineL, 0, captureL, 0);
AudioConnection      c_lineL_peak  (tdmIn, 0, peakCh3, 0);
AudioConnection      c_lineL_rms   (tdmIn, 0, rmsCh3, 0);
AudioConnection      c_lineL_mon   (tdmIn, 0, monLineL, 0);
AudioConnection      c_lineL_mix   (monLineL, 0, mixL, 2);
AudioConnection      c_lineR_rec   (tdmIn, 2, recLineR, 0);
AudioConnection      c_recLineR_cap(recLineR, 0, captureR, 0);
AudioConnection      c_lineR_peak  (tdmIn, 2, peakCh4, 0);
AudioConnection      c_lineR_rms   (tdmIn, 2, rmsCh4, 0);
AudioConnection      c_lineR_mon   (tdmIn, 2, monLineR, 0);
AudioConnection      c_lineR_mix   (monLineR, 0, mixR, 2);

// Mono cross-feed: Line L (tdmIn ch0 / ADC CH1) → monoXfeed amp → mixR slot 3.
// Slot 3 is otherwise unused on both mixers. In stereo mode the amp is
// at gain 0 so this path is silent. In mono mode CH1's differential
// signal feeds both mixL[2] (normal path) and mixR[3] (cross-feed).
AudioConnection      c_lineL_xfeed (tdmIn, 0, monoXfeed, 0);
AudioConnection      c_xfeed_mixR  (monoXfeed, 0, mixR, 3);

// PDM mic: split across TDM slots 2+3 = Teensy ch 4,5,6,7, then combined
AudioConnection      c_pdmL0       (tdmIn, 4, pdmMixL, 0);
AudioConnection      c_pdmL1       (tdmIn, 5, pdmMixL, 1);
AudioConnection      c_pdmR0       (tdmIn, 6, pdmMixR, 0);
AudioConnection      c_pdmR1       (tdmIn, 7, pdmMixR, 1);

// PDM combiners → record send (through recMic* amp → captureL/R slot 1)
// + peak/RMS taps + listenback monitor branch. Same pattern as line
// above: rec send is gated by Channel.recSend, meters are unity.
AudioConnection      c_micL_rec    (pdmMixL, 0, recMicL, 0);
AudioConnection      c_recMicL_cap (recMicL, 0, captureL, 1);
AudioConnection      c_micL_peak   (pdmMixL, 0, peakCh5, 0);
AudioConnection      c_micL_rms    (pdmMixL, 0, rmsCh5, 0);
AudioConnection      c_micL_mon    (pdmMixL, 0, monMicL, 0);
AudioConnection      c_micL_mix    (monMicL, 0, mixL, 1);
AudioConnection      c_micR_rec    (pdmMixR, 0, recMicR, 0);
AudioConnection      c_recMicR_cap (recMicR, 0, captureR, 1);
AudioConnection      c_micR_peak   (pdmMixR, 0, peakCh6, 0);
AudioConnection      c_micR_rms    (pdmMixR, 0, rmsCh6, 0);
AudioConnection      c_micR_mon    (pdmMixR, 0, monMicR, 0);
AudioConnection      c_micR_mix    (monMicR, 0, mixR, 1);

// Input mixers → preMix bus (slot 0) → main fader amps → hostvol amps → DAC.
// preMix slot 1 = Dexed (wired below), slots 2..3 = reserved for future
// synth engines. Unused slots sit at gain 0 and contribute nothing.
AudioConnection      c_mixL_pre    (mixL, 0, preMixL, 0);
AudioConnection      c_mixR_pre    (mixR, 0, preMixR, 0);

// Dexed: engine → volume amp → both sides of preMix slot 1 (dual-mono).
// The single AudioAmplifier output feeds two downstream AudioConnections,
// which Teensy Audio supports natively (each connection gets the same
// block, ref-counted internally).
AudioConnection      c_dexed_gain  (g_dexed,     0, g_dexedGain, 0);
// Dry path — dual-mono into preMix slot 1.
AudioConnection      c_dexed_preL  (g_dexedGain, 0, preMixL,     1);
AudioConnection      c_dexed_preR  (g_dexedGain, 0, preMixR,     1);
// FX send path — post-volume tap through the per-synth send amp into
// the shared FX bus slot 0. Setting g_dexedSend's gain to 0 mutes
// just this synth's contribution; other synths keep sending.
AudioConnection      c_dexed_send  (g_dexedGain, 0, g_dexedSend, 0);
AudioConnection      c_dexed_bus   (g_dexedSend, 0, fxSendBus,   0);

// MPE VA voice chains: osc → filter → env → mpeMixL/R (slot = voice
// index for easy debugging). Voice chains go in voice-order so a
// silent slot is obviously "voice N missing" rather than a wiring
// confusion.
AudioConnection      c_mpeOsc0_filt (mpeOsc0,  0, mpeFilt0, 0);
AudioConnection      c_mpeFilt0_env (mpeFilt0, 0, mpeEnv0,  0);
AudioConnection      c_mpeEnv0_L    (mpeEnv0,  0, mpeMixL,  0);
AudioConnection      c_mpeEnv0_R    (mpeEnv0,  0, mpeMixR,  0);
AudioConnection      c_mpeOsc1_filt (mpeOsc1,  0, mpeFilt1, 0);
AudioConnection      c_mpeFilt1_env (mpeFilt1, 0, mpeEnv1,  0);
AudioConnection      c_mpeEnv1_L    (mpeEnv1,  0, mpeMixL,  1);
AudioConnection      c_mpeEnv1_R    (mpeEnv1,  0, mpeMixR,  1);
AudioConnection      c_mpeOsc2_filt (mpeOsc2,  0, mpeFilt2, 0);
AudioConnection      c_mpeFilt2_env (mpeFilt2, 0, mpeEnv2,  0);
AudioConnection      c_mpeEnv2_L    (mpeEnv2,  0, mpeMixL,  2);
AudioConnection      c_mpeEnv2_R    (mpeEnv2,  0, mpeMixR,  2);
AudioConnection      c_mpeOsc3_filt (mpeOsc3,  0, mpeFilt3, 0);
AudioConnection      c_mpeFilt3_env (mpeFilt3, 0, mpeEnv3,  0);
AudioConnection      c_mpeEnv3_L    (mpeEnv3,  0, mpeMixL,  3);
AudioConnection      c_mpeEnv3_R    (mpeEnv3,  0, mpeMixR,  3);
AudioConnection      c_mpeMixL_gain (mpeMixL,  0, mpeGainL, 0);
AudioConnection      c_mpeMixR_gain (mpeMixR,  0, mpeGainR, 0);
// Dry path — mpeGainL/R into preMix slot 2 (preMix slot 3 is taken
// by the shared FX wet return).
AudioConnection      c_mpeGainL_pre (mpeGainL, 0, preMixL,  2);
AudioConnection      c_mpeGainR_pre (mpeGainR, 0, preMixR,  2);
// FX send path — L side only (MPE is dual-mono so mpeGainL carries
// the full mono content). Feeds shared FX bus slot 1.
AudioConnection      c_mpe_send     (mpeGainL, 0, g_mpeSend, 0);
AudioConnection      c_mpe_bus      (g_mpeSend, 0, fxSendBus, 1);

// Shared FX chain: send bus → chorus → stereo reverb → return amps → preMix slot 3.
AudioConnection      c_fx_bus_cho  (fxSendBus,  0, fxChorus,  0);
AudioConnection      c_fx_cho_rev  (fxChorus,   0, fxReverb,  0);
AudioConnection      c_fx_rev_retL (fxReverb,   0, fxReturnL, 0);
AudioConnection      c_fx_rev_retR (fxReverb,   1, fxReturnR, 0);
AudioConnection      c_fx_retL_pre (fxReturnL,  0, preMixL,   3);
AudioConnection      c_fx_retR_pre (fxReturnR,  0, preMixR,   3);

AudioConnection      c_mainL_amp   (preMixL, 0, mainAmpL, 0);
AudioConnection      c_mainR_amp   (preMixR, 0, mainAmpR, 0);
AudioConnection      c_mainL_hv    (mainAmpL, 0, hostvolAmpL, 0);
AudioConnection      c_mainR_hv    (mainAmpR, 0, hostvolAmpR, 0);
AudioConnection      c_hvL_shelf   (hostvolAmpL, 0, procShelfL,   0);
AudioConnection      c_hvR_shelf   (hostvolAmpR, 0, procShelfR,   0);
AudioConnection      c_shelfL_lim  (procShelfL,  0, procLimiterL, 0);
AudioConnection      c_shelfR_lim  (procShelfR,  0, procLimiterR, 0);
AudioConnection      c_limL_dac    (procLimiterL, 0, tdmOut, 0);
AudioConnection      c_limR_dac    (procLimiterR, 0, tdmOut, 2);

// Main bus meter taps — on the FADER amp output, pre-hostvol. So the
// meter tracks the main fader but is unaffected by Windows volume.
AudioConnection      c_mainL_peak  (mainAmpL, 0, peakMainL, 0);
AudioConnection      c_mainL_rms   (mainAmpL, 0, rmsMainL, 0);
AudioConnection      c_mainR_peak  (mainAmpR, 0, peakMainR, 0);
AudioConnection      c_mainR_rms   (mainAmpR, 0, rmsMainR, 0);

// Main bus FFT taps — same tap node as the peak/RMS meters above so
// the spectrum reflects the engineer's fader but not Windows volume.
// Each FFT1024 instance runs continuously (the stock Audio lib offers
// no clean enable/disable); it only costs network + CPU when the
// SpectrumEngine is actually polling and emitting blobs.
AudioConnection      c_mainL_fft   (mainAmpL, 0, fftMainL, 0);
AudioConnection      c_mainR_fft   (mainAmpR, 0, fftMainR, 0);

// Main bus loopback tap — same node as meter/FFT taps (post-mainAmp,
// pre-hostvol). Feeds captureL/R slot 3 through loopL/R amps. Gain is
// 1.0 when Main.loopEnable is true, else 0.0 (bindings update on change).
AudioConnection      c_mainL_loop  (mainAmpL, 0, loopL, 0);
AudioConnection      c_loopL_cap   (loopL, 0, captureL, 3);
AudioConnection      c_mainR_loop  (mainAmpR, 0, loopR, 0);
AudioConnection      c_loopR_cap   (loopR, 0, captureR, 3);

// Host meter taps — on the hostvol amp output, POST-hostvol. So the
// meter shows what the DAC actually receives, including Windows volume
// attenuation. Compare vs the main meters to see hostvol in action.
AudioConnection      c_hostL_peak  (hostvolAmpL, 0, peakHostL, 0);
AudioConnection      c_hostL_rms   (hostvolAmpL, 0, rmsHostL, 0);
AudioConnection      c_hostR_peak  (hostvolAmpR, 0, peakHostR, 0);
AudioConnection      c_hostR_rms   (hostvolAmpR, 0, rmsHostR, 0);

// Capture mixers → USB out (host recording)
AudioConnection      c_capL_usb    (captureL, 0, usbOut, 0);
AudioConnection      c_capR_usb    (captureR, 0, usbOut, 1);

// ============================================================================
// TDspMixer framework objects
// ============================================================================

tdsp::MixerModel          g_model;
tdsp::SignalGraphBinding  g_binding;
tdsp::OscDispatcher       g_dispatcher;
tdsp::SlipOscTransport    g_transport;
tdsp::MeterEngine         g_meters;
tdsp::SpectrumEngine      g_spectrum;

// lib/TAC5212 codec instance used by Tac5212Panel for runtime register
// access. The chip is physically initialized by setupCodecHandRolled()
// below; g_codec is NOT .begin()-ed because that would re-reset the
// chip and wipe the hand-rolled init state. Both APIs touch the same
// I2C address, which is safe as long as we don't interleave writes
// to the same register from both sides.
tac5212::TAC5212          g_codec(Wire);

Tac5212Panel              g_codecPanel(g_codec);

// Line input mode: false = stereo (CH1=L, CH2=R, single-ended),
// true = mono differential (CH1 differential, mono → both L+R outputs).
static bool g_lineMonoMode = false;

// Dexed synth output level, 0..1 linear. Stored in the sketch because
// Teensy Audio's AudioAmplifier exposes no getter — broadcastSnapshot()
// and any future UI echo needs the last-set value. Applied via
// applyDexedVolume() which writes g_dexedGain.gain().
static float g_dexedVolume = 0.7f;

static void applyDexedVolume(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    g_dexedVolume = v;
    g_dexedGain.gain(v);
}

// Currently-loaded voice within the bundled DX7 bank set. Mirrored
// here so /synth/dexed/voice read-back and snapshot work without
// poking the engine. (synth_dexed doesn't expose a "what voice is
// this?" accessor — it stores VCED bytes, not the source bank/voice.)
static int g_dexedBank  = 0;
static int g_dexedVoice = 0;

// --------------------------------------------------------------------
// Shared FX bus state — chorus + reverb params, per-synth send amounts.
//
// Per-synth: g_dexedSendAmt is the 0..1 gain of g_dexedSend. Default
// 0.0 means "dry by default" so a first-boot user hears the synth
// unprocessed until they explicitly dial in some send.
//
// Global chorus: voice count 0/1 = bypass, 2..8 = progressively
// thicker chorus. We expose this as an "enable + voices" pair to the
// UI so a binary toggle maps cleanly onto the single integer the
// library wants.
//
// Global reverb: roomsize and damping map 1:1 to AudioEffectFreeverb-
// Stereo's internal params (both 0..1). Wet return amount is the
// shared gain on fxReturnL/R — set to 0 to mute the wet regardless
// of reverb settings.
// --------------------------------------------------------------------

static float g_dexedSendAmt       = 0.0f;   // 0..1, default dry
static bool  g_fxChorusEnable     = false;  // OFF by default
static int   g_fxChorusVoices     = 3;      // 2..8 when enabled
static bool  g_fxReverbEnable     = false;  // OFF by default
static float g_fxReverbRoomSize   = 0.6f;   // medium room
static float g_fxReverbDamping    = 0.5f;   // balanced
static float g_fxReverbReturnAmt  = 0.6f;   // 0..1 wet level into main mix

static void applyDexedSend() {
    g_dexedSend.gain(g_dexedSendAmt);
}

static void applyFxChorus() {
    // voices(1) = pure passthrough (the library's update() early-exits
    // through a copy-the-block path when num_chorus <= 1). voices(2..8)
    // = active chorus. We never set voices(0) because that ALSO works
    // as passthrough at runtime, but if begin() is ever called with
    // n_chorus < 1 it returns false and leaves the delay line null —
    // killing the wet bus permanently. Keeping the runtime value in
    // [1, 8] avoids any confusion with that init-time landmine.
    int n = g_fxChorusEnable ? g_fxChorusVoices : 1;
    if (n > 8) n = 8;
    if (g_fxChorusEnable && n < 2) n = 2;
    fxChorus.voices(n);
}

static void applyFxReverb() {
    // "Disable" mutes the wet return so the reverb engine keeps
    // running (cheap enough — CPU is always on) but contributes
    // nothing to the mix. This avoids audible tail chops if a user
    // toggles reverb while notes are ringing.
    const float ret = g_fxReverbEnable ? g_fxReverbReturnAmt : 0.0f;
    fxReverb.roomsize(g_fxReverbRoomSize);
    fxReverb.damping(g_fxReverbDamping);
    fxReturnL.gain(ret);
    fxReturnR.gain(ret);
}

// --------------------------------------------------------------------
// Main-bus processing stage: high-shelf EQ + peak-limit waveshaper.
//
// Tone shelf defaults to -4 dB above 5 kHz — a mild "darken" that
// takes the hardest edge off FM synth sizzle without making things
// sound veiled. The limiter uses a tanh soft-clip curve capped at
// 0.7079 (-3 dBFS), giving a generous peak margin the DAC can't
// exceed no matter how hot the upstream signal is.
//
// Both stages always sit in the audio graph (see the object declara-
// tions up top). "Disable" reconfigures them to a transparent
// response rather than bypassing the block — cheaper than rebuilding
// connections at runtime and keeps meter taps stable.
//
// The limiter's lookup table is AUDIO_BLOCK_SAMPLES-independent: it
// maps input amplitude (via a 513-point lerp) to output amplitude
// once, and the waveshaper object stores the resulting int16_t table.
// Rebuilt when enable toggles.
// --------------------------------------------------------------------

static bool  g_procShelfEnable   = true;
static float g_procShelfFreqHz   = 5000.0f;
static float g_procShelfGainDb   = -4.0f;
static bool  g_procLimiterEnable = true;

constexpr int   kProcLimiterTableLen = 513;      // 2^N + 1 as required by AudioEffectWaveshaper
constexpr float kProcLimiterCeiling  = 0.7079f;  // ~ -3 dBFS
constexpr float kProcLimiterKnee     = 2.0f;     // tanh steepness: higher = harder knee

static float g_procLimiterTable[kProcLimiterTableLen];

static void applyProcShelf() {
    // Biquad setHighShelf gain is in dB. A gain of 0 dB collapses to
    // an identity filter mathematically (a = 1.0 makes all the
    // intermediate terms fold to {b0=1, b1=0, b2=0, a1=0, a2=0}), so
    // "disabled" is just gain=0 on the same stage — no bypass wiring.
    const float gain = g_procShelfEnable ? g_procShelfGainDb : 0.0f;
    procShelfL.setHighShelf(0, g_procShelfFreqHz, gain, 1.0f);
    procShelfR.setHighShelf(0, g_procShelfFreqHz, gain, 1.0f);
}

static void applyProcLimiter() {
    // Build the lookup table. Input i in [0, N-1] maps to x in [-1, +1];
    // output y is either the tanh-soft-clipped value (enabled) or a
    // straight identity (disabled, i.e. y = x).
    for (int i = 0; i < kProcLimiterTableLen; ++i) {
        const float x = -1.0f + 2.0f * (float)i / (float)(kProcLimiterTableLen - 1);
        float y;
        if (g_procLimiterEnable) {
            // tanh(kx) / tanh(k) normalizes so tanh(1) still maps to 1.0
            // before the ceiling scale; this keeps headroom below the
            // ceiling clean (1:1 response at low levels) and eases into
            // saturation near the edges.
            y = tanhf(kProcLimiterKnee * x) / tanhf(kProcLimiterKnee) * kProcLimiterCeiling;
        } else {
            y = x;
        }
        g_procLimiterTable[i] = y;
    }
    procLimiterL.shape(g_procLimiterTable, kProcLimiterTableLen);
    procLimiterR.shape(g_procLimiterTable, kProcLimiterTableLen);
}

// Load the selected bank/voice into the Dexed engine. Clamps to valid
// ranges; on failure (bad indices) leaves the engine untouched. Kills
// sounding notes before loading — decodeVoice() calls panic() anyway,
// but doing it here makes the intent explicit and survives any future
// library revision that stops auto-panicking on voice change.
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

// ============================================================================
// USB host — MIDI keyboard input, routed through tdsp::MidiRouter
// ============================================================================
//
// The Teensy 4.1 has a second USB port wired as a host controller. Plug in
// a USB MIDI keyboard and g_usbHost.Task() enumerates it; g_midiIn.read()
// drains any queued events and fires the installed callbacks. Those callbacks
// all forward to g_midiRouter, which dispatches normalized events to
// every registered MidiSink. Today there's one sink (MidiVizSink below)
// that broadcasts note on/off to the web dev surface. Phase 2c+ adds
// synth-engine sinks.
//
// MIDIDevice (not _BigBuffer) is sufficient for notes and basic CCs.
// When we wire Dexed and want SysEx voice dumps, switch to
// MIDIDevice_BigBuffer — its constructor + setup calls are identical.

USBHost      g_usbHost;
USBHub       g_usbHub1(g_usbHost);
USBHub       g_usbHub2(g_usbHost);
MIDIDevice   g_midiIn(g_usbHost);

tdsp::MidiRouter g_midiRouter;
DexedSink        g_dexedSink(&g_dexed);

// MPE VA sink + voice-port wiring. The sink never allocates; it just
// steers the file-scope oscillator / filter / envelope primitives via
// pointers handed to it at construction.
MpeVaSink::VoicePorts g_mpeVoices[4] = {
    {&mpeOsc0, &mpeEnv0, &mpeFilt0},
    {&mpeOsc1, &mpeEnv1, &mpeFilt1},
    {&mpeOsc2, &mpeEnv2, &mpeFilt2},
    {&mpeOsc3, &mpeEnv3, &mpeFilt3},
};
MpeVaSink g_mpeSink(g_mpeVoices, 4);

// MPE VA output level (post-summing, pre-preMix). Mirrors the Dexed
// volume pattern — AudioAmplifier has no getter, so the sketch keeps
// the last-set value for broadcastSnapshot() and OSC echo.
static float g_mpeVolume     = 0.7f;
static float g_mpeSendAmount = 0.0f;

static void applyMpeVolume(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    g_mpeVolume = v;
    mpeGainL.gain(v);
    mpeGainR.gain(v);
}

static void applyMpeSend(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    g_mpeSendAmount = v;
    g_mpeSend.gain(v);
}

// ============================================================================
// MidiVizSink — broadcasts /midi/note on each note-on/off for the web
// keyboard visualization. Subscription-gated: the web UI sends
// /sub addSub /midi/events when the Synth tab opens, which flips _enabled.
// When disabled, events are dropped silently so no USB CDC traffic happens
// if nobody's watching (meters/spectrum follow the same pattern).
//
// OSC argument casts use plain `int` — matches CNMAT/OSC's intOSC_t on
// this gcc-arm toolchain where `int32_t` resolves to `long int` and has
// no exact OSCData constructor overload.
// ============================================================================

class MidiVizSink : public tdsp::MidiSink {
public:
    void setEnabled(bool on) { _enabled = on; }
    bool isEnabled() const   { return _enabled; }

    void onNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) override {
        if (!_enabled) return;
        broadcastNote(note, velocity, channel);
    }
    void onNoteOff(uint8_t channel, uint8_t note, uint8_t /*velocity*/) override {
        if (!_enabled) return;
        // Note-off velocity is rarely meaningful for visualization; the
        // wire format uses velocity==0 as the note-off sentinel so the
        // web UI can treat (velocity > 0) as "held".
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

// MPE voice telemetry broadcast. Subscription-gated (Synth tab flips
// it on via /sub addSub /synth/mpe/voices). When enabled, every ~33
// ms the firmware snapshots MpeVaSink's voice state and fires
// /synth/mpe/voices with 4 × {held, ch, note, pitchSemi, pressure,
// timbre}. Flat layout keeps the OSC overhead to one bundle per
// frame.
static bool     g_mpeVoicesEnabled = false;
static uint32_t g_mpeLastVoicesMs  = 0;

static void broadcastMpeVoices() {
    MpeVaSink::VoiceSnapshot snap[4];
    const int n = g_mpeSink.voiceSnapshot(snap, 4);
    OSCMessage m("/synth/mpe/voices");
    for (int i = 0; i < n; ++i) {
        m.add((int)(snap[i].held ? 1 : 0));
        m.add((int)snap[i].channel);
        m.add((int)snap[i].note);
        m.add(snap[i].pitchSemi);
        m.add(snap[i].pressure);
        m.add(snap[i].timbre);
    }
    OSCBundle b;
    b.add(m);
    g_transport.broadcastBundle(b);
}

// ---------------------------------------------------------------------
// USB host MIDI callbacks — forward to the router. Keep these as plain
// free functions (USBHost_t36's setHandleXxx takes function pointers,
// not std::function). Every callback is a one-liner into g_midiRouter.
// ---------------------------------------------------------------------

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
    // USBHost_t36 delivers pitch bend as a plain `int` in the range
    // -8192..+8191. Narrow to int16_t which is what the router expects;
    // the value range is guaranteed to fit.
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
// Hand-rolled codec init (Phase 1 path, preserved as the working audio config)
// ============================================================================

static void writeReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(TAC5212_I2C_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

static uint8_t readReg(uint8_t reg) {
    Wire.beginTransmission(TAC5212_I2C_ADDR);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom(TAC5212_I2C_ADDR, (uint8_t)1);
    return Wire.available() ? Wire.read() : 0xFF;
}

static void codecResetAndWake() {
    writeReg(REG_SW_RESET, 0x01);
    delay(100);
    writeReg(REG_SLEEP_CFG, SLEEP_CFG_WAKE);
    delay(100);
}

static void codecConfigureSerialInterface() {
    writeReg(REG_PASI_CFG0, PASI_CFG0_TDM_32_BCLK_INV);
    writeReg(REG_INTF_CFG1, INTF_CFG1_DOUT_PASI);
    writeReg(REG_INTF_CFG2, INTF_CFG2_DIN_ENABLE);
    writeReg(REG_PASI_RX_CFG0, PASI_OFFSET_1);
    writeReg(REG_PASI_TX_CFG1, PASI_OFFSET_1);
}

static void codecConfigureSlotMappings() {
    writeReg(REG_RX_CH1_SLOT, slot(0));
    writeReg(REG_RX_CH2_SLOT, slot(1));
    writeReg(REG_TX_CH1_SLOT, slot(0));
    writeReg(REG_TX_CH2_SLOT, slot(1));
}

static void codecConfigurePdmMic() {
    writeReg(REG_GPIO1_CFG, GPIO1_PDM_CLK);
    writeReg(REG_GPI1_CFG,  GPI1_INPUT);
    writeReg(REG_INTF_CFG4, INTF_CFG4_GPI1_PDM_3_4);
    writeReg(REG_TX_CH3_SLOT, slot(2));
    writeReg(REG_TX_CH4_SLOT, slot(3));
}

static void codecConfigureAdcInputs() {
    writeReg(REG_ADC_CH1_CFG0, ADC_CFG0_SE_INP_ONLY);
    writeReg(REG_ADC_CH2_CFG0, ADC_CFG0_SE_INP_ONLY);
}

static void codecConfigureDacOutputs() {
    writeReg(REG_OUT1_CFG0, OUT_SRC_DAC | OUT_ROUTE_MONO_SE_P);
    writeReg(REG_OUT1_CFG1, OUT_CFG1_HP_0DB);
    writeReg(REG_OUT1_CFG2, OUT_CFG1_HP_0DB);
    writeReg(REG_OUT2_CFG0, OUT_SRC_DAC | OUT_ROUTE_MONO_SE_P);
    writeReg(REG_OUT2_CFG1, OUT_CFG1_HP_0DB);
    writeReg(REG_OUT2_CFG2, OUT_CFG1_HP_0DB);
}

static void codecMuteDacVolume() {
    // Boot the DAC at the mute code (volume == 0) so the chip powers up
    // silent. The DAC volume registers are written BEFORE codecPowerUp()
    // turns on the DAC channel via PWR_CFG, so when audio first starts
    // flowing the analog output is already at -inf. setup() releases the
    // boot gate later by calling g_codecPanel.unmuteOutput(), which
    // writes DAC_VOL_0DB into all four registers.
    writeReg(REG_DAC_L1_VOL, 0);
    writeReg(REG_DAC_R1_VOL, 0);
    writeReg(REG_DAC_L2_VOL, 0);
    writeReg(REG_DAC_R2_VOL, 0);
}

static void codecPowerUp() {
    writeReg(REG_CH_EN,   CH_EN_ALL);
    writeReg(REG_PWR_CFG, PWR_CFG_ADC_DAC_MICBIAS);
    delay(100);
}

static void setupCodecHandRolled() {
    Serial.println("Initializing TAC5212 (Phase 1 hand-rolled path)...");
    codecResetAndWake();
    codecConfigureSerialInterface();
    codecConfigureSlotMappings();
    codecConfigurePdmMic();
    codecConfigureAdcInputs();
    codecConfigureDacOutputs();
    codecMuteDacVolume();  // boot gate: DAC powers up muted (volume == 0)
    codecPowerUp();
    Serial.print("DEV_STS0: 0x");
    Serial.println(readReg(REG_DEV_STS0), HEX);
    Serial.println("Codec ready: TDM + PDM + ADC");
}

// ============================================================================
// TDspMixer integration callbacks
// ============================================================================

// Forward decl — defined below near pollCaptureHostVolume so it can
// see the sketch-local statics (s_lastCapVolRaw etc), but called here.
static void broadcastSnapshot(OSCBundle &reply);

// Forward decls — defined later in the file alongside their static
// state. setup() calls these once at the end of init to read the
// initial USB Feature Unit values into the audio graph BEFORE the
// boot-gate release (g_codecPanel.unmuteOutput()), so the first
// audio frame out of the DAC reflects whatever Windows has already
// pushed instead of the cold-boot defaults.
static void pollHostVolume();
static void pollCaptureHostVolume();

// Apply the line input mode to the codec and audio graph.
// Mono mode: ADC CH1 differential (IN1+/IN1-) reads the balanced mic;
//            CH2 disabled because IN2+ is tied to the mic's ring/cold
//            wire and would otherwise load it and buzz.
// Stereo mode: CH1 = Line L (single-ended), CH2 = Line R (single-ended).
//
// Wiring assumption (user TRS cable):
//   Tip  -> IN1+               (CH1 hot)
//   Ring -> IN2+ and IN1-      (CH2 hot AND CH1 cold)
//   CH1 differential reads tip - ring = hot - cold = full mic signal.
static void applyLineMode(bool mono) {
    g_lineMonoMode = mono;

    if (mono) {
        // ADC CH1 → differential (IN1+ / IN1-)
        g_codec.adc(1).setMode(tac5212::AdcMode::Differential);
        // Disable ADC CH2 — its hot input (IN2+) is tied to the mic's
        // ring/cold wire. Leaving CH2 powered on loads that signal and
        // causes buzz. Read-modify-write CH_EN to clear IN_CH2 (bit 6).
        {
            uint8_t chEn = g_codec.readRegister(0, 0x76);
            g_codec.writeRegister(0, 0x76, chEn & ~0x40);
        }
        // Cross-feed: ch3 (Line L) mirrors into mixR[3], ch4 muted
        monoXfeed.gain(1.0f);
        g_binding.setMonoMirror(3, 4, &mixR, 3);
    } else {
        // ADC CH1 → single-ended on INxP
        g_codec.adc(1).setMode(tac5212::AdcMode::SingleEndedInp);
        // Re-enable ADC CH2
        {
            uint8_t chEn = g_codec.readRegister(0, 0x76);
            g_codec.writeRegister(0, 0x76, chEn | 0x40);
        }
        // Cross-feed off, restore ch4
        monoXfeed.gain(0.0f);
        g_binding.clearMonoMirror();
        // Clear the cross-feed mixer slot
        mixR.gain(3, 0.0f);
    }

    // Re-apply ch3 + ch4 gains through the binding so the mirror
    // (or restored ch4) takes effect immediately.
    g_binding.applyChannel(3);
    g_binding.applyChannel(4);
}

// OSC frame arrived from the transport. Build a reply bundle, route into
// the dispatcher (which handles /ch/..., /main/..., /sub, /info, and
// forwards /codec/tac5212/* to Tac5212Panel), and flush the reply back
// via SLIP if anything was added.
//
// One address is intercepted BEFORE dispatching: /snapshot (no args)
// triggers a full-state dump and bypasses the dispatcher entirely,
// since the dispatcher doesn't know about the sketch-local capture-
// side hostvol state. A client sends /snapshot on connect to catch
// up to live firmware state without waiting for the next change.
static void onOscMessage(OSCMessage &msg, void *userData) {
    (void)userData;

    char address[32];
    int addrLen = msg.getAddress(address, 0, sizeof(address) - 1);
    if (addrLen < 0) addrLen = 0;
    address[addrLen] = '\0';

    if (strcmp(address, "/snapshot") == 0) {
        OSCBundle reply;
        broadcastSnapshot(reply);
        if (reply.size() > 0) {
            g_transport.sendBundle(reply);
        }
        return;
    }

    if (strcmp(address, "/line/mode") == 0) {
        OSCBundle reply;
        // Read (no args) → echo current; Write (string arg) → set.
        if (msg.size() == 0) {
            OSCMessage m("/line/mode");
            m.add(g_lineMonoMode ? "mono" : "stereo");
            reply.add(m);
        } else {
            char val[16] = {0};
            if (msg.isString(0)) msg.getString(0, val, sizeof(val));
            bool mono = (strcmp(val, "mono") == 0);
            applyLineMode(mono);
            OSCMessage m("/line/mode");
            m.add(mono ? "mono" : "stereo");
            reply.add(m);
        }
        if (reply.size() > 0) g_transport.sendBundle(reply);
        return;
    }

    // /midi/note/in i i i — UI-originated note (synth tab's on-screen
    // keyboard). Feeds the MidiRouter the same way the USB-host keyboard
    // does, so downstream consumers (MidiVizSink broadcast, future synth
    // engines) see UI notes and hardware notes identically. The router
    // folds velocity==0 into note-off per the standard running-status
    // rule, so this handler doesn't have to split the cases itself.
    if (strcmp(address, "/midi/note/in") == 0 && msg.size() >= 3
        && msg.isInt(0) && msg.isInt(1) && msg.isInt(2)) {
        uint8_t note     = (uint8_t)msg.getInt(0);
        uint8_t velocity = (uint8_t)msg.getInt(1);
        uint8_t channel  = (uint8_t)msg.getInt(2);
        if (velocity > 0) g_midiRouter.handleNoteOn(channel, note, velocity);
        else              g_midiRouter.handleNoteOff(channel, note, 0);
        return;
    }

    // /synth/dexed/volume f — Dexed output level into preMix slot 1.
    // 0..1 linear on the g_dexedGain amplifier. No arg = read (echo
    // current); float arg = write. Echoes on change so cross-client sync
    // works the same as the codec panel.
    if (strcmp(address, "/synth/dexed/volume") == 0) {
        OSCBundle reply;
        if (msg.size() == 0) {
            OSCMessage m("/synth/dexed/volume");
            m.add(g_dexedVolume);
            reply.add(m);
        } else if (msg.isFloat(0)) {
            applyDexedVolume(msg.getFloat(0));
            OSCMessage m("/synth/dexed/volume");
            m.add(g_dexedVolume);
            reply.add(m);
        }
        if (reply.size() > 0) g_transport.sendBundle(reply);
        return;
    }

    // /synth/dexed/midi/ch i — which MIDI channel Dexed listens on.
    // 0 = omni (accept all channels), 1..16 = single-channel. Clamped
    // to valid range in DexedSink::setListenChannel. Values outside
    // 0..16 are treated as omni (the safer default).
    if (strcmp(address, "/synth/dexed/midi/ch") == 0) {
        OSCBundle reply;
        if (msg.size() == 0) {
            OSCMessage m("/synth/dexed/midi/ch");
            m.add((int)g_dexedSink.listenChannel());
            reply.add(m);
        } else if (msg.isInt(0)) {
            int ch = msg.getInt(0);
            if (ch < 0) ch = 0;
            if (ch > 16) ch = 0;
            // Release any notes currently held on the old channel so a
            // channel switch mid-performance doesn't leave voices stuck.
            g_dexed.panic();
            g_dexedSink.setListenChannel((uint8_t)ch);
            OSCMessage m("/synth/dexed/midi/ch");
            m.add((int)g_dexedSink.listenChannel());
            reply.add(m);
        }
        if (reply.size() > 0) g_transport.sendBundle(reply);
        return;
    }

    // --- Shared FX bus: per-synth send + chorus + reverb -------------
    //
    // Addresses:
    //   /synth/dexed/fx/send f   — Dexed's send amount into the bus
    //   /fx/chorus/enable   i
    //   /fx/chorus/voices   i   (2..8 when enabled; bypass otherwise)
    //   /fx/reverb/enable   i
    //   /fx/reverb/size     f   (0..1 roomsize)
    //   /fx/reverb/damping  f   (0..1)
    //   /fx/reverb/return   f   (0..1 wet amount into main mix)

    if (strcmp(address, "/synth/dexed/fx/send") == 0) {
        OSCBundle reply;
        if (msg.size() == 0) {
            OSCMessage m("/synth/dexed/fx/send");
            m.add(g_dexedSendAmt);
            reply.add(m);
        } else if (msg.isFloat(0)) {
            float v = msg.getFloat(0);
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            g_dexedSendAmt = v;
            applyDexedSend();
            OSCMessage m("/synth/dexed/fx/send");
            m.add(g_dexedSendAmt);
            reply.add(m);
        }
        if (reply.size() > 0) g_transport.sendBundle(reply);
        return;
    }

    if (strcmp(address, "/fx/chorus/enable") == 0) {
        OSCBundle reply;
        if (msg.size() == 0) {
            OSCMessage m("/fx/chorus/enable");
            m.add(g_fxChorusEnable ? 1 : 0);
            reply.add(m);
        } else if (msg.isInt(0)) {
            g_fxChorusEnable = msg.getInt(0) != 0;
            applyFxChorus();
            OSCMessage m("/fx/chorus/enable");
            m.add(g_fxChorusEnable ? 1 : 0);
            reply.add(m);
        }
        if (reply.size() > 0) g_transport.sendBundle(reply);
        return;
    }

    if (strcmp(address, "/fx/chorus/voices") == 0) {
        OSCBundle reply;
        if (msg.size() == 0) {
            OSCMessage m("/fx/chorus/voices");
            m.add(g_fxChorusVoices);
            reply.add(m);
        } else if (msg.isInt(0)) {
            int n = msg.getInt(0);
            if (n < 2) n = 2;
            if (n > 8) n = 8;
            g_fxChorusVoices = n;
            applyFxChorus();
            OSCMessage m("/fx/chorus/voices");
            m.add(g_fxChorusVoices);
            reply.add(m);
        }
        if (reply.size() > 0) g_transport.sendBundle(reply);
        return;
    }

    if (strcmp(address, "/fx/reverb/enable") == 0) {
        OSCBundle reply;
        if (msg.size() == 0) {
            OSCMessage m("/fx/reverb/enable");
            m.add(g_fxReverbEnable ? 1 : 0);
            reply.add(m);
        } else if (msg.isInt(0)) {
            g_fxReverbEnable = msg.getInt(0) != 0;
            applyFxReverb();
            OSCMessage m("/fx/reverb/enable");
            m.add(g_fxReverbEnable ? 1 : 0);
            reply.add(m);
        }
        if (reply.size() > 0) g_transport.sendBundle(reply);
        return;
    }

    if (strcmp(address, "/fx/reverb/size") == 0) {
        OSCBundle reply;
        if (msg.size() == 0) {
            OSCMessage m("/fx/reverb/size");
            m.add(g_fxReverbRoomSize);
            reply.add(m);
        } else if (msg.isFloat(0)) {
            float v = msg.getFloat(0);
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            g_fxReverbRoomSize = v;
            applyFxReverb();
            OSCMessage m("/fx/reverb/size");
            m.add(g_fxReverbRoomSize);
            reply.add(m);
        }
        if (reply.size() > 0) g_transport.sendBundle(reply);
        return;
    }

    if (strcmp(address, "/fx/reverb/damping") == 0) {
        OSCBundle reply;
        if (msg.size() == 0) {
            OSCMessage m("/fx/reverb/damping");
            m.add(g_fxReverbDamping);
            reply.add(m);
        } else if (msg.isFloat(0)) {
            float v = msg.getFloat(0);
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            g_fxReverbDamping = v;
            applyFxReverb();
            OSCMessage m("/fx/reverb/damping");
            m.add(g_fxReverbDamping);
            reply.add(m);
        }
        if (reply.size() > 0) g_transport.sendBundle(reply);
        return;
    }

    if (strcmp(address, "/fx/reverb/return") == 0) {
        OSCBundle reply;
        if (msg.size() == 0) {
            OSCMessage m("/fx/reverb/return");
            m.add(g_fxReverbReturnAmt);
            reply.add(m);
        } else if (msg.isFloat(0)) {
            float v = msg.getFloat(0);
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            g_fxReverbReturnAmt = v;
            applyFxReverb();
            OSCMessage m("/fx/reverb/return");
            m.add(g_fxReverbReturnAmt);
            reply.add(m);
        }
        if (reply.size() > 0) g_transport.sendBundle(reply);
        return;
    }

    // --- Main-bus processing: high-shelf EQ + peak-limit waveshaper --
    //
    // Addresses live under /proc/ so they're clearly separate from the
    // mixer's /main/st/* tree; the Processing tab in the web UI owns
    // this namespace. All four fields follow the standard pattern:
    // no args = echo current, typed args = write-then-echo.

    if (strcmp(address, "/proc/shelf/enable") == 0) {
        OSCBundle reply;
        if (msg.size() == 0) {
            OSCMessage m("/proc/shelf/enable");
            m.add(g_procShelfEnable ? 1 : 0);
            reply.add(m);
        } else if (msg.isInt(0)) {
            g_procShelfEnable = msg.getInt(0) != 0;
            applyProcShelf();
            OSCMessage m("/proc/shelf/enable");
            m.add(g_procShelfEnable ? 1 : 0);
            reply.add(m);
        }
        if (reply.size() > 0) g_transport.sendBundle(reply);
        return;
    }

    if (strcmp(address, "/proc/shelf/freq") == 0) {
        OSCBundle reply;
        if (msg.size() == 0) {
            OSCMessage m("/proc/shelf/freq");
            m.add(g_procShelfFreqHz);
            reply.add(m);
        } else if (msg.isFloat(0)) {
            float f = msg.getFloat(0);
            // Clamp to a sensible range — the biquad math is stable
            // well beyond this, but values outside 500..18000 aren't
            // useful for the "tame harshness" job this shelf does.
            if (f < 500.0f)   f = 500.0f;
            if (f > 18000.0f) f = 18000.0f;
            g_procShelfFreqHz = f;
            applyProcShelf();
            OSCMessage m("/proc/shelf/freq");
            m.add(g_procShelfFreqHz);
            reply.add(m);
        }
        if (reply.size() > 0) g_transport.sendBundle(reply);
        return;
    }

    if (strcmp(address, "/proc/shelf/gain") == 0) {
        OSCBundle reply;
        if (msg.size() == 0) {
            OSCMessage m("/proc/shelf/gain");
            m.add(g_procShelfGainDb);
            reply.add(m);
        } else if (msg.isFloat(0)) {
            float g = msg.getFloat(0);
            // Cut-only semantics by convention: positive shelf gains
            // add sizzle, which is the opposite of what this control
            // exists to do. Clamp to [-18, 0] dB.
            if (g < -18.0f) g = -18.0f;
            if (g >   0.0f) g =   0.0f;
            g_procShelfGainDb = g;
            applyProcShelf();
            OSCMessage m("/proc/shelf/gain");
            m.add(g_procShelfGainDb);
            reply.add(m);
        }
        if (reply.size() > 0) g_transport.sendBundle(reply);
        return;
    }

    if (strcmp(address, "/proc/limiter/enable") == 0) {
        OSCBundle reply;
        if (msg.size() == 0) {
            OSCMessage m("/proc/limiter/enable");
            m.add(g_procLimiterEnable ? 1 : 0);
            reply.add(m);
        } else if (msg.isInt(0)) {
            g_procLimiterEnable = msg.getInt(0) != 0;
            applyProcLimiter();
            OSCMessage m("/proc/limiter/enable");
            m.add(g_procLimiterEnable ? 1 : 0);
            reply.add(m);
        }
        if (reply.size() > 0) g_transport.sendBundle(reply);
        return;
    }

    // /synth/dexed/voice i i — select bundled DX7 voice (bank, voice).
    // No args = read (echo current bank, voice, and trimmed name). Two
    // int args = write. Reply always contains the voice NAME as the
    // third arg so a UI can label the dropdown without a second
    // round-trip.
    if (strcmp(address, "/synth/dexed/voice") == 0) {
        OSCBundle reply;
        bool send = false;
        if (msg.size() == 0) {
            send = true;
        } else if (msg.size() >= 2 && msg.isInt(0) && msg.isInt(1)) {
            applyDexedVoice(msg.getInt(0), msg.getInt(1));
            send = true;
        }
        if (send) {
            char name[tdsp::dexed::kVoiceNameBufBytes] = {0};
            tdsp::dexed::copyVoiceName(g_dexedBank, g_dexedVoice, name, sizeof(name));
            OSCMessage m("/synth/dexed/voice");
            m.add(g_dexedBank);
            m.add(g_dexedVoice);
            m.add(name);
            reply.add(m);
        }
        if (reply.size() > 0) g_transport.sendBundle(reply);
        return;
    }

    // /synth/dexed/voice/names i — return the 32 voice names for the
    // given bank as a single message carrying 32 string args, in order
    // (voice 0 first). The UI queries this on bank-select to populate
    // its voice dropdown. Bank index is clamped; an out-of-range value
    // gets the first bank's names.
    if (strcmp(address, "/synth/dexed/voice/names") == 0 && msg.size() >= 1 && msg.isInt(0)) {
        int bank = msg.getInt(0);
        if (bank < 0)                           bank = 0;
        if (bank >= tdsp::dexed::kNumBanks)     bank = tdsp::dexed::kNumBanks - 1;
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

    // /synth/dexed/bank/names — return all bank names as a single
    // message of 10 string args (bank 0 first). Hardcoded on the
    // firmware side because the bank set is compile-time fixed.
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

    // ------------- /synth/mpe/* — MPE-native VA synth engine -------------
    //
    // 11 addresses total. All follow the read/write/echo pattern: empty
    // args = read-back, single-arg write updates and echoes. The sink
    // itself handles all range clamping so this handler can forward
    // raw values and trust the sink's getters for the echo.

    if (strcmp(address, "/synth/mpe/volume") == 0) {
        OSCBundle reply;
        if (msg.size() == 0) {
            OSCMessage m("/synth/mpe/volume"); m.add(g_mpeVolume); reply.add(m);
        } else if (msg.isFloat(0)) {
            applyMpeVolume(msg.getFloat(0));
            OSCMessage m("/synth/mpe/volume"); m.add(g_mpeVolume); reply.add(m);
        }
        if (reply.size() > 0) g_transport.sendBundle(reply);
        return;
    }

    if (strcmp(address, "/synth/mpe/attack") == 0) {
        OSCBundle reply;
        if (msg.size() == 0) {
            OSCMessage m("/synth/mpe/attack"); m.add(g_mpeSink.attack()); reply.add(m);
        } else if (msg.isFloat(0)) {
            g_mpeSink.setAttack(msg.getFloat(0));
            OSCMessage m("/synth/mpe/attack"); m.add(g_mpeSink.attack()); reply.add(m);
        }
        if (reply.size() > 0) g_transport.sendBundle(reply);
        return;
    }

    if (strcmp(address, "/synth/mpe/release") == 0) {
        OSCBundle reply;
        if (msg.size() == 0) {
            OSCMessage m("/synth/mpe/release"); m.add(g_mpeSink.release()); reply.add(m);
        } else if (msg.isFloat(0)) {
            g_mpeSink.setRelease(msg.getFloat(0));
            OSCMessage m("/synth/mpe/release"); m.add(g_mpeSink.release()); reply.add(m);
        }
        if (reply.size() > 0) g_transport.sendBundle(reply);
        return;
    }

    if (strcmp(address, "/synth/mpe/waveform") == 0) {
        OSCBundle reply;
        if (msg.size() == 0) {
            OSCMessage m("/synth/mpe/waveform"); m.add((int)g_mpeSink.waveform()); reply.add(m);
        } else if (msg.isInt(0)) {
            int w = msg.getInt(0);
            if (w < 0 || w > 3) w = 0;
            g_mpeSink.setWaveform((uint8_t)w);
            OSCMessage m("/synth/mpe/waveform"); m.add((int)g_mpeSink.waveform()); reply.add(m);
        }
        if (reply.size() > 0) g_transport.sendBundle(reply);
        return;
    }

    if (strcmp(address, "/synth/mpe/filter/cutoff") == 0) {
        OSCBundle reply;
        if (msg.size() == 0) {
            OSCMessage m("/synth/mpe/filter/cutoff"); m.add(g_mpeSink.filterCutoff()); reply.add(m);
        } else if (msg.isFloat(0)) {
            g_mpeSink.setFilterCutoff(msg.getFloat(0));
            OSCMessage m("/synth/mpe/filter/cutoff"); m.add(g_mpeSink.filterCutoff()); reply.add(m);
        }
        if (reply.size() > 0) g_transport.sendBundle(reply);
        return;
    }

    if (strcmp(address, "/synth/mpe/filter/resonance") == 0) {
        OSCBundle reply;
        if (msg.size() == 0) {
            OSCMessage m("/synth/mpe/filter/resonance"); m.add(g_mpeSink.filterResonance()); reply.add(m);
        } else if (msg.isFloat(0)) {
            g_mpeSink.setFilterResonance(msg.getFloat(0));
            OSCMessage m("/synth/mpe/filter/resonance"); m.add(g_mpeSink.filterResonance()); reply.add(m);
        }
        if (reply.size() > 0) g_transport.sendBundle(reply);
        return;
    }

    if (strcmp(address, "/synth/mpe/lfo/rate") == 0) {
        OSCBundle reply;
        if (msg.size() == 0) {
            OSCMessage m("/synth/mpe/lfo/rate"); m.add(g_mpeSink.lfoRate()); reply.add(m);
        } else if (msg.isFloat(0)) {
            g_mpeSink.setLfoRate(msg.getFloat(0));
            OSCMessage m("/synth/mpe/lfo/rate"); m.add(g_mpeSink.lfoRate()); reply.add(m);
        }
        if (reply.size() > 0) g_transport.sendBundle(reply);
        return;
    }

    if (strcmp(address, "/synth/mpe/lfo/depth") == 0) {
        OSCBundle reply;
        if (msg.size() == 0) {
            OSCMessage m("/synth/mpe/lfo/depth"); m.add(g_mpeSink.lfoDepth()); reply.add(m);
        } else if (msg.isFloat(0)) {
            g_mpeSink.setLfoDepth(msg.getFloat(0));
            OSCMessage m("/synth/mpe/lfo/depth"); m.add(g_mpeSink.lfoDepth()); reply.add(m);
        }
        if (reply.size() > 0) g_transport.sendBundle(reply);
        return;
    }

    if (strcmp(address, "/synth/mpe/lfo/dest") == 0) {
        OSCBundle reply;
        if (msg.size() == 0) {
            OSCMessage m("/synth/mpe/lfo/dest"); m.add((int)g_mpeSink.lfoDest()); reply.add(m);
        } else if (msg.isInt(0)) {
            g_mpeSink.setLfoDest((uint8_t)msg.getInt(0));
            OSCMessage m("/synth/mpe/lfo/dest"); m.add((int)g_mpeSink.lfoDest()); reply.add(m);
        }
        if (reply.size() > 0) g_transport.sendBundle(reply);
        return;
    }

    if (strcmp(address, "/synth/mpe/lfo/waveform") == 0) {
        OSCBundle reply;
        if (msg.size() == 0) {
            OSCMessage m("/synth/mpe/lfo/waveform"); m.add((int)g_mpeSink.lfoWaveform()); reply.add(m);
        } else if (msg.isInt(0)) {
            g_mpeSink.setLfoWaveform((uint8_t)msg.getInt(0));
            OSCMessage m("/synth/mpe/lfo/waveform"); m.add((int)g_mpeSink.lfoWaveform()); reply.add(m);
        }
        if (reply.size() > 0) g_transport.sendBundle(reply);
        return;
    }

    if (strcmp(address, "/synth/mpe/midi/master") == 0) {
        OSCBundle reply;
        if (msg.size() == 0) {
            OSCMessage m("/synth/mpe/midi/master"); m.add((int)g_mpeSink.masterChannel()); reply.add(m);
        } else if (msg.isInt(0)) {
            int ch = msg.getInt(0);
            if (ch < 1 || ch > 16) ch = 1;
            // Release everything before re-interpreting the master
            // channel so a mid-performance switch doesn't strand
            // voices on the (now-reinterpreted) old master.
            g_mpeSink.onAllNotesOff(0);
            g_mpeSink.setMasterChannel((uint8_t)ch);
            OSCMessage m("/synth/mpe/midi/master"); m.add((int)g_mpeSink.masterChannel()); reply.add(m);
        }
        if (reply.size() > 0) g_transport.sendBundle(reply);
        return;
    }

    // /synth/mpe/fx/send f — MPE's contribution into the shared FX bus
    // (chorus → reverb chain). 0..1 linear; 0 = dry only.
    if (strcmp(address, "/synth/mpe/fx/send") == 0) {
        OSCBundle reply;
        if (msg.size() == 0) {
            OSCMessage m("/synth/mpe/fx/send"); m.add(g_mpeSendAmount); reply.add(m);
        } else if (msg.isFloat(0)) {
            applyMpeSend(msg.getFloat(0));
            OSCMessage m("/synth/mpe/fx/send"); m.add(g_mpeSendAmount); reply.add(m);
        }
        if (reply.size() > 0) g_transport.sendBundle(reply);
        return;
    }

    // /sub target=/midi/events — subscription for the MIDI visualization
    // broadcast. Intercepted here (not in OscDispatcher) to keep the MIDI
    // path confined to this sketch: OscDispatcher::handleSub only
    // recognises /meters/ and /spectrum/ target prefixes. For the MIDI
    // target we toggle g_midiVizSink's enable flag — the sink stays
    // registered with the router either way, but it only produces output
    // when enabled, so events still reach future synth sinks regardless
    // of whether a web client is watching.
    if (strcmp(address, "/sub") == 0 && msg.size() >= 2 && msg.isString(0)) {
        const int last = msg.size() - 1;
        if (msg.isString(last)) {
            char target[32] = {0};
            int tl = msg.getString(last, target, sizeof(target));
            target[(tl < (int)sizeof(target)) ? tl : (int)sizeof(target) - 1] = '\0';
            if (strcmp(target, "/midi/events") == 0) {
                char verb[16] = {0};
                int vl = msg.getString(0, verb, sizeof(verb));
                verb[(vl < (int)sizeof(verb)) ? vl : (int)sizeof(verb) - 1] = '\0';
                if (strcmp(verb, "addSub") == 0) g_midiVizSink.setEnabled(true);
                else if (strcmp(verb, "unsubscribe") == 0) g_midiVizSink.setEnabled(false);
                return;
            }
            if (strcmp(target, "/synth/mpe/voices") == 0) {
                char verb[16] = {0};
                int vl = msg.getString(0, verb, sizeof(verb));
                verb[(vl < (int)sizeof(verb)) ? vl : (int)sizeof(verb) - 1] = '\0';
                if (strcmp(verb, "addSub") == 0) {
                    g_mpeVoicesEnabled = true;
                    // Fire one frame immediately so the UI paints
                    // current state without waiting up to 33 ms.
                    broadcastMpeVoices();
                } else if (strcmp(verb, "unsubscribe") == 0) {
                    g_mpeVoicesEnabled = false;
                }
                return;
            }
        }
    }

    OSCBundle reply;
    g_dispatcher.route(msg, reply);
    if (reply.size() > 0) {
        g_transport.sendBundle(reply);
    }
}

// CLI line arrived (plain ASCII). Recognized commands:
//   s / S  — status dump (audio memory, CPU, model state)
//   unsub_all — disable all streaming engines (meters, spectrum).
//              Used by the serial-bridge when the WebSocket client
//              disconnects so the firmware stops writing data that
//              nobody is reading.
static void onCliLine(char *line, int length, void *userData) {
    (void)userData;
    (void)length;
    if (!line) return;
    if (strcmp(line, "unsub_all") == 0) {
        g_meters.setEnabled(false);
        g_spectrum.setEnabled(false);
        g_midiVizSink.setEnabled(false);
        g_mpeVoicesEnabled = false;
        Serial.println("streaming disabled");
        return;
    }
    if (line[0] == 's' || line[0] == 'S') {
        Serial.println("\n--- Status ---");
        Serial.print("Audio Mem: ");
        Serial.print(AudioMemoryUsage());
        Serial.print("/");
        Serial.println(AudioMemoryUsageMax());
        Serial.print("CPU: ");
        Serial.print(AudioProcessorUsage(), 2);
        Serial.println("%");
        Serial.print("USB host vol raw: ");
        Serial.print(usbIn.volume(), 4);
        Serial.print("  scaled: ");
        Serial.println(g_model.main().hostvolValue, 3);
        // Capture-side feature unit (USB FU 0x30) — driven by the Windows
        // recording-device slider. Added by the teensy4 core patch on
        // branch teensy4-usb-audio-capture-feature-unit. If this prints
        // 0.5000 / mute=0 right after enumeration, the descriptor is
        // accepted; if it changes when you drag the slider in Windows
        // Sound -> Recording -> Properties -> Levels, the SET_CUR
        // dispatch is routing to the right entity ID.
        Serial.print("USB capture vol raw: ");
        Serial.print(usbOut.volume(), 4);
        Serial.print("  mute: ");
        Serial.println(usbOut.mute() ? 1 : 0);
        for (int n = 1; n <= tdsp::kChannelCount; ++n) {
            Serial.print("  ");
            Serial.print(g_model.channel(n).name);
            Serial.print("  fader=");
            Serial.print(g_model.channel(n).fader, 3);
            Serial.print("  on=");
            Serial.print(g_model.channel(n).on ? 1 : 0);
            Serial.print("  solo=");
            Serial.println(g_model.channel(n).solo ? 1 : 0);
        }
        Serial.print("Main  faderL=");
        Serial.print(g_model.main().faderL, 3);
        Serial.print("  faderR=");
        Serial.print(g_model.main().faderR, 3);
        Serial.print("  link=");
        Serial.print(g_model.main().link ? 1 : 0);
        Serial.print("  on=");
        Serial.print(g_model.main().on ? 1 : 0);
        Serial.print("  hostvol.enable=");
        Serial.print(g_model.main().hostvolEnable ? 1 : 0);
        Serial.print("  hostvol.value=");
        Serial.println(g_model.main().hostvolValue, 3);
        Serial.print("DEV_STS0: 0x");
        Serial.println(readReg(REG_DEV_STS0), HEX);
        Serial.println("--------------\n");
    }
    // Unknown CLI input is silently ignored for MVP.
}

// ============================================================================
// setup()
// ============================================================================

void setup() {
    pinMode(TAC5212_EN_PIN, OUTPUT);
    digitalWrite(TAC5212_EN_PIN, HIGH);

    Serial.begin(115200);
    pinMode(LED_BUILTIN, OUTPUT);

    while (!Serial && millis() < 3000) {
        digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
        delay(100);
    }

    Serial.println("================================");
    Serial.println("  T-DSP TAC5212 Audio Shield");
    Serial.println("  MVP v1 (TDspMixer + WebSerial)");
    Serial.println("================================");

    delay(100);
    Wire.begin();

    Wire.beginTransmission(TAC5212_I2C_ADDR);
    if (Wire.endTransmission() != 0) {
        Serial.println("TAC5212 not found!");
        while (1) { delay(100); }
    }

    // Codec init via the Phase 1 hand-rolled path. After this, g_codec
    // (lib/TAC5212) is ALSO valid for runtime register access through
    // Tac5212Panel without a re-init. Never call g_codec.begin() — that
    // would SW-reset the chip and wipe the hand-rolled init.
    setupCodecHandRolled();

    // Pool sized for all taps + fan-outs in the graph. Was 96 for the
    // pre-synth graph; bumped to 144 for Phase 2c when Dexed (8 voices,
    // each with its own operator mix buffers) and the preMix stage
    // joined. Dexed alone asks for ~30 blocks peak under full polyphony;
    // the preMix adds ~4; the existing graph was already near 70 under
    // load. Monitor via the 's' CLI — AudioMemoryUsageMax() should stay
    // well below this ceiling.
    //
    // Phase 2d adds the MPE VA engine on preMix slot 2: 4 oscs + 4
    // filters + 4 envs + 2 mixers + 3 amps = 17 primitives, each
    // envelope fan-outs into both L+R. Conservatively +24 blocks peak
    // for fan-out headroom → 168.
    AudioMemory(168);

    // PDM mic amplitude trim: 32-bit PDM split across two 16-bit slots
    // — high 16 bits need a 16x boost, low 16 bits need a 1/65536
    // scaling so they add up to the full 32-bit value.
    const float kPdmGain = 16.0f;
    pdmMixL.gain(0, kPdmGain);
    pdmMixL.gain(1, kPdmGain / 65536.0f);
    pdmMixR.gain(0, kPdmGain);
    pdmMixR.gain(1, kPdmGain / 65536.0f);

    // preMix bus: slot 0 = input-channel mix, slot 1 = Dexed (volume
    // is controlled by g_dexedGain upstream, so the slot itself is
    // unity), slot 2 = TDspMPE (same pattern — mpeGainL/R upstream
    // controls volume, this slot just passes), slot 3 = shared FX
    // bus wet return (level owned by fxReturnL/R amps upstream, so
    // the slot is unity here — wet amount is controlled by
    // g_fxReverbReturnAmt via applyFxReverb()).
    preMixL.gain(0, 1.0f);
    preMixL.gain(1, 1.0f);
    preMixL.gain(2, 1.0f);
    preMixL.gain(3, 1.0f);
    preMixR.gain(0, 1.0f);
    preMixR.gain(1, 1.0f);
    preMixR.gain(2, 1.0f);
    preMixR.gain(3, 1.0f);

    // Capture mixers at unity on every slot. The per-source gating is
    // done by the upstream rec/loop amps (applyChannelRec / applyMainLoop
    // in the binding), so the mixer itself is just a passive sum.
    captureL.gain(0, 1.0f);
    captureL.gain(1, 1.0f);
    captureL.gain(2, 1.0f);
    captureL.gain(3, 1.0f);
    captureR.gain(0, 1.0f);
    captureR.gain(1, 1.0f);
    captureR.gain(2, 1.0f);
    captureR.gain(3, 1.0f);

    // Listenback monitor amps default to unity. pollCaptureHostVolume()
    // will overwrite them as soon as Windows pushes a SET_CUR for FU 0x30.
    monLineL.gain(1.0f);
    monLineR.gain(1.0f);
    monMicL.gain(1.0f);
    monMicR.gain(1.0f);

    // Mono cross-feed off by default (stereo mode).
    monoXfeed.gain(0.0f);

    // Force the capture-side Feature Unit's cold-boot value to 100% so
    // the headphone monitor is at unity until Windows tells us otherwise.
    // The Teensy core defaults this to FEATURE_MAX_VOLUME/2 = 128 (matches
    // the playback FU pattern), which at our linear taper would silently
    // attenuate listenback by ~6 dB on every cold boot. This also means
    // GET_CUR returns FEATURE_MAX_VOLUME on first query, so Windows shows
    // the recording slider at 100% on enumeration. Set BEFORE the first
    // pollCaptureHostVolume() call so the change-detection logic doesn't
    // fire a spurious /usb/cap/hostvol/value 0.502 broadcast.
    AudioOutputUSB::features.volume = FEATURE_MAX_VOLUME;

    // --- Wire TDspMixer to the audio graph ---

    g_binding.setModel(&g_model);

    // Register each channel with its main-mixer slot. Per-channel HPF
    // objects are nullptr for MVP — the model tracks HPF state but the
    // audio path doesn't have biquads wired in yet. Follow-on commit
    // adds the biquad chain once everything else is green.
    g_binding.setChannel(1, &mixL, 0, nullptr);  // USB L
    g_binding.setChannel(2, &mixR, 0, nullptr);  // USB R
    g_binding.setChannel(3, &mixL, 2, nullptr);  // Line L
    g_binding.setChannel(4, &mixR, 2, nullptr);  // Line R
    g_binding.setChannel(5, &mixL, 1, nullptr);  // Mic L
    g_binding.setChannel(6, &mixR, 1, nullptr);  // Mic R
    g_binding.setMain(&mainAmpL, &mainAmpR);
    g_binding.setMainHostvol(&hostvolAmpL, &hostvolAmpR);
    // Per-source USB record-send amps and main loop tap. Gain is 0/1,
    // driven by Channel.recSend / Main.loopEnable (see SignalGraphBinding).
    g_binding.setChannelRecAmp(1, &recUsbL);
    g_binding.setChannelRecAmp(2, &recUsbR);
    g_binding.setChannelRecAmp(3, &recLineL);
    g_binding.setChannelRecAmp(4, &recLineR);
    g_binding.setChannelRecAmp(5, &recMicL);
    g_binding.setChannelRecAmp(6, &recMicR);
    g_binding.setMainLoop(&loopL, &loopR);
    g_binding.applyAll();  // push initial model defaults into audio objects

    g_dispatcher.setModel(&g_model);
    g_dispatcher.setBinding(&g_binding);
    g_dispatcher.setMeterEngine(&g_meters);
    g_dispatcher.setSpectrumEngine(&g_spectrum);
    g_dispatcher.registerCodecPanel(&g_codecPanel);

    g_transport.begin(115200);
    g_transport.setOscMessageHandler(&onOscMessage, nullptr);
    g_transport.setCliLineHandler(&onCliLine, nullptr);

    g_meters.setDispatcher(&g_dispatcher);
    g_meters.setChannel(1, &peakCh1, &rmsCh1);
    g_meters.setChannel(2, &peakCh2, &rmsCh2);
    g_meters.setChannel(3, &peakCh3, &rmsCh3);
    g_meters.setChannel(4, &peakCh4, &rmsCh4);
    g_meters.setChannel(5, &peakCh5, &rmsCh5);
    g_meters.setChannel(6, &peakCh6, &rmsCh6);
    g_meters.setMain(&peakMainL, &rmsMainL, &peakMainR, &rmsMainR);
    g_meters.setHost(&peakHostL, &rmsHostL, &peakHostR, &rmsHostR);

    g_spectrum.setDispatcher(&g_dispatcher);
    g_spectrum.setChannels(&fftMainL, &fftMainR);

    // --- Boot-gate release ---
    //
    // The TAC5212 DAC has been muted since codecMuteDacVolume() ran inside
    // setupCodecHandRolled() — every analog output has been silent through
    // the entire init. Before un-muting, capture the current USB Feature
    // Unit values (playback FU 0x101 = Windows master volume slider,
    // capture FU 0x30 = Windows recording-device slider) and push them
    // into the audio graph so the first audio frame out of the DAC
    // reflects the actual host state instead of the cold-boot defaults.
    //
    // If the USB host hasn't enumerated yet (cold cable plug, slow host),
    // the polled values are the cold-boot defaults — playback FU at 50%
    // (usb_audio.cpp default), capture FU at 100% (the override applied
    // above). The next pollHostVolume() / pollCaptureHostVolume() in
    // loop() will overwrite both as soon as Windows pushes its real
    // SET_CUR. That brief window is bounded by USB enumeration time
    // (milliseconds), and during it the audio graph is at the loaded /
    // applied state — better than the unloaded state we had before
    // this gate existed.
    pollHostVolume();
    pollCaptureHostVolume();
    g_codecPanel.unmuteOutput();

    // Start the USB host stack. Non-blocking — enumeration happens over
    // later myusb.Task() calls in loop(). Every USBHost_t36 callback is
    // a thin forwarder into g_midiRouter, which fans out to registered
    // sinks. MidiVizSink is registered below; it's the only consumer
    // for Phase 2b. Synth-engine sinks get added in Phase 2c+.
    g_usbHost.begin();
    g_midiIn.setHandleNoteOn        (onUsbHostNoteOn);
    g_midiIn.setHandleNoteOff       (onUsbHostNoteOff);
    g_midiIn.setHandleControlChange (onUsbHostControlChange);
    g_midiIn.setHandlePitchChange   (onUsbHostPitchChange);
    g_midiIn.setHandleAfterTouch    (onUsbHostAfterTouch);
    g_midiIn.setHandleProgramChange (onUsbHostProgramChange);
    g_midiIn.setHandleSysEx         (onUsbHostSysEx);

    // Seed pitch bend range for the LinnStrument master + members.
    // LinnStrument factory default is 48 semi; it will re-assert this
    // via RPN 0 at power-on, but seeding up front means the very first
    // bend that arrives (before any RPN traffic) is scaled correctly.
    for (uint8_t ch = 1; ch <= tdsp::MidiRouter::kNumChannels; ++ch) {
        g_midiRouter.setPitchBendRange(ch, tdsp::MidiRouter::kDefaultPitchBendRange);
    }

    g_midiRouter.addSink(&g_midiVizSink);

    // --- Shared FX bus init -------------------------------------------
    //
    // Chorus needs its delay line installed via begin() before any
    // audio block hits it — update() early-returns if the delay line
    // pointer is null, which would mean silence in the wet path.
    // CRUCIAL: begin() returns false (and leaves the delay line NULL
    // forever) if n_chorus < 1, regardless of any later voices() call.
    // Always pass a non-zero count here; applyFxChorus() then flips
    // between voices(1)=passthrough and voices(2..8)=active.
    fxChorus.begin(g_fxChorusDelayLine, kFxChorusDelayLen, 2);
    // FX send bus per-slot gain: 0.5 (-6 dB) gives the reverb the
    // headroom it needs. AudioEffectFreeverbStereo sums 8 comb-filter
    // taps internally with int16 accumulators — feeding it near
    // full-scale is the classic recipe for overflow distortion. The
    // 6 dB headroom is the same convention hardware mixers use on
    // aux send buses for the same reason. The user's per-synth send
    // slider still spans 0..1, so they keep full control of how much
    // signal arrives here; we're just preventing arithmetic clipping
    // even at slider=1.0.
    for (int i = 0; i < 4; ++i) fxSendBus.gain(i, 0.5f);
    applyDexedSend();
    applyFxChorus();
    applyFxReverb();

    // --- Main-bus processing init -------------------------------------
    //
    // Configure the shelf biquad and limiter waveshaper before the
    // first audio block is produced. If we left them untouched, the
    // biquad would pass a silent (all-zero coefficients) default and
    // the waveshaper would skip its update() entirely (null table) —
    // both of those collapse the output to silence on the DAC path.
    // Calling these APIs here guarantees the graph is audibly valid
    // from block 0.
    applyProcShelf();
    applyProcLimiter();

    // --- Dexed engine init --------------------------------------------
    //
    // synth_dexed's default state after construction is silent — the
    // engine needs a voice loaded and a handful of controllers seeded
    // before it will produce sound on keydown. applyDexedVoice(0, 0)
    // loads bank 0 voice 0 ("FM-Rhodes" from RitChie 1) which is more
    // interesting than the library's built-in sine init voice and
    // tells the user immediately that the bank system works. Mod wheel,
    // pitch bend, sustain default to neutral positions.
    //
    // Volume starts at 0.7 (about -3 dB) — loud enough that "does it
    // work?" is obvious on first note-on, quiet enough that an
    // accidental all-voices-playing burst doesn't pin the DAC.
    applyDexedVoice(g_dexedBank, g_dexedVoice);
    g_dexed.setPitchbendRange(1);
    g_dexed.setPitchbend((int16_t)0);
    g_dexed.setModWheel(0);
    g_dexed.setSustain(false);
    applyDexedVolume(g_dexedVolume);

    // Register the Dexed sink with the router. Default listen channel
    // is 1 (see DexedSink constructor default), which matches both the
    // web UI on-screen keyboard (always sends on 1) and the typical
    // non-MPE hardware keyboard setup. LinnStrument in MPE mode sends
    // notes on channels 2..16, which Dexed ignores until the user flips
    // /synth/dexed/midi/ch to 0 (omni).
    g_midiRouter.addSink(&g_dexedSink);

    // --- MPE VA engine init -------------------------------------------
    //
    // mpeMixL/R slot gains at 0.25 each so 4 simultaneous voices at
    // unity sum to 1.0 without clipping. Per-voice amplitude is
    // already trimmed by velocity × pressure × setVoiceVolumeScale
    // inside MpeVaSink, so this mixer-side fraction is the pure
    // polyphony-headroom factor.
    for (int s = 0; s < 4; ++s) {
        mpeMixL.gain(s, 0.25f);
        mpeMixR.gain(s, 0.25f);
    }

    // Envelope defaults: 5 ms attack, 300 ms release. Decay (100 ms)
    // + sustain (0.7) are set per-voice directly since the sink's
    // public API only exposes A and R — the other two shape
    // parameters live at the envelope-object level.
    g_mpeSink.setAttack (0.005f);
    g_mpeSink.setRelease(0.300f);
    g_mpeSink.setWaveform(0);  // 0 = saw
    for (int i = 0; i < 4; ++i) {
        g_mpeVoices[i].env->decay  (100.0f);
        g_mpeVoices[i].env->sustain(0.7f);
    }

    // Filter defaults: wide open (8 kHz Butterworth). CC#74 steers
    // each voice's cutoff around this base by ±1 octave.
    g_mpeSink.setFilterCutoff   (8000.0f);
    g_mpeSink.setFilterResonance(0.707f);

    // LFO defaults: rate=0 disables the whole path. tick() is a
    // no-op until the user engages it.
    g_mpeSink.setLfoRate    (0.0f);
    g_mpeSink.setLfoDepth   (0.0f);
    g_mpeSink.setLfoDest    (MpeVaSink::LfoOff);
    g_mpeSink.setLfoWaveform(0);

    // Output volume + FX send. Volume 0.7 matches Dexed so both
    // synths sit at comparable loudness; send defaults to 0 (dry).
    applyMpeVolume(g_mpeVolume);
    applyMpeSend  (g_mpeSendAmount);

    // Register the MPE sink AFTER Dexed. When a LinnStrument sends a
    // member-channel note-on (ch 2..16) and Dexed is in omni mode, it
    // fires first; MPE then also fires. Users who want only MPE on
    // those notes leave Dexed on channel 1 (default).
    g_midiRouter.addSink(&g_mpeSink);

    Serial.println("\nReady!");
    Serial.println("  6 input channels: USB L/R, Line L/R, Mic L/R");
    Serial.println("  Main stereo -> DAC + USB capture");
    Serial.println("  Connect with the WebSerial app in tools/web_dev_surface/");
    Serial.println("  or type 's' in the serial monitor for status.");
}

// ============================================================================
// Host volume tracking (square-law taper preserved from Phase 1)
// ============================================================================

static float s_lastUsbVolRaw = -1.0f;

static void pollHostVolume() {
    const float raw = usbIn.volume();
    if (raw == s_lastUsbVolRaw) return;
    s_lastUsbVolRaw = raw;

    // Square-law taper: 50% slider → -12 dB, 25% → -24 dB. Phase 1
    // experimentation settled on this as "close enough to a real log-
    // taper pot" without the cost of an actual logf() per poll.
    float scaled = raw * raw;
    if (scaled > 1.0f) scaled = 1.0f;

    if (g_model.setMainHostvolValue(scaled)) {
        g_binding.applyMain();
        // Echo /main/st/hostvol/value to any subscribed clients.
        OSCBundle reply;
        g_dispatcher.broadcastMainHostvolValue(reply);
        if (reply.size() > 0) g_transport.sendBundle(reply);
    }
}

// ============================================================================
// USB capture-side host volume tracking (Windows recording-device slider)
// ============================================================================
//
// Companion to pollHostVolume() above, but for the OTHER USB Audio Class
// Feature Unit added by the teensy4 core patch on branch
// teensy4-usb-audio-capture-feature-unit. FU 0x30 lives on the device->host
// capture path; Windows binds it to the recording-device "Levels" tab
// slider. Dragging that slider sends SET_CUR over the USB control endpoint,
// which lands in AudioOutputUSB::features (volume + mute).
//
// We do not (yet) apply this gain to the audio path. The whole reason it
// exists is so the user can control listenback level from Windows. Wiring
// it into the capture mixer is a follow-on once the value is verified to
// be flowing correctly. For now we just broadcast it so the web dev
// surface can render it as a read-only "CAP HOST" strip and prove the
// SET_CUR routing works end to end.
//
// Wire format:
//   /usb/cap/hostvol/value f <0..1>
//   /usb/cap/hostvol/mute  i <0|1>
//
// Both addresses fire ONLY on change (rising-edge style). The dev surface
// signal store de-dupes too, so this just keeps the byte rate down.

static float s_lastCapVolRaw = -1.0f;
static int   s_lastCapMute   = -1;

// Compute the actual gain to apply to the listenback monitor amps from
// the raw FU 0x30 volume + mute state. Linear taper — NOT the square-law
// that pollHostVolume uses for the playback side. Reason: the sources
// this attenuates are line-level mic/line inputs, which are already
// quiet. Square-law gives ~-12 dB at 50% slider, dropping a mic below
// audibility; linear gives -6 dB at 50%, keeping usable range across
// the whole slider travel. The playback hostvol still uses square-law
// because it's attenuating the final DAC output which is much hotter —
// the tapers are tuned to the signal levels they act on. Mute folds
// into a hard 0.
static float captureMonitorGain(float rawVol, int rawMute) {
    if (rawMute) return 0.0f;
    if (rawVol < 0.0f) return 0.0f;
    if (rawVol > 1.0f) return 1.0f;
    return rawVol;
}

static void applyCaptureMonitorGain(float g) {
    monLineL.gain(g);
    monLineR.gain(g);
    monMicL.gain(g);
    monMicR.gain(g);
}

static void pollCaptureHostVolume() {
    const float rawVol = AudioOutputUSB::features.volume *
                         (1.0f / (float)FEATURE_MAX_VOLUME);
    const int   rawMute = AudioOutputUSB::features.mute ? 1 : 0;

    const bool valueChanged = (rawVol != s_lastCapVolRaw);
    const bool muteChanged  = (rawMute != s_lastCapMute);

    if (valueChanged) {
        s_lastCapVolRaw = rawVol;
        OSCMessage m("/usb/cap/hostvol/value");
        m.add(rawVol);
        OSCBundle reply;
        reply.add(m);
        g_transport.sendBundle(reply);
    }
    if (muteChanged) {
        s_lastCapMute = rawMute;
        OSCMessage m("/usb/cap/hostvol/mute");
        m.add(rawMute);
        OSCBundle reply;
        reply.add(m);
        g_transport.sendBundle(reply);
    }

    // Push the new gain into all four monitor amps when EITHER value or
    // mute changed — mute is a multiplier so a value-only change still
    // needs the gain re-applied at the current mute state, and vice versa.
    if (valueChanged || muteChanged) {
        applyCaptureMonitorGain(captureMonitorGain(rawVol, rawMute));
    }
}

// ============================================================================
// /snapshot — dump all current state on demand
// ============================================================================
//
// When a dev surface client connects mid-session there's no other way to
// populate its signals — the change-only broadcasts above only fire when
// something moves, and the initial state dump patterns in TDspMixer
// (individual broadcast* helpers) aren't wired together anywhere. This
// function gathers everything a fresh client needs to render the mixer
// correctly and sends it as one bundle.
//
// Triggered by an incoming OSC message at address "/snapshot" (no args).
// The onOscMessage handler intercepts that address before passing to
// g_dispatcher.route() so we don't need to extend OscDispatcher just for
// this sketch-local state (capture-side hostvol).
//
// Contents:
//   * Per-channel: fader, on, solo, name, link (odd channels)
//   * Main bus: faderL, faderR, link, on, hostvol/value
//   * Capture-side hostvol: /usb/cap/hostvol/{value,mute}
//   * Codec panel: whatever Tac5212Panel::snapshot() reads back from
//     the chip (see CodecPanel::snapshot in lib/TDspMixer/) — currently
//     /codec/tac5212/out/N/mode
//
// Meter state is NOT included — clients subscribe to meters separately
// via /sub addSub and the firmware streams them independently.
static void broadcastSnapshot(OSCBundle &reply) {
    // Channel state via the existing dispatcher broadcast helpers.
    for (int n = 1; n <= tdsp::kChannelCount; ++n) {
        g_dispatcher.broadcastChannelFader(n, reply);
        g_dispatcher.broadcastChannelOn(n, reply);
        g_dispatcher.broadcastChannelSolo(n, reply);
        g_dispatcher.broadcastChannelName(n, reply);
        g_dispatcher.broadcastChannelRecSend(n, reply);
    }

    // Main bus state.
    g_dispatcher.broadcastMainFaderL(reply);
    g_dispatcher.broadcastMainFaderR(reply);
    g_dispatcher.broadcastMainLink(reply);
    g_dispatcher.broadcastMainOn(reply);
    g_dispatcher.broadcastMainHostvolValue(reply);
    g_dispatcher.broadcastMainLoop(reply);

    // /main/st/hostvol/enable has no broadcast helper in the dispatcher
    // yet, emit it inline so clients can render the ENABLE button
    // correctly on reconnect.
    {
        OSCMessage m("/main/st/hostvol/enable");
        m.add((int)(g_model.main().hostvolEnable ? 1 : 0));
        reply.add(m);
    }

    // Capture-side hostvol (sketch-local, not in MixerModel). Read
    // straight from AudioOutputUSB::features so the snapshot always
    // reflects whatever Windows most recently pushed, not the cached
    // s_lastCapVolRaw which could be stale between polls.
    {
        const float v = AudioOutputUSB::features.volume *
                        (1.0f / (float)FEATURE_MAX_VOLUME);
        OSCMessage m("/usb/cap/hostvol/value");
        m.add(v);
        reply.add(m);
    }
    {
        OSCMessage m("/usb/cap/hostvol/mute");
        m.add((int)(AudioOutputUSB::features.mute ? 1 : 0));
        reply.add(m);
    }

    // Line input mode (stereo vs mono/differential).
    {
        OSCMessage m("/line/mode");
        m.add(g_lineMonoMode ? "mono" : "stereo");
        reply.add(m);
    }

    // Dexed synth state — volume, MIDI listen channel, and current
    // voice (with name, so UIs don't need a second round-trip to
    // label the dropdown).
    {
        OSCMessage m("/synth/dexed/volume");
        m.add(g_dexedVolume);
        reply.add(m);
    }
    {
        OSCMessage m("/synth/dexed/midi/ch");
        m.add((int)g_dexedSink.listenChannel());
        reply.add(m);
    }
    {
        char name[tdsp::dexed::kVoiceNameBufBytes] = {0};
        tdsp::dexed::copyVoiceName(g_dexedBank, g_dexedVoice, name, sizeof(name));
        OSCMessage m("/synth/dexed/voice");
        m.add(g_dexedBank);
        m.add(g_dexedVoice);
        m.add(name);
        reply.add(m);
    }

    // MPE VA synth state — all 11 OSC addresses echoed so a
    // reconnecting client renders every control correctly without
    // having to round-trip per-address reads.
    {
        OSCMessage m("/synth/mpe/volume");           m.add(g_mpeVolume); reply.add(m);
    }
    {
        OSCMessage m("/synth/mpe/attack");           m.add(g_mpeSink.attack()); reply.add(m);
    }
    {
        OSCMessage m("/synth/mpe/release");          m.add(g_mpeSink.release()); reply.add(m);
    }
    {
        OSCMessage m("/synth/mpe/waveform");         m.add((int)g_mpeSink.waveform()); reply.add(m);
    }
    {
        OSCMessage m("/synth/mpe/filter/cutoff");    m.add(g_mpeSink.filterCutoff()); reply.add(m);
    }
    {
        OSCMessage m("/synth/mpe/filter/resonance"); m.add(g_mpeSink.filterResonance()); reply.add(m);
    }
    {
        OSCMessage m("/synth/mpe/lfo/rate");         m.add(g_mpeSink.lfoRate()); reply.add(m);
    }
    {
        OSCMessage m("/synth/mpe/lfo/depth");        m.add(g_mpeSink.lfoDepth()); reply.add(m);
    }
    {
        OSCMessage m("/synth/mpe/lfo/dest");         m.add((int)g_mpeSink.lfoDest()); reply.add(m);
    }
    {
        OSCMessage m("/synth/mpe/lfo/waveform");     m.add((int)g_mpeSink.lfoWaveform()); reply.add(m);
    }
    {
        OSCMessage m("/synth/mpe/midi/master");      m.add((int)g_mpeSink.masterChannel()); reply.add(m);
    }
    {
        OSCMessage m("/synth/mpe/fx/send");          m.add(g_mpeSendAmount); reply.add(m);
    }

    // Shared FX bus (FX tab + per-synth send).
    {
        OSCMessage m("/synth/dexed/fx/send");
        m.add(g_dexedSendAmt);
        reply.add(m);
    }
    {
        OSCMessage m("/fx/chorus/enable");
        m.add(g_fxChorusEnable ? 1 : 0);
        reply.add(m);
    }
    {
        OSCMessage m("/fx/chorus/voices");
        m.add(g_fxChorusVoices);
        reply.add(m);
    }
    {
        OSCMessage m("/fx/reverb/enable");
        m.add(g_fxReverbEnable ? 1 : 0);
        reply.add(m);
    }
    {
        OSCMessage m("/fx/reverb/size");
        m.add(g_fxReverbRoomSize);
        reply.add(m);
    }
    {
        OSCMessage m("/fx/reverb/damping");
        m.add(g_fxReverbDamping);
        reply.add(m);
    }
    {
        OSCMessage m("/fx/reverb/return");
        m.add(g_fxReverbReturnAmt);
        reply.add(m);
    }

    // Main-bus processing (Processing tab).
    {
        OSCMessage m("/proc/shelf/enable");
        m.add(g_procShelfEnable ? 1 : 0);
        reply.add(m);
    }
    {
        OSCMessage m("/proc/shelf/freq");
        m.add(g_procShelfFreqHz);
        reply.add(m);
    }
    {
        OSCMessage m("/proc/shelf/gain");
        m.add(g_procShelfGainDb);
        reply.add(m);
    }
    {
        OSCMessage m("/proc/limiter/enable");
        m.add(g_procLimiterEnable ? 1 : 0);
        reply.add(m);
    }

    // Codec panel state.
    g_codecPanel.snapshot(reply);
}

// ============================================================================
// loop()
// ============================================================================

void loop() {
    // USB CDC priority model:
    //   1. Audio  — hardware ISR, isochronous endpoint, never disrupted.
    //   2. Control — fader echoes, snapshot → sendBundle() (blocking).
    //      Infrequent, small, must deliver. Triggers a 5ms cooldown
    //      during which streaming writes are suppressed.
    //   3. Streaming — meters, spectrum → broadcastBundle() (non-blocking).
    //      At most ONE frame per loop() tick, with a minimum gap between
    //      writes so CDC bulk transfers don't crowd Audio isochronous
    //      scheduling on the shared USB controller.
    //
    // The serial-bridge sends "unsub_all" when the last WebSocket client
    // disconnects, so the firmware stops computing data nobody is watching.

    pollHostVolume();
    pollCaptureHostVolume();

    // Advance the MPE VA's LFO and push current modulation into held
    // voices. Cheap no-op when LFO dest=OFF; at dest!=OFF this costs
    // ~12 writes per call per held voice. Running every loop() tick
    // keeps modulation smooth (loop() turns over in under 1 ms typ).
    g_mpeSink.tick(millis());

    // USB host: service enumeration + drain all queued MIDI events. Each
    // midi1.read() returns true if it dispatched one event to the installed
    // handler; the while loop keeps draining so a fast-running arpeggio
    // doesn't back up across loop() ticks.
    g_usbHost.Task();
    while (g_midiIn.read()) {}

    g_transport.poll();

    // --- Streaming throttle: one broadcast per tick, minimum gap ---
    static uint32_t s_lastBroadcastMs = 0;
    static constexpr uint32_t BROADCAST_MIN_GAP_MS = 10;
    uint32_t now = millis();

    if (now - s_lastBroadcastMs >= BROADCAST_MIN_GAP_MS) {
        // Always call tick() so engines consume analyzer data and track
        // their internal timing. Send at most one frame per pass.
        // Priority: meters (most useful for mixing) → spectrum → MPE
        // voices. MPE at ~30 Hz is smooth without crowding mix-critical
        // data; the subscription gate turns it off when no UI is
        // watching.
        OSCBundle meterReply;
        bool meterReady = g_meters.tick(meterReply) && meterReply.size() > 0;

        OSCBundle spectrumReply;
        bool spectrumReady = g_spectrum.tick(spectrumReply) && spectrumReply.size() > 0;

        bool mpeVoicesReady = g_mpeVoicesEnabled &&
                              (now - g_mpeLastVoicesMs >= 33);

        if (meterReady) {
            g_transport.broadcastBundle(meterReply);
            s_lastBroadcastMs = now;
        } else if (spectrumReady) {
            g_transport.broadcastBundle(spectrumReply);
            s_lastBroadcastMs = now;
        } else if (mpeVoicesReady) {
            broadcastMpeVoices();
            g_mpeLastVoicesMs = now;
            s_lastBroadcastMs = now;
        }
    }

    // Heartbeat LED — proves the main loop is running and not wedged.
    static unsigned long lastBlink = 0;
    if (millis() - lastBlink >= 500) {
        lastBlink = millis();
        digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
    }
}
