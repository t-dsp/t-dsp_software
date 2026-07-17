// spike_esp32_bt_spdif_mix_kit_f32 — F32 / 24-bit port of spike_esp32_bt_spdif_mix_kit.
// Same sources, same ESP32 control, same songs/instruments — but the mix bus and the DAC
// output are now OpenAudio F32 (float32 in the graph) into 32-bit TDM slots.
//
//   (A) phone --A2DP--> ESP32 (I2S master, 44.1k) --> Teensy SAI2 slave (pin 5)
//         --> AsyncAudioInput<...I2S2_16bitslave> (int16 resampler) --> AudioConvert_I16toF32
//   (B) tone --> S/PDIF OUT (pin 14 optical) --[loopback cable]--> S/PDIF IN (pin 15)
//         --> AsyncAudioInputSPDIF3_F32  (F32-native async resampler)
//   (C) Dexed (int16 FM engine) --> AudioConvert_I16toF32
//   (D) local test tone: AudioSynthWaveformSine_F32
//   mix (A..D) --> AudioMixer4_F32 --> AudioOutputTDM_F32 (SAI1, 32-bit slots)
//              --> TAC5212 DAC (WordLen::Bits32) --> OUT1/OUT2.
//
// Why F32: the int16 build clipped on dense/low Dexed notes — AudioMixer4 (int16)
// hard-saturates the sum, and Dexed's own float->q15 step saturates at +/-1.0. F32 gives
// the mix bus unbounded internal headroom (no mid-graph clip) and hands the codec a
// 24-bit word instead of 16-bit — a wider "hose" into the DAC.
//
// ESP32 control (via the kit):  r=reset->app  g=flash mode  @BOOTAPP@=exit flash
//   U=Teensy program mode.  Audio is paused during flashing via onFlashEnter.
// Needs the pin37->ESP32-EN jumper (see lib/TDspProgrammingKit/README.md).

#include <Arduino.h>
#include <Wire.h>
#include "tdsp_hw_config.h"
#include <Audio.h>
#include <TAC5212.h>
#include <TDspProgrammingKit.h>
#if TDSP_HAS_DIN_MIDI
#include <MIDI.h>
#endif
#if TDSP_HAS_USB_MIDI_HOST
#include <USBHost_t36.h>   // USB host: receive MIDI from a controller (e.g. LinnStrument) via USB
#endif
#include <MidiRouter.h>    // MPE-aware fan-out: bend->semitones, CC74->timbre, pressure->onPressure
#include <SD.h>
#ifdef USB_MTPDISK_SERIAL
#include <MTP_Teensy.h>   // expose the SD card to the host over USB (Serial+MTP)
#endif
#include <TDspSdXfer.h>   // host->SD file push over USB CDC (@WB), fast, no reflash
#include "async_input.h"
#include "input_i2s2_16bit.h"
// OpenAudio F32: the mix bus, int16->F32 converts, F32-native async S/PDIF input,
// F32 sine, F32 peak meters, and the 24-bit AudioOutputTDM_F32 all come from here.
#include <AudioStream_F32.h>
#include <AudioSettings_F32.h>
#include <OpenAudio_ArduinoLibrary.h>
#include "william_tell_mid.h"
#include "moonlight_mid.h"
#include "billie_jean_mid.h"
#include "bohemian_mid.h"
#include "song_event.h"           // baked built-in songs are SongEv[] arrays
#include "test_songs.h"           // built-in MIDI/MPE test sequences (MidiFileEvent[])
// Synth-agnostic MIDI playback (lib/TDspMidiPlayer): the non-blocking player
// fans events into a tdsp::MidiSink. The concrete synth engine (Dexed / ymfm
// OPM) is a build-time choice pulled in below via SynthBackend*.h; nothing in
// this file is engine-specific.
#include <MidiFilePlayer.h>
#include <MidiSmfFile.h>          // runtime SD .mid parser -> MidiFileEvent[]
// Master clock system (lib/TDspTempo): one Conductor owns the BPM + transport;
// the song + drum players follow it via PlayerFollower adapters so a single
// tempo knob retimes both and they share a downbeat. Its 24-PPQN tick is fanned
// through the router (ready for an arp/onClock consumer). ClockSink is the
// external-MIDI-clock seam. See lib/TDspTempo/README.md.
#include <TDspTempo.h>
#include <ClockSink.h>
// Arpeggiator (lib/TDspArp): a MidiSink between the router and the synth. In
// bypass it forwards verbatim; active, it steps held notes on the router's
// 24-PPQN onClock() — which the Conductor's tick hook drives at the master BPM,
// so arp rates lock to the same tempo as the drums + song. See project_arp.
#include <TDspArp.h>
// Beat-aware MIDI loop recorder (lib/TDspMidiLoop): a MidiSink placed DOWNSTREAM
// of the arp so it captures the arp's BAKED note stream; plays the loop back into
// the synth sink directly (bypassing the arp -> no double-arp). One per voice.
// Build-flag gated (TDSP_RECORDER); the app hides the card without it. See
// project_midi_loop_recorder.
#include <TDspMidiLoop.h>
// Audio loop recorder (lib/TDspAudioLoop): stereo AudioStream_F32 nodes that capture
// the master mix into bar-locked, crossfaded audio loops (the audio-domain sibling of
// the MIDI looper). N independent loops, build-flag gated (TDSP_AUDIOLOOP), buffers
// allocated from PSRAM when present else OCRAM. See planning/audio-looper/DESIGN.md.
#include <TDspAudioLoop.h>
#include <AudioLoopWav.h>   // @ALSAVE -> /loops/<name>.wav (pulls <SD.h>, already used)

// Developer bench diagnostics (self-tests, MPE/axis proofs, capture probes, the
// ReplayGain sweep) are opt-in and live in Diagnostics.inc.h. Default ON so every
// existing build behaves EXACTLY as before; a lean product build sets
// -D TDSP_DIAGNOSTICS=0 to compile them out and reclaim flash.
#ifndef TDSP_DIAGNOSTICS
#define TDSP_DIAGNOSTICS 1
#endif

extern "C" uint8_t external_psram_size;   // MB of soldered PSRAM (Teensy core startup)
extern "C" void   *extmem_malloc(size_t size);   // Teensy 4.1 PSRAM heap (EXTMEM) allocator

constexpr int     TAC5212_EN_PIN      = 35;     // shared SHDNZ, active-low
constexpr uint8_t TAC5212_I2C_ADDRESS = 0x51;

// Physical MIDI IN: schematic MIDI_RX = Teensy pin 0 (Serial1 RX) via the H11L1
// opto. Drives the Dexed source below. (See projects/spike_midi_dexed.)
#if TDSP_HAS_DIN_MIDI
MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI);
#endif

// --- Audio graph — F32 mix bus, 24-bit (32-bit slot) TDM out ----------------
// In the OpenAudio F32 world AudioOutputTDM_F32 masters SAI1 and owns
// update_responsibility (see project_f32_update_order), so it is the FIRST audio
// object constructed. No separate AudioInputTDM clock object is needed — the F32
// TDM output drives the SAI1 clock on its own (proven by spike_f32_usb_loopback).
AudioSettings_F32      g_audioSettings(AUDIO_SAMPLE_RATE_EXACT, AUDIO_BLOCK_SAMPLES);
AudioOutputTDM_F32     tdmOut;               // SAI1 TDM (32-bit slots) -> TAC5212 DAC
// Diagnostic ADC capture of the analog loopback (HP OUT1/OUT2 -> IN1/IN2). The codec
// re-digitizes its own DAC output and sends it back on DOUT / SAI1 RX (pin 8); slots
// 0/1 = ADC ch1/ch2 (see setupCodec). tdmOut is still constructed FIRST so it keeps
// update_responsibility (see project_f32_update_order); tdmIn just adds its own RX DMA.
AudioInputTDM_F32      tdmIn;                // SAI1 TDM RX: codec ADC (loopback) -> Teensy

// (A) Bluetooth: the async I2S resampler is int16-only (lib/TDspAsyncI2S has no
// F32 variant), so we bridge its two output channels to F32 immediately with two
// AudioConvert_I16toF32 — "convert as soon as possible".
// Gated by TDSP_ROLE_BT_RECEIVER (default = TDSP_HAS_ESP32_BT). A board with no
// ESP32 sets it 0 to drop the A2DP path — reclaiming the async I2S resampler's
// filter[] (DTCM). The ESP32 control/flash kit is separate and always present.
#if TDSP_ROLE_BT_RECEIVER
AsyncAudioInput<AsyncAudioInputI2S2_16bitslave> btIn(false, false, 100, 20, 80);
AudioConvert_I16toF32  btToF32L, btToF32R;
#endif

// (B) S/PDIF: F32-native async resampler — no int16 anywhere on this path. The
// optical-OUT self-test tone stays int16 (separate SPDIF TX peripheral; it does
// not touch the F32 mix bus). filter[] fits DTCM via the MAX_FILTER_SAMPLES cap.
//
// The async resampler's filter[] lives in DTCM (RAM1) and costs ~87 KB. RAM-tight
// builds (e.g. the 8-engine Dexed pool) drop optical IN with -D TDSP_NO_SPDIF_IN
// to reclaim it; optical OUT self-test + Bluetooth IN are unaffected.
#ifdef TDSP_NO_SPDIF_IN
#define TDSP_SPDIF_IN 0
#else
#define TDSP_SPDIF_IN 1
#endif
#if TDSP_SPDIF_IN
AsyncAudioInputSPDIF3_F32 spdifIn(g_audioSettings, 100, 20, 80);  // optical IN, pin 15
#endif
AudioOutputSPDIF3      spdifOut;                                  // optical OUT, pin 14
AudioSynthWaveformSine spdifTone;                                // int16 tone -> optical

// (C)/(D) local DAC self-test tone (F32). The synth engine itself (slot 3) is
// declared by the build-selected backend header, included after the mixers.
AudioSynthWaveformSine_F32 testTone;         // local DAC self-test source (F32)
tdsp::MidiFilePlayer   g_player;             // non-blocking, synth-agnostic song player
tdsp::MidiFilePlayer   g_drumPlayer;         // dedicated LOOPING drum-groove player (channel 10)

// Live MIDI: a USB-host controller (LinnStrument etc.) + the DIN MIDI IN both feed
// one MPE-aware router that normalizes bend/timbre/pressure into the synth sink.
#if TDSP_HAS_USB_MIDI_HOST
USBHost                g_usbHost;
MIDIDevice             g_usbMidi(g_usbHost);
#endif
tdsp::MidiRouter       g_router;

// --- Voices 2 (build-flag gated) ------------------------------------------------
// Optional second synth voice driven by a SEPARATE MIDI source (the USB-host keyboard),
// on the top half of the Dexed pool (engines 4..7). Only the pool backend defines the
// second sink (g_synthSinkB), so this is a pool-only feature; the app hides the card on
// builds without it (see @STATE "caps"). TDSP_ARP2 adds a second arp on that path.
#ifndef TDSP_VOICE2
#define TDSP_VOICE2 0
#endif
#ifndef TDSP_ARP2
#define TDSP_ARP2 0
#endif
// Beat-aware MIDI loop recorder. Voice-1 recorder is always available on a
// recorder build; the voice-2 recorder additionally needs TDSP_VOICE2 (its
// playback target g_synthSinkB only exists on the split-pool build).
#ifndef TDSP_RECORDER
#define TDSP_RECORDER 0
#endif
// Audio loop recorder (record the master mix as looping audio). Independent of the
// MIDI recorder. Buffers are allocated at runtime from PSRAM (big) or OCRAM (small);
// loops that can't allocate are dropped, so a no-PSRAM board degrades gracefully.
#ifndef TDSP_AUDIOLOOP
#define TDSP_AUDIOLOOP 0
#endif
#ifndef TDSP_AUDIOLOOP_N
#define TDSP_AUDIOLOOP_N 2          // number of independent audio loops (<=3: final mixer slots 1..N)
#endif
#if TDSP_VOICE2
tdsp::MidiRouter       g_kbdRouter;          // USB-host keyboard -> (arp2 ->) g_synthSinkB
tdsp::MidiFilePlayer   g_player2;            // SECOND song player, routed to voice 2 (engines 4..7) so two songs play at once
static bool            g_voice2On = false;   // runtime split enable (@VOICE2=1)
#if TDSP_ARP2
tdsp::ArpFilter        g_arpFilter2;         // optional arp on the keyboard/Voices-2 path
#endif
#endif

// Master clock: THE tempo authority. The song + drum players follow it via
// PlayerFollower adapters (applyTempos() below is the single tempo write path).
// Its internal 24-PPQN tick fans through the router so an arp / onClock() sink
// is a drop-in. g_clockSink lets external MIDI clock (0xF8) slave the kit once
// real-time handlers + Clock::External are enabled. See lib/TDspTempo/README.md.
tdsp::Conductor        g_conductor;
tdsp::PlayerFollower   g_songFollow{g_player};      // g_player / g_drumPlayer are
tdsp::PlayerFollower   g_drumFollow{g_drumPlayer};  // declared above (lines ~92-93)
#if TDSP_VOICE2
tdsp::PlayerFollower   g_songFollow2{g_player2};    // player 2 follows the same master tempo grid
#endif
tdsp::ClockSink        g_clockSink{&g_conductor.clock()};
tdsp::ArpFilter        g_arpFilter;                 // live MIDI -> arp -> synth (bypass by default)
static bool            g_mpeMode = false;    // false = normal MIDI (bend +-2, ch10 drums), true = MPE

// --- MIDI loop recorder (build-flag gated) --------------------------------------
// Each MidiLooper taps its voice's arp downstream (captures the BAKED note stream)
// and plays the loop back into that voice's synth sink. begin() is wired in setup()
// once the sinks exist. g_recVoice picks which voice the app's record controls hit.
#if TDSP_RECORDER
tdsp::MidiLooper       g_loop1;              // voice-1 loop recorder
#if TDSP_VOICE2
tdsp::MidiLooper       g_loop2;              // voice-2 loop recorder (pool split only)
#endif
static uint8_t         g_recVoice = 1;       // 1 or 2: target of @REC/@RECDUB/@RECCLR
static bool            g_recClickAuto = false; // WE turned the count-in click on for a fresh
                                              // record; auto-stop it when the loop is captured
#endif

AudioMixer4_F32        outL, outR;           // F32 mix: 0=BT, 1=local tone, 2=S/PDIF-in, 3=synth
AudioAnalyzePeak_F32   peakSpdif, peakOut;
#if TDSP_ROLE_BT_RECEIVER
AudioAnalyzePeak_F32   peakBt;
#endif

// int16 leg: optical-out tone -> SPDIF TX (self-test loopback source)
AudioConnection     c_txL    (spdifTone,  0, spdifOut, 0);
AudioConnection     c_txR    (spdifTone,  0, spdifOut, 1);
// int16 -> F32 bridges (BT L/R) — the int16 side of the convert blocks
#if TDSP_ROLE_BT_RECEIVER
AudioConnection     c_btcL   (btIn,       0, btToF32L, 0);
AudioConnection     c_btcR   (btIn,       1, btToF32R, 0);
#endif
// F32 mix bus and 24-bit TDM output (synth engine feeds slot 3 from its backend)
#if TDSP_ROLE_BT_RECEIVER
AudioConnection_F32 c_btL    (btToF32L,   0, outL, 0);   // BT -> mix slot 0
AudioConnection_F32 c_btR    (btToF32R,   0, outR, 0);
#endif
AudioConnection_F32 c_toneL  (testTone,   0, outL, 1);
AudioConnection_F32 c_toneR  (testTone,   0, outR, 1);
#if TDSP_SPDIF_IN
AudioConnection_F32 c_spL    (spdifIn,    0, outL, 2);
AudioConnection_F32 c_spR    (spdifIn,    1, outR, 2);
#endif
#if TDSP_AUDIOLOOP
// --- Audio loop recorder: record bus (outL/outR) -> loops -> final mix -> DAC ------
// outL/outR stay the RECORD BUS (everything the user makes); each loop taps it and its
// return sums back in finalL/finalR. Recording the bus (NOT the post-loop mix) means
// overdub can't feed back. The app master fader (tdmOut.setGain) is unchanged — it's
// after the final mix. See planning/audio-looper/DESIGN.md §3.2.
tdsp::AudioLooper   g_aloop[TDSP_AUDIOLOOP_N];
AudioMixer4_F32     finalL, finalR;
AudioConnection_F32 c_finBusL(outL, 0, finalL, 0);        // record bus -> final slot 0
AudioConnection_F32 c_finBusR(outR, 0, finalR, 0);
AudioConnection_F32 c_finOutL(finalL, 0, tdmOut, 0);      // final mix -> DAC
AudioConnection_F32 c_finOutR(finalR, 0, tdmOut, 1);
AudioConnection_F32 c_al0inL (outL, 0, g_aloop[0], 0);    // loop 0 taps the record bus
AudioConnection_F32 c_al0inR (outR, 0, g_aloop[0], 1);
AudioConnection_F32 c_al0rL  (g_aloop[0], 0, finalL, 1);  // loop 0 return -> final slot 1
AudioConnection_F32 c_al0rR  (g_aloop[0], 1, finalR, 1);
#if TDSP_AUDIOLOOP_N >= 2
AudioConnection_F32 c_al1inL (outL, 0, g_aloop[1], 0);
AudioConnection_F32 c_al1inR (outR, 0, g_aloop[1], 1);
AudioConnection_F32 c_al1rL  (g_aloop[1], 0, finalL, 2);
AudioConnection_F32 c_al1rR  (g_aloop[1], 1, finalR, 2);
#endif
#if TDSP_AUDIOLOOP_N >= 3
AudioConnection_F32 c_al2inL (outL, 0, g_aloop[2], 0);
AudioConnection_F32 c_al2inR (outR, 0, g_aloop[2], 1);
AudioConnection_F32 c_al2rL  (g_aloop[2], 0, finalL, 3);
AudioConnection_F32 c_al2rR  (g_aloop[2], 1, finalR, 3);
#endif
static uint8_t  g_aloopSel = 0;                          // selected loop for @AL* commands
static uint8_t  g_aloopN   = 0;                          // loops that actually allocated (runtime)
static int16_t *g_aloopBuf[TDSP_AUDIOLOOP_N]        = { 0 };
static uint32_t g_aloopBufSamples[TDSP_AUDIOLOOP_N] = { 0 };
#else
AudioConnection_F32 c_outL   (outL,       0, tdmOut, 0);
AudioConnection_F32 c_outR   (outR,       0, tdmOut, 1);
#endif
#if TDSP_ROLE_BT_RECEIVER
AudioConnection_F32 c_pkBt   (btToF32L,   0, peakBt,    0);
#endif
#if TDSP_SPDIF_IN
AudioConnection_F32 c_pkSp   (spdifIn,    0, peakSpdif, 0);
#endif
AudioConnection_F32 c_pkOut  (outL,       0, peakOut,   0);

// --- Development output capture (build-agnostic) -----------------------------
// A capture-only probe on the FINAL digital output (same tap as peakOut / the DAC).
// `@CAP[=<n>]` arms it, records the next samples of the actual DAC-bound signal into a
// DMAMEM buffer, and dumps them over USB serial for tools/capture_analyze.py (FFT /
// spectral-centroid "brightness" / RMS / WAV / plot). It taps the output BUS, not any
// engine, so it works on every build now and in the future — a permanent dev instrument.
#ifdef TDSP_LEAN_RAM
DMAMEM static float g_capBuf[64];       // @CAP capture stubbed on lean-RAM builds (frees ~64 KB OCRAM for the SD-song heap)
#else
DMAMEM static float g_capBuf[16384];
#endif
class OutCaptureProbe_F32 : public AudioStream_F32 {
public:
    static const int kCapN = (int)(sizeof(g_capBuf) / sizeof(g_capBuf[0]));   // follows the buffer (tiny on lean-RAM)
    OutCaptureProbe_F32(void) : AudioStream_F32(1, m_inq) {}
    void update(void) override {
        audio_block_f32_t *b = receiveReadOnly_f32(0);
        if (!b) return;
        if (m_arm) for (int i = 0; i < AUDIO_BLOCK_SAMPLES && m_idx < kCapN; i++) {
            g_capBuf[m_idx++] = b->data[i];
            if (m_idx >= kCapN) m_arm = false;
        }
        AudioStream_F32::release(b);
    }
    void arm(void)         { __disable_irq(); m_idx = 0; m_arm = true; __enable_irq(); }
    bool done(void) const  { return !m_arm; }
    int  count(void) const { return m_idx; }
    const float *data(void) const { return g_capBuf; }
private:
    audio_block_f32_t *m_inq[1];
    volatile bool m_arm = false;
    volatile int  m_idx = 0;
};
OutCaptureProbe_F32 g_outCap;
AudioConnection_F32 c_capOut(outL, 0, g_outCap, 0);

// SD-card ready flag — declared before the synth backend so the ymfm backend
// can load its /ymfm/*.opm instrument banks in synthBegin() (set by SD.begin()
// in setup(), which runs before synthBegin() is called).
static bool g_sdReady = false;

// ---- Synth backend: chosen at build time (see platformio.ini) --------------
// Declares the engine, wires it into mix slot 3, exposes g_synthSink + the
// synth* interface. Included HERE so outL/outR already exist for its
// AudioConnection_F32s (same translation unit -> constructed after the mixers).
#if defined(TDSP_SYNTH_SF2_TSF)
  #include "SynthBackendSF2Tsf.h"   // full-fidelity SF2 GM via TinySoundFont (lib/TDspTsf, PSRAM)
#elif defined(TDSP_SYNTH_SF2)
  #include "SynthBackendSF2.h"      // SF2 sampled General MIDI (lib/TDspSF2 + sf22aswt, PSRAM)
#elif defined(TDSP_SYNTH_OPL3)
  #include "SynthBackendOpl3.h"     // OPL3 + DMXOPL GM (needs lib/TDspYmfm OPL3 engine; see spec)
#elif defined(TDSP_SYNTH_OPLL_POOL)
  #include "SynthBackendOpllPool.h" // OPLL YM2413 chip pool — full 3-axis MPE (bend+pressure+timbre)
#elif defined(TDSP_SYNTH_OPLL)
  #include "SynthBackendOpll.h"     // OPLL (YM2413) — the PSS-140 chip: 15 ROM voices + rhythm
#elif defined(TDSP_SYNTH_PLAITS)
  #include "SynthBackendPlaits.h"   // authentic Mutable Plaits macro-oscillator (lib/TDspPlaits2, MIT, MPE)
#elif defined(TDSP_SYNTH_RINGS)
  #include "SynthBackendRings.h"    // Rings-style modal/string resonator (DaisySP, lib/TDspRings, MIT, MPE)
#elif defined(TDSP_SYNTH_VA)
  #include "SynthBackendDaisyVa.h"  // DaisySP virtual-analog: 2 osc -> ladder -> ADSR (lib/TDspDaisyVa, MIT, MPE)
#elif defined(TDSP_SYNTH_YMFM)
  #include "SynthBackendYmfm.h"
#elif defined(TDSP_SYNTH_DEXED_POOL)
  #include "SynthBackendDexedPool.h" // MPE-capable Dexed: pool of engines, one per note
#else
  #include "SynthBackendDexed.h"
#endif

// Optional dedicated GM-drum engine (channel 10) for a MELODIC backend: a second,
// drum-only TSF on the free mix slot 2 so drum grooves can play under Dexed/Plaits/etc.
// Needs PSRAM for the resident font. See DrumTsf.h. (No effect on GM backends, which
// already render ch10 drums themselves.)
// The two parallel drum voices both own mix slot 2 — exactly one at a time.
#if defined(TDSP_DRUM_TSF) && defined(TDSP_DRUM_VOICE)
  #error "TDSP_DRUM_TSF (TinySoundFont) and TDSP_DRUM_VOICE (OPLL) are mutually exclusive — pick one drum voice."
#endif
#ifdef TDSP_DRUM_TSF
  #include "DrumTsf.h"      // sampled full GM kit; needs PSRAM (resident font)
#endif
#ifdef TDSP_DRUM_VOICE
  #include "DrumVoice.h"    // OPLL 5-sound rhythm; ~9 KB, no PSRAM
#endif

#include "CatalogDb.h"   // on-demand /tdsp/ catalog database (@REINDEX); client-triggered
// Parallel drum-voice label for the catalog header (a melodic synth + a dedicated drum engine).
#if defined(TDSP_DRUM_VOICE)
  static const char *kDrumEngineName = "OPLL";
#elif defined(TDSP_DRUM_TSF)
  static const char *kDrumEngineName = "TSF";
#else
  static const char *kDrumEngineName = "";   // GM engines render their own ch10 drums
#endif

// Catalog relevance (compile-time, from the synth build) -> fed into EngineCaps so the app
// fetches only the catalogs this engine can use. hasSoundfonts = SF2/TSF can load an SD .sf2
// as the MAIN synth; hasDexedLibrary = the /dexed DX7 cart browser is meaningful.
#if defined(TDSP_SYNTH_SF2) || defined(TDSP_SYNTH_SF2_TSF)
  static const bool kEngineUsesSoundfonts   = true;
#else
  static const bool kEngineUsesSoundfonts   = false;
#endif
#if defined(TDSP_SYNTH_DEXED) || defined(TDSP_SYNTH_DEXED_POOL)
  static const bool kEngineUsesDexedLibrary = true;
#else
  static const bool kEngineUsesDexedLibrary = false;
#endif

#ifdef TDSP_SYNTH_DEXED_POOL
// Analog-loopback capture probe: taps tdmIn slot 0 = ADC ch1 = the re-digitized OUT1.
// A dedicated capture-only class (not ClipProbe_F32) so its 32 KB buffer can live in
// DMAMEM (RAM2) instead of RAM1 — a second full ClipProbe overflows RAM1.
#ifdef TDSP_LEAN_RAM
DMAMEM static float g_adcCapBuf[64];    // ADC-loopback capture stubbed on lean-RAM builds (frees ~32 KB OCRAM)
#else
DMAMEM static float g_adcCapBuf[8192];
#endif
DMAMEM static float g_adcSnap[256];
class AdcCaptureProbe_F32 : public AudioStream_F32 {
public:
    static const int kCapN = (int)(sizeof(g_adcCapBuf) / sizeof(g_adcCapBuf[0]));
    static const int kPre = 128, kPost = 128, kSnapN = kPre + kPost;
    AdcCaptureProbe_F32(void) : AudioStream_F32(1, inputQueueArray) {}
    void update(void) override {
        audio_block_f32_t *b = receiveReadOnly_f32(0);
        if (!b) return;
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
            float s = b->data[i];
            if (m_arm && m_idx < kCapN) { g_adcCapBuf[m_idx++] = s; if (m_idx >= kCapN) m_arm = false; }
            // Continuous discontinuity watch on the ANALOG loopback: an intermittent
            // codec/DAC pop shows as a step here that has no match in the digital sum.
            if (m_haveHist) {
                float dj = s - m_prev; if (dj < 0) dj = -dj;
                if (dj > m_maxJump) m_maxJump = dj;
                if (dj > m_worstJump && m_snapFill == 0) {
                    m_worstJump = dj;
                    for (int k = 0; k < kPre; k++) g_adcSnap[k] = m_ring[(m_ringHead + k) % kPre];
                    m_snapFill = kPre;
                }
            }
            if (m_snapFill > 0 && m_snapFill < kSnapN) {
                g_adcSnap[m_snapFill++] = s;
                if (m_snapFill >= kSnapN) { m_snapValid = true; m_snapFill = 0; }
            }
            m_ring[m_ringHead] = s; m_ringHead = (m_ringHead + 1) % kPre;
            m_prev = s; m_haveHist = true;
        }
        AudioStream_F32::release(b);
    }
    void         armCapture(void)  { __disable_irq(); m_idx = 0; m_arm = true; __enable_irq(); }
    bool         captureDone(void) const { return !m_arm; }
    int          captureCount(void) const { return m_idx; }
    const float *capture(void)     const { return g_adcCapBuf; }
    float        maxJump(void)   const { return m_maxJump; }
    void         resetPeriod(void)     { m_maxJump = 0.0f; }
    void         resetWorst(void)      { __disable_irq(); m_worstJump = 0.0f; m_snapValid = false; m_snapFill = 0; __enable_irq(); }
    float        worstJump(void) const { return m_worstJump; }
    bool         snapValid(void) const { return m_snapValid; }
    const float *snap(void)      const { return g_adcSnap; }
private:
    audio_block_f32_t *inputQueueArray[1];
    volatile bool m_arm = false;
    volatile int  m_idx = 0;
    float         m_ring[kPre];
    volatile int  m_ringHead = 0, m_snapFill = 0;
    volatile bool m_snapValid = false, m_haveHist = false;
    volatile float m_maxJump = 0.0f, m_worstJump = 0.0f, m_prev = 0.0f;
};
AdcCaptureProbe_F32 adcProbe;
AudioConnection_F32 cAdcCap(tdmIn, 0, adcProbe, 0);

// Slot scanner: tracks peak on ALL 8 TDM input slots so we can find which slot (if
// any) carries the ADC loopback — diagnostic for when slot 0 comes back silent.
class TdmScan_F32 : public AudioStream_F32 {
public:
    TdmScan_F32(void) : AudioStream_F32(8, inputQueueArray) {}
    void update(void) override {
        for (int ch = 0; ch < 8; ch++) {
            audio_block_f32_t *b = receiveReadOnly_f32(ch);
            if (!b) continue;
            for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) { float m = fabsf(b->data[i]); if (m > pk[ch]) pk[ch] = m; }
            AudioStream_F32::release(b);
        }
    }
    void reset(void) { for (int i = 0; i < 8; i++) pk[i] = 0.0f; }
    volatile float pk[8] = {0};
private:
    audio_block_f32_t *inputQueueArray[8];
};
TdmScan_F32 tdmScan;
AudioConnection_F32 csc0(tdmIn,0,tdmScan,0), csc1(tdmIn,1,tdmScan,1), csc2(tdmIn,2,tdmScan,2), csc3(tdmIn,3,tdmScan,3),
                    csc4(tdmIn,4,tdmScan,4), csc5(tdmIn,5,tdmScan,5), csc6(tdmIn,6,tdmScan,6), csc7(tdmIn,7,tdmScan,7);
#endif

tac5212::TAC5212 g_codec(Wire);

// ESP32 control/flash — the reusable kit (EN=37, IO0=36, Serial7). Pins 28/29/36/37
// don't overlap the audio pins, so it coexists with the audio graph.
TDspProgrammingKit kit;
elapsedMillis hb;

static void hardResetCodecPower() {
    pinMode(TAC5212_EN_PIN, OUTPUT);
    digitalWrite(TAC5212_EN_PIN, LOW);  delay(5);
    digitalWrite(TAC5212_EN_PIN, HIGH); delay(10);
}

// I2C bus recovery (see spike_esp32_bt_spdif_mix for the full rationale): bit-bang SCL to
// free a stuck slave before Wire.begin(), so setup() can never hang. Wire0: SDA=18, SCL=19.
static void i2cBusRecover(uint8_t sdaPin = 18, uint8_t sclPin = 19) {
    pinMode(sclPin, INPUT_PULLUP);
    pinMode(sdaPin, INPUT_PULLUP);
    delayMicroseconds(10);
    if (digitalRead(sdaPin) == HIGH) return;
    for (int i = 0; i < 9 && digitalRead(sdaPin) == LOW; ++i) {
        pinMode(sclPin, OUTPUT);
        digitalWrite(sclPin, LOW);  delayMicroseconds(5);
        pinMode(sclPin, INPUT_PULLUP);
        delayMicroseconds(5);
    }
    pinMode(sdaPin, OUTPUT); digitalWrite(sdaPin, LOW); delayMicroseconds(5);
    pinMode(sclPin, INPUT_PULLUP);                delayMicroseconds(5);
    pinMode(sdaPin, INPUT_PULLUP);                delayMicroseconds(5);
}

static bool g_codecOk = false;
static const char *g_codecMsg = "not run";

// --- Two-stage master volume --------------------------------------------------
// (1) CODEC analog level (g_dvol): the TAC5212 DAC output in dB, FIXED per board
//     (headphone vs line calibration, TDSP_DEFAULT_OUT_DVOL_DB). Pushed to the codec
//     once at init via applyVol(); the app does NOT move it. (Diagnostics may nudge
//     it for loopback captures.)
// (2) APP master (g_appMasterPct): a DIGITAL gain at the F32 TDM output
//     (tdmOut.setGain, skipped when unity), driven by the app @VOL and the +/- keys.
//     This is the user-facing master; the codec's analog output stays board-fixed so
//     line-out calibration is preserved.
static float g_dvol = TDSP_DEFAULT_OUT_DVOL_DB;   // codec analog output level (board-fixed)
static void applyVol() {                          // push g_dvol to the TAC5212 DAC
    g_codec.out(1).setDvol(g_dvol);
    g_codec.out(2).setDvol(g_dvol);
}

static int g_appMasterPct = TDSP_DEFAULT_APP_VOL_PCT;   // digital app master, 0..100 %
static float appPctToGain(int pct) {              // 0 = mute, 1..100 -> -60..0 dB -> linear
    if (pct <= 0) return 0.0f;
    if (pct > 100) pct = 100;
    return powf(10.0f, (-60.0f + 0.60f * (float)pct) / 20.0f);
}
static void applyAppMaster() { tdmOut.setGain(appPctToGain(g_appMasterPct)); }

// App master volume from the phone app ("@VOL=<pct>") or the +/- keys. A DIGITAL
// gain, so the codec's analog output stays at its fixed board level. 0=mute, 100=unity.
static void setMasterVolumePct(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    g_appMasterPct = pct;
    applyAppMaster();
    Serial.printf("[vol] app master %d%% (gain %.4f), codec fixed %.1f dB\n",
                  pct, (double)appPctToGain(pct), (double)g_dvol);
}

// TAC5212 DAC highpass filter from the phone app: arrives as "@HPF=<mode>" on the
// ESP32 UART. mode 0 = off (all-pass), 1/2/3 = 1/12/96 Hz cutoff. Chip-global,
// applied to the DAC output (the ADC path is disabled in this firmware). g_hpf keeps
// the current mode so @STATE can hydrate the app's filter control on connect.
static int g_hpf = 0;   // 0=off, 1=1Hz, 2=12Hz, 3=96Hz
static void setDacHpfMode(int mode) {
    if (mode < 0 || mode > 3) mode = 0;
    tac5212::DacHpf hpf;
    switch (mode) {
        case 1:  hpf = tac5212::DacHpf::Cut1Hz;  break;
        case 2:  hpf = tac5212::DacHpf::Cut12Hz; break;
        case 3:  hpf = tac5212::DacHpf::Cut96Hz; break;
        default: hpf = tac5212::DacHpf::Programmable; break;  // 0 / unknown = off
    }
    g_hpf = mode;
    if (g_codecOk) g_codec.setDacHpf(hpf);
    Serial.printf("[hpf] app set DAC HPF mode %d\n", mode);
}

FLASHMEM static void setupCodec() {
    Serial.println("Init TAC5212 (TDM, HP out)...");
    tdspMuxAutoSelectCodec(TAC5212_I2C_ADDRESS);
    tac5212::Result r = g_codec.begin(TAC5212_I2C_ADDRESS);
    g_codecOk = !r.isError();
    g_codecMsg = r.isError() ? (r.message ? r.message : "unknown") : "ok";
    if (r.isError()) { Serial.print("  begin failed: ");
        Serial.println(r.message ? r.message : "(unknown)"); return; }

    tac5212::TAC5212::SerialFormat sf;
    sf.format  = tac5212::TAC5212::Format::Tdm;
    sf.wordLen = tac5212::TAC5212::WordLen::Bits32;   // 32-bit slots for AudioOutputTDM_F32 (was Bits16)
    g_codec.setSerialFormat(sf);
#ifdef TDSP_DIGITAL_AUDIO_BOARD
    // The t-dsp_digital_audio_board mis-wires DOUT (a buffer contends on the TDM data
    // line), so its codec DOUT must be forced off. The t-dsp_tac5212_audio_adaptor_shield
    // is wired correctly and leaves setSerialFormat's DOUT routing intact (needed for the
    // ADC loopback capture). Board switch: define TDSP_DIGITAL_AUDIO_BOARD for the former.
    g_codec.writeRegister(0, /*INTF_CFG1*/ 0x10, 0x00);   // board bodge: disable codec DOUT
#endif
    g_codec.setRxSlotOffset(1);
    g_codec.setRxChannelSlot(1, 0);
    g_codec.setRxChannelSlot(2, 1);
    // Output driver mode from the board profile (TDSP_OUT_TYPE).
#if TDSP_OUT_TYPE == TDSP_OUT_LINE
    const tac5212::OutMode kOutMode = tac5212::OutMode::SeLine;      // single-ended line
#elif TDSP_OUT_TYPE == TDSP_OUT_BALANCED
    const tac5212::OutMode kOutMode = tac5212::OutMode::DiffLine;    // differential / balanced line
#else
    const tac5212::OutMode kOutMode = tac5212::OutMode::HpDriver;    // headphone
#endif
    g_codec.out(1).setMode(kOutMode);
    g_codec.out(2).setMode(kOutMode);
    g_codec.out(1).setDvol(-128.0f);
    g_codec.out(2).setDvol(-128.0f);
    // --- ADC capture of the analog loopback (OUT1/OUT2 -> IN1/IN2) ---------------
    // IN1/IN2 as single-ended line inputs (INxP), DC-low coupling for headroom; the
    // codec re-digitizes its DAC output and transmits ADC ch1/ch2 on TDM TX slots 0/1
    // (DOUT / SAI1 RX pin 8), where AudioInputTDM_F32 tdmIn reads them.
    g_codec.adc(1).setMode(tac5212::AdcMode::SingleEndedInp);
    g_codec.adc(2).setMode(tac5212::AdcMode::SingleEndedInp);
    g_codec.adc(1).setCoupling(tac5212::AdcCoupling::DcLow);
    g_codec.adc(2).setCoupling(tac5212::AdcCoupling::DcLow);
    g_codec.adc(1).setFullscale(tac5212::AdcFullscale::V2rms);
    g_codec.adc(2).setFullscale(tac5212::AdcFullscale::V2rms);
    g_codec.adc(1).setDvol(0.0f);
    g_codec.adc(2).setDvol(0.0f);
    g_codec.setTxChannelSlot(1, 0);   // ADC ch1 -> TDM slot 0 (loopback of OUT1)
    g_codec.setTxChannelSlot(2, 1);   // ADC ch2 -> TDM slot 1 (loopback of OUT2)
    g_codec.setTxSlotOffset(1);       // mirror the RX slot offset
    g_codec.setChannelEnable(/*inMask=*/0xC, /*outMask=*/0xC);   // IN1/IN2 + OUT1/OUT2 (CH1/CH2 = top bits of each nibble)
    g_codec.powerAdc(true);
    g_codec.powerDac(true);
    delay(100);
    g_codec.setDspAvddSelect(true);
    setDacHpfMode(g_hpf);   // enforce the tracked HPF mode (boot: 0/off) so @STATE matches the chip; a re-init restores the current mode
}

// mixer helper: 0=BT, 1=local test tone, 2=S/PDIF-in (slot 3 = Dexed, set once,
// stays on independently of source-mode switches)
static void setMix(float bt, float tone, float spdif) {
#if TDSP_ROLE_BT_RECEIVER
    outL.gain(0, bt);    outR.gain(0, bt);
#else
    (void)bt;   // no BT receiver: mix slot 0 has no source
#endif
    outL.gain(1, tone);  outR.gain(1, tone);
#if !defined(TDSP_DRUM_TSF) && !defined(TDSP_DRUM_VOICE)
    outL.gain(2, spdif); outR.gain(2, spdif);
#else
    (void)spdif;   // slot 2 is the drum voice's bus in this build (drum*Begin owns its gain)
#endif
}

// ---------------------------------------------------------------------------
// Metronome / idle time signature (beats per bar). Set by @METROSIG; drives the
// metronome accent AND applyMeter()'s idle downbeat fallback. NOT gated on
// TDSP_METRONOME because applyMeter() references it unconditionally.
static uint8_t g_metroBpb = 4;

// Emit "@BEAT=<i>/<n>" for the app's beat lights — but ONLY if the USB serial can
// take it WITHOUT blocking loop(). Click/playback timing precision matters more than
// a single light frame, and the app has a local-clock fallback, so if the TX buffer
// is backed up we simply drop this frame instead of stalling the foreground (which
// would jitter the metronome click / the synced players). This is the fix for
// "the beat-light feed hurting precision".
static void emitBeat(uint8_t i, uint8_t n) {
    if (Serial.availableForWrite() >= 16) Serial.printf("@BEAT=%u/%u\n", (unsigned)i, (unsigned)n);
}

// Metronome (opt-in: -D TDSP_METRONOME) — a SELF-TIMED on-beat click, deliberately
// Keeps its OWN micros-based beat schedule + bar counter once running, so it is immune to
// the content meter and the clock's catch-up ticks — and stays precise no matter what else
// runs. It takes the TEMPO from the Conductor (g_conductor.bpm()), and on ENABLE it takes the
// PHASE too when the transport is already running (see metroSetEnabled): the click joins the
// grid rather than declaring a new downbeat, so starting it never moves the bar. Only from an
// idle transport does the first click fire immediately (= this Play defines the downbeat).
// The accent lands on beat 1 of its own g_metroBpb bar.
// It emits its own @BEAT (non-blocking) so the app's amber downbeat light counts
// forward with it. Slot-free: reuses the local test-tone oscillator (testTone, mix
// slot 1). v1 clicks the quarter-note beat; compound "in-2" pulses are a phase-2 item.
#ifdef TDSP_METRONOME
static bool          g_metroOn      = false;
static uint8_t       g_metroBeatIdx = 0;     // beat within the metronome's OWN bar (0 = downbeat)
static uint32_t      g_metroNextUs  = 0;     // micros() when the next click is due
static float         g_metroPeak    = 0.0f;  // current click's peak amplitude (0 = idle)
static elapsedMillis g_metroAge;             // ms since the current click fired
static constexpr float    kMetroGain      = 0.9f;    // mix-slot-1 gain while enabled
static constexpr float    kMetroAccentAmp = 0.85f;   // beat 1 (downbeat) click level
static constexpr float    kMetroBeatAmp   = 0.30f;   // other beats (well below the accent)
static constexpr float    kMetroAccentHz  = 2093.0f; // C7 — accent pitch (an octave over the beat)
static constexpr float    kMetroBeatHz    = 1047.0f; // C6 — normal pitch
static constexpr float    kMetroDecayMs   = 45.0f;   // percussive click decay
static int           g_metroVolPct = 100;  // click level 0..150 (% of the default gain), independent of @VOL master
static float metroSlotGain() { return kMetroGain * (g_metroVolPct / 100.0f); }   // slot-1 gain scaled by the volume

// micros per quarter-note beat at the MASTER tempo (tempo only — not the grid phase).
static uint32_t metroBeatUs() {
    float bpm = g_conductor.bpm(); if (bpm < 1.0f) bpm = 1.0f;
    return (uint32_t)(60000000.0f / bpm);
}

static void metroSetEnabled(bool on) {
    g_metroOn = on;
    if (on) {
        // Starting the click must never MOVE the downbeat. When the transport is already running,
        // JOIN its grid in phase: schedule the next click on the clock's next beat edge and
        // continue the real bar count, so the accent stays on the true downbeat and the @BEAT
        // lights don't jump. This matters because arming a loop recorder auto-starts the click:
        // declaring a fresh beat 1 here made the bar appear to restart, and worse, left the click
        // disagreeing with latchAnchor() (which anchors to the Conductor's bar) — so a note played
        // "on the click's one" got recorded against a different downbeat.
        // Only when the transport is IDLE does Play define a fresh downbeat.
        const uint8_t bpb = g_metroBpb ? g_metroBpb : 1;
        tdsp::Clock &clk = g_conductor.clock();
        if (clk.running()) {
            const double  pos  = clk.positionBeats();
            const double  frac = pos - floor(pos);                 // how far into the current beat
            const int64_t nb   = (int64_t)floor(pos) + 1;          // the beat the next click lands on
            g_metroBeatIdx = (uint8_t)(((nb % bpb) + bpb) % bpb);  // continue the bar as it stands
            g_metroNextUs  = micros() + (uint32_t)((1.0 - frac) * (double)metroBeatUs());
        } else {
            g_metroBeatIdx = 0;             // next click is beat 1 (the downbeat)...
            g_metroNextUs  = micros();      // ...and it fires immediately on Play
        }
        outL.gain(1, metroSlotGain()); outR.gain(1, metroSlotGain());   // open slot 1 for the click
    } else {
        g_metroPeak = 0.0f;
        testTone.amplitude(0.0f);
        outL.gain(1, 0.0f); outR.gain(1, 0.0f);               // silence slot 1 again
    }
}

// Call once per loop(). Self-timed off micros() at the master tempo — independent
// of the shared clock's grid, so serial/loop jitter can't drag it off the beat.
static void metroPoll() {
    if (g_metroOn) {
        const uint32_t now = micros();
        if ((int32_t)(now - g_metroNextUs) >= 0) {            // a beat is due
            const uint8_t bpb = g_metroBpb ? g_metroBpb : 1;
            const bool accent = (g_metroBeatIdx == 0);        // beat 1 of the metronome's own bar
            testTone.frequency(accent ? kMetroAccentHz : kMetroBeatHz);
            g_metroPeak = accent ? kMetroAccentAmp : kMetroBeatAmp;
            g_metroAge  = 0;
            emitBeat(g_metroBeatIdx, bpb);                    // drive the app lights (non-blocking)
            if (++g_metroBeatIdx >= bpb) g_metroBeatIdx = 0;
            const uint32_t mpb = metroBeatUs();
            g_metroNextUs += mpb;                             // schedule from the accumulator (drift-free)
            if ((int32_t)(now - g_metroNextUs) >= 0)          // fell >1 beat behind (a stall) -> resync, don't burst
                g_metroNextUs = now + mpb;
        }
    }
    if (g_metroPeak > 0.0f) {                                 // percussive linear decay
        float a = g_metroPeak * (1.0f - (float)g_metroAge / kMetroDecayMs);
        if (a <= 0.001f) { a = 0.0f; g_metroPeak = 0.0f; }
        testTone.amplitude(a);
    }
}

// Metronome click level (0..150 %). Scales slot-1 gain; applied live when running.
static void setMetroVol(int pct) {
    if (pct < 0) pct = 0; if (pct > 150) pct = 150;
    g_metroVolPct = pct;
    if (g_metroOn) { outL.gain(1, metroSlotGain()); outR.gain(1, metroSlotGain()); }
}
#endif  // TDSP_METRONOME

// --- Beat position emit (@BEAT) ---------------------------------------------
// Drives the app's visual beat lights off the REAL master clock whenever the
// metronome is OFF. While a song/groove plays it reflects the content's grid (real
// downbeat + meter); while idle it free-runs at the master tempo and the @METROSIG
// time signature (clk.beatsPerBar() falls back to g_metroBpb via applyMeter), so the
// lights ALWAYS show where the system thinks the beat is — even stopped. A running
// metronome emits its OWN @BEAT from metroPoll() (self-timed), so we skip here when
// it's on to avoid two sources fighting over the lights. Watches beatCount() change
// (does NOT consume the beat latch) and emits non-blocking via emitBeat(), so the
// light feed can never stall loop().
static uint32_t g_lastBeatEmit = 0xFFFFFFFFu;   // force an emit on the first beat seen
static void beatEmitPoll() {
#ifdef TDSP_METRONOME
    if (g_metroOn) return;                       // the metronome owns the lights while running
#endif
    tdsp::Clock &clk = g_conductor.clock();
    if (!clk.running()) return;                  // clock stalled/stopped -> nothing to show
    const uint32_t bc = clk.beatCount();
    if (bc == g_lastBeatEmit) return;            // still within the same beat
    g_lastBeatEmit = bc;
    uint8_t bpb = clk.beatsPerBar(); if (!bpb) bpb = 1;
    emitBeat(clk.beatInBar(), bpb);
}

// --- Non-blocking song sequencer --------------------------------------------
// Song registry: index (sent by the app as @SONG=<i>) -> a transcoded MIDI
// stream. Keep in sync with DX_SONGS[] in the app (tdspBle.ts). The player is
// non-blocking (ticked every loop()) so BT audio, the ESP32 relay, and app
// control keep running and the app can stop/switch it mid-song.
// Built-in songs baked into flash (always available, even with no SD card).
// `bpm` is an ESTIMATE — these were transcoded to raw milliseconds, so their true
// tempo is lost. It's the reference the master-BPM tempo scale divides by, so a
// drum groove can lock to the song. For accurate lock use an SD .mid (real tempo).
struct BuiltinSong { const char *name; const SongEv *ev; uint32_t count; float bpm; };
static const BuiltinSong kBuiltinSongs[] = {
    {"William Tell Overture",      kWilliamTellSong, sizeof(kWilliamTellSong) / sizeof(SongEv), 152.0f},
    {"Moonlight Sonata (3rd Mvt)", kMoonlightSong,   sizeof(kMoonlightSong)   / sizeof(SongEv), 120.0f},
    {"Billie Jean",                kBillieJeanSong,  sizeof(kBillieJeanSong)  / sizeof(SongEv), 117.0f},
    {"Bohemian Rhapsody",          kBohemianSong,    sizeof(kBohemianSong)    / sizeof(SongEv),  72.0f},
};
static const int kNumBuiltin = sizeof(kBuiltinSongs) / sizeof(kBuiltinSongs[0]);

// Songs live on the SD card (/songs/*.mid) plus a handful of baked demo/test
// sequences in flash. Nothing but the ONE currently-playing song is held in RAM
// (parsed into g_buf on play) — exactly like the drum grooves. The browsable list is
// the catalog (/tdsp/songs.ndjson, built by @REINDEX); the app plays a song by NAME
// via @SONGF (an SD filename, or a built-in's display name). There is NO fixed-size
// registry and therefore NO song-count cap.
//   built-in test seq : baked MidiFileEvent[] (mev), flips MPE mode via `mpe`
//   built-in demo     : baked legacy SongEv[] (ev), tempo estimate
//   SD song           : /songs/<name>.mid, parsed on play (real tempo)
// g_sdReady is declared earlier (before the synth backend include).

static const int MAX_EVENTS = 24000;                 // longest playable song (baked or SD)
DMAMEM static tdsp::MidiFileEvent g_buf[MAX_EVENTS];  // ~144KB in OCRAM (off the DTCM budget)
#if TDSP_VOICE2
// Player 2 needs its OWN event buffer — MidiFilePlayer::play() holds a pointer (does not copy),
// so it can't share g_buf with player 1. Half-size (~72KB) to keep OCRAM in budget on the
// no-PSRAM pool build; a second simultaneous song is typically a shorter backing/loop. A song
// longer than this is truncated on player 2 (player 1 still gets the full 24000-event buffer).
static const int MAX_EVENTS2 = 12000;
DMAMEM static tdsp::MidiFileEvent g_buf2[MAX_EVENTS2];
#endif

static bool endsWithMid(const char *s) {
    size_t n = strlen(s);
    return n > 4 && strcasecmp(s + n - 4, ".mid") == 0;
}
// strip a trailing ".mid" from a filename into `out` (the display name)
static void songDisp(char *out, size_t n, const char *fname) {
    size_t c = strlen(fname);
    if (c > 4 && strcasecmp(fname + c - 4, ".mid") == 0) c -= 4;
    if (c > n - 1) c = n - 1;
    memcpy(out, fname, c); out[c] = 0;
}
// True if an SD .mid with this display name exists (so a baked built-in defers to the
// tempo-bearing SD copy). Checked against the card, not a RAM list.
static bool sdSongExists(const char *disp) {
    if (!::g_sdReady) return false;
    char p[128];
    snprintf(p, sizeof p, "/songs/%s.mid", disp); if (SD.exists(p)) return true;
    snprintf(p, sizeof p, "/%s.mid", disp);        return SD.exists(p);
}
// Extract a JSON string field ("<key>":"<val>") from one songs.ndjson line into `out`.
// Minimal (handles a leading backslash-escape); song names/filenames carry no exotic
// escaping in practice. Returns false if the key is absent.
static bool jsonStrField(const char *line, const char *key, char *out, size_t n) {
    char pat[24]; snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *s = strstr(line, pat); if (!s) return false;
    s += strlen(pat);
    size_t i = 0;
    while (*s && *s != '"' && i < n - 1) { if (*s == '\\' && s[1]) ++s; out[i++] = *s++; }
    out[i] = 0; return true;
}
// Number of rows in /tdsp/songs.ndjson (one song per line). 0 if no catalog.
static int songCatalogCount() {
    File f = SD.open("/tdsp/songs.ndjson"); if (!f) return 0;
    int n = 0; while (f.available()) if (f.read() == '\n') ++n;
    f.close(); return n;
}
// Read the idx-th song from the catalog: `argOut` = the @SONGF play arg (the `file`
// field for SD songs, else the `name` for built-ins), `nameOut` = the display name.
// Either out-pointer may be null. Returns false if out of range / no catalog.
static bool songByIndex(int idx, char *argOut, size_t argN, char *nameOut, size_t nameN) {
    File f = SD.open("/tdsp/songs.ndjson"); if (!f) return false;
    char line[224]; int i = 0; bool ok = false;
    while (f.available()) {
        int len = f.readBytesUntil('\n', line, sizeof(line) - 1); line[len] = 0;
        if (len <= 0) continue;
        if (i == idx) {
            char file[128];
            if (argOut) {
                if (jsonStrField(line, "file", file, sizeof file)) snprintf(argOut, argN, "%s", file);
                else { char nm[120]; jsonStrField(line, "name", nm, sizeof nm); snprintf(argOut, argN, "%s", nm); }
            }
            if (nameOut) jsonStrField(line, "name", nameOut, nameN);
            ok = true; break;
        }
        ++i;
    }
    f.close(); return ok;
}
// Write one directory's *.mid rows to songs.ndjson ({name, file}). Used by the catalog
// builder (catdbWriteBundled) — a direct SD scan, so the catalog is uncapped.
static void writeSongDir(Print &so, const char *dir) {
    File d = SD.open(dir);
    if (!d || !d.isDirectory()) { if (d) d.close(); return; }
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
        const char *nm = f.name();
        if (!f.isDirectory() && nm && endsWithMid(nm)) {
            char disp[64]; songDisp(disp, sizeof disp, nm);
            so.print("{\"name\":"); tdsp::catdb::jsonStr(so, disp);
            so.print(",\"file\":"); tdsp::catdb::jsonStr(so, nm); so.print("}\n");
        }
        f.close();
    }
    d.close();
}

// Current/last song — a single slot, not a capped array. g_curSongArg is the @SONGF
// arg (SD filename or built-in name) replayed to re-arm on loop; g_curSongName is the
// display name for @STATE/logs. Songs live on SD + songs.ndjson; only the one playing
// song is in RAM (g_buf), like the drum grooves.
static char g_curSongName[64] = "";
static char g_curSongArg[100] = "";
static int  g_songBrowse = 0;       // dev-key ('S') browse cursor into songs.ndjson
static bool g_loop = false;         // when set, a song restarts itself when it ends
static bool g_songWasPlaying = false;  // edge-detect natural song end (for loop) in loop()
#if TDSP_VOICE2
// Player 2's own copies of the per-song state (mirrors the voice-1 slot above), so the second
// song player is fully independent: its own name/arg/loop/tempo/meter/sync length.
static char   g_curSong2Name[64] = "";
static char   g_curSong2Arg[100] = "";
static bool   g_song2Loop = false;   // NB: g_loop2 (a MidiLooper) is the recorder's — this is the player-2 song-loop flag
static bool   g_song2WasPlaying = false;
static float  g_song2Bpm = 120.0f;      // player-2 song native tempo (retimed to master by applyTempos)
static uint8_t g_song2Bpb = 4;
static double g_song2LoopBeats = 0.0;
#endif
static bool          g_syncProbe = false;   // @SYNCPROBE: 1 Hz drift probe (PLAN §9)
static elapsedMillis g_syncProbeClock;      // throttle for the probe print

// Generic app-owned state blob. The app has settings the firmware never interprets or acts
// on (e.g. the MIDI player's end-of-song mode: shuffle/continue/stop) but which must survive
// an app reload/reconnect. Rather than a per-setting command + @STATE field for each, the app
// serializes them into ONE opaque payload (compact JSON, its own schema) and we just store +
// echo it. RAM cost is a single fixed buffer; the app-only state is tiny, so 256 bytes is
// plenty and stays under the 288-byte USB/BLE line buffers once the "@APP=" prefix is added.
// This is RAM-only: it survives an app reload while the box stays powered, not a reboot.
static char g_appState[256] = "";

// Load the selected song into g_buf and hand it to the player. Baked built-ins
// expand from their legacy SongEv[] (channel 0); SD songs parse straight to
// MidiFileEvent[] (full channel/program/velocity). The player is non-blocking
// (g_player.tick() in loop) and drives the synth via g_synthSink.
static void applyMidiMode(bool mpe);   // defined below; test songs flip mode on start

// Drum controls. g_drumSel / g_drumKit are used by the drum section further below.
static int  g_drumSel      = 0;     // selected / currently-playing groove index
static int  g_drumKit      = 0;     // index into kDrumKits ("instrument")
static int  g_drumVolPct   = 100;   // drum level 0..150 (% of file velocity)
static int  g_songVolPct   = 100;   // MIDI-player level 0..150 (% of file velocity), independent of @VOL master
static bool g_drumSynchro  = false; // SYNCHRO START (PSS-140 style): groove starts on your first note
static bool g_engineHasDrums = false;// engine renders ch10 (captured once at setup; not the live mask)

// LAUNCH QUANTIZE (opt-in, default off): defer a song/groove START to the next bar edge of
// the free-running master clock — the SAME grid the arp already steps on — so songs, grooves
// and the arp all share one downbeat and stay locked. A launch only ever waits ≤ 1 bar. The
// per-loop restart of a looping song is NOT quantized (it re-arms via songStartArg directly),
// so a loop never grows a bar-long gap. This aligns to the free grid without re-zeroing it.
static bool g_launchQuantize   = false;
static bool g_songLaunchPending = false;
static char g_pendingSongArg[64] = {0};
static bool g_drumLaunchPending = false;
static char g_pendingDrumFile[80] = {0};
// A tiny tempo follower that just flags each bar edge so loop() can fire pending launches
// OUTSIDE the conductor's follower fan-out (keeps the heavy SD-load start off the callback).
struct LaunchScheduler : tdsp::ITempoFollower { volatile bool barHit = false; void onBarEdge() override { barHit = true; } };
static LaunchScheduler g_launchSched;

// --- Master tempo (BPM) — one knob drives the song AND the drum groove -------
// The song and the groove each have a NATIVE tempo; the master BPM retimes both
// to a single tempo so they stay locked, and moving it speeds/slows both together:
//   song scale = masterBpm / songNativeBpm     (songNativeBpm: SD = real, built-in = estimate)
//   drum scale = masterBpm / grooveNativeBpm
// Downbeat align: whichever starts SECOND begins on the other's bar/loop downbeat
// (both bars are 4/4 = 4*60000/masterBpm long once retimed, so they line up).
// NOTE accurate lock needs the song's REAL tempo -> use an SD .mid; the baked
// built-ins only carry an estimate.
static float         g_masterBpm     = TDSP_DEFAULT_BPM;  // the one tempo knob (40..240), board-configurable
static float         g_songBpm       = 120.0f;  // playing/last song NATIVE tempo
static float         g_drumFileBpm   = 120.0f;  // selected groove's NATIVE tempo
static uint8_t       g_songBpb       = 4;       // playing/last song's beats-per-bar (quarter beats)
static uint8_t       g_drumBpb       = 4;       // selected groove's beats-per-bar
static double        g_songLoopBeats = 0.0;     // playing/last song's exact loop length (quarter beats); 0 = unknown
static double        g_drumLoopBeats = 0.0;     // selected groove's exact loop length (quarter beats); 0 = unknown
static elapsedMillis g_songBarClock;            // ms since the playing song's beat 1
static bool          g_drumArmed     = false;   // SYNCHRO: groove loaded, waiting for the first live note
static uint32_t      g_drumArmedN    = 0;

// Retime both players to the master BPM (call after changing BPM / native tempos).
// This is the SINGLE tempo write path: feed the followers their native tempos,
// then let the Conductor push the master BPM out to both (and to any future
// follower — arp, LFO). The Conductor also keeps its Clock at the master BPM so
// a tick consumer stays locked to the same grid. The drum groove follows the
// master BPM exactly — there is no separate drum-speed trim (one tempo, one knob).
static void applyTempos() {
    g_songFollow.setNativeBpm(g_songBpm);
    g_drumFollow.setNativeBpm(g_drumFileBpm);
#if TDSP_VOICE2
    g_songFollow2.setNativeBpm(g_song2Bpm);   // player 2 retimes to the same master BPM
#endif
    g_conductor.setBpm(g_masterBpm);
}

// Set the master bar length from the CONTENT's time signature so the downbeat
// (beatInBar()==0 / barPhase()==0) and consumeBarEdge() land on real bar 1 for
// non-4/4 material — not just common time. The song is the meter master while one
// plays (song = master, same rule as the tempo lock); otherwise a looping groove
// owns the meter; idle falls back to 4/4. Call after any song/groove start or stop.
static void applyMeter() {
    // Content owns the meter while it plays (song = master, else a looping
    // groove); idle falls back to the METRONOME time signature (g_metroBpb,
    // default 4/4) so a standalone click bars-up on the chosen signature.
    uint8_t bpb = g_player.isPlaying()     ? g_songBpb
                : g_drumPlayer.isPlaying() ? g_drumBpb
                                           : g_metroBpb;
    g_conductor.setBeatsPerBar(bpb);
}

// When set, the next ensureTransportStarted() re-zeroes the grid even if something is
// already playing — a Play / ‹ › press that hard-restarts the song on a fresh downbeat.
static bool g_forceTransportZero = false;

// Zero the master transport (downbeat = now) when nothing is already playing — the FIRST
// player to start defines the grid's zero point; every later player JOINS the running grid
// in phase (a synced player anchors itself via fmod(now, loopBeats) in setSyncedMode). Never
// yank the grid out from under an already-locked song/groove/arp — UNLESS g_forceTransportZero
// is set, i.e. the user explicitly asked to restart the song from the top (see songRestart).
// PLAN §5. Call this BEFORE the new player's play()/setSyncedMode(), so the anchor reads the
// (possibly re-zeroed) clock.
#if TDSP_RECORDER
// A looper mid-capture or looping OWNS the grid just as much as a song does: its clip is
// anchored to an absolute beat, so re-zeroing the clock under it restarts/jumps its loop.
static inline bool loopHoldsGrid(const tdsp::MidiLooper &L) {
    const tdsp::MidiLooper::State st = L.state();
    return st == tdsp::MidiLooper::Recording || st == tdsp::MidiLooper::Overdub || st == tdsp::MidiLooper::Playing;
}
#endif
static void ensureTransportStarted() {
    bool anyPlaying = g_player.isPlaying() || g_drumPlayer.isPlaying();
#if TDSP_VOICE2
    anyPlaying = anyPlaying || g_player2.isPlaying();   // player 2 also holds the grid
#endif
#if TDSP_RECORDER
    // ...and so does a running loop. Without this, arming synth B's recorder while only synth
    // A's loop is going looks "idle" here and re-zeroes the clock — restarting A's loop. Each
    // recorder anchors to the bar of its own first note, so the two loops may sit out of phase
    // with each other; that's fine and intended. They still share the one bar grid.
    anyPlaying = anyPlaying || loopHoldsGrid(g_loop1);
#if TDSP_VOICE2
    anyPlaying = anyPlaying || loopHoldsGrid(g_loop2);
#endif
#endif
    if (g_forceTransportZero || !anyPlaying) {
        g_conductor.start();
        g_arpFilter.resyncToGrid();   // a chord held on the arp re-locks to the new downbeat
    }
    g_forceTransportZero = false;
}

#if TDSP_RECORDER
// The looper the app's record controls currently target (voice 1 or 2).
static tdsp::MidiLooper *recSel() {
#if TDSP_VOICE2
    if (g_recVoice == 2) return &g_loop2;
#endif
    return &g_loop1;
}
// True while a looper is still arming/capturing a FRESH take (the count-in click should
// run during this, then stop). Overdub plays the existing loop as its own reference.
static bool recFreshCapturing() {
    auto arming = [](tdsp::MidiLooper &l) {
        return l.state() == tdsp::MidiLooper::Armed || l.state() == tdsp::MidiLooper::Recording;
    };
    bool a = arming(g_loop1);
#if TDSP_VOICE2
    a = a || arming(g_loop2);
#endif
    return a;
}

// Arming a recording needs a running beat grid to anchor to; when nothing else is playing,
// also strike the metronome as a count-in so the player has a click to play against
// (recording still begins on the first note press). The click is auto-stopped once the loop
// is captured (recPollClick, below) so it never bleeds into playback. Overdub/resume pass
// startClick=false — the already-looping clip is the reference. See project_midi_loop_recorder.
static void recArmTransport(bool startClick) {
    applyMeter();                 // bars-up the clock on the current (record) signature
    ensureTransportStarted();     // define the downbeat if the transport is idle
#ifdef TDSP_METRONOME
    if (startClick && !g_metroOn && !g_player.isPlaying() && !g_drumPlayer.isPlaying()) {
        metroSetEnabled(true);
        g_recClickAuto = true;    // remember WE started it, so we may auto-stop it
    }
#endif
    (void)startClick;
}

// Auto-stop the count-in click the instant the loop finishes recording (state -> Playing),
// but only if we started it — never kill a click the user turned on themselves. Call from loop().
static void recPollClick() {
#ifdef TDSP_METRONOME
    if (g_recClickAuto && !recFreshCapturing()) {
        metroSetEnabled(false);
        g_recClickAuto = false;
    }
#endif
}
#endif

#if TDSP_AUDIOLOOP
// The audio loop the @AL* commands currently target.
static tdsp::AudioLooper *alSel() { return &g_aloop[g_aloopSel < g_aloopN ? g_aloopSel : 0]; }

// (Re)init loop i for mono/stereo. Mono stores 1 int16/frame, so the same buffer holds
// 2x the frames — set the capacity accordingly. Preserves bars/level/follow (separate setters).
static void aloopInit(uint8_t i, bool mono) {
    if (i >= TDSP_AUDIOLOOP_N || !g_aloopBuf[i]) return;
    g_aloop[i].clear();
    g_aloop[i].setMono(mono);
    const uint32_t bs = g_aloopBufSamples[i];
    g_aloop[i].begin(&g_conductor.clock(), g_aloopBuf[i], mono ? bs : bs / 2);
}

// Allocate loop buffers once at boot: PSRAM (generous) when present, else OCRAM (small).
// Stop at the first allocation failure so a low-RAM board simply gets fewer loops
// (g_aloopN), and the app hides slots it doesn't have (caps.audioloop = g_aloopN).
FLASHMEM static void audioLoopSetup() {
    finalL.gain(0, 1.0f); finalR.gain(0, 1.0f);                 // record bus at unity
    for (uint8_t s = 1; s <= 3; s++) { finalL.gain(s, 1.0f); finalR.gain(s, 1.0f); }
    const uint32_t psramSamples = 2u * 8u * (uint32_t)AUDIO_SAMPLE_RATE_I;   // 8 s stereo/loop (PSRAM)
    // OCRAM fallback (no PSRAM): 1.0 s stereo = 2.0 s in mono = one bar at 120 BPM 4/4 —
    // the MINIMUM that can hold a musical loop. We deliberately do NOT fall back smaller:
    // a 0.2 s loop can't hold any bar length, and a card whose every option is greyed out
    // is worse than no card. If this won't fit (a lean, no-PSRAM board like the COM4 dev
    // unit), that loop simply doesn't exist -> caps.audioloop=0 and the app hides it.
    // Real capacity needs PSRAM; see planning/audio-looper/DESIGN.md §5.
    const uint32_t ramSamples = 2u * (uint32_t)AUDIO_SAMPLE_RATE_I;
    for (uint8_t i = 0; i < TDSP_AUDIOLOOP_N; i++) {
        int16_t *buf = nullptr; uint32_t n = 0;
        if (external_psram_size > 0) { n = psramSamples; buf = (int16_t *)extmem_malloc((size_t)n * sizeof(int16_t)); }
        if (!buf)                    { n = ramSamples;   buf = (int16_t *)malloc((size_t)n * sizeof(int16_t)); }
        if (!buf) break;
        g_aloopBuf[i] = buf; g_aloopBufSamples[i] = n;
        g_aloop[i].begin(&g_conductor.clock(), buf, n / 2);    // stereo default
        g_aloop[i].setBars(4);
        g_aloopN = (uint8_t)(i + 1);
    }
    Serial.printf("[aloop] %u loop(s) allocated (psram=%u MB)\n", g_aloopN, external_psram_size);
}

// Audio recording captures on the next bar downbeat, so the master grid must be running.
static void audioArmTransport() { applyMeter(); ensureTransportStarted(); }
#endif

// Lock a LOOPING song to the master beat grid (like the drums + arp) so it wraps
// drift-free and stays in phase with everything else. Call right AFTER
// g_player.play(). Only looping songs sync — full / tempo-map songs keep the ms
// engine (their tempo map needs it; PLAN §7). loopBeats: exact from the SD parse
// (`parsedLoopBeats`), else derived from the baked stream's total ms at its native
// tempo and snapped to the eighth-note grid. Leaves the player in ms mode if there
// is no usable loop length.
static void songApplySync(double parsedLoopBeats) {
    g_songLoopBeats = 0.0;
    if (!g_loop) return;                         // one-shot / full song -> ms engine
    double lb = parsedLoopBeats;
    if (lb <= 0.0)                               // baked stream (no PPQN meta): derive from ms
        lb = tdsp::smf::snapLoopBeatsHalf((double)g_player.totalMs() * (double)g_songBpm / 60000.0);
    if (lb <= 0.0) return;
    g_songLoopBeats = lb;
    g_player.setSyncedMode(&g_conductor.clock(), lb, g_songBpm);
    Serial.printf("[song] tick-synced: loop=%.2f beats @ %.1f bpm\n", lb, (double)g_songBpm);
}

// Clean slate before starting ANY song: silence sounding notes + clear latched
// per-engine expression (bend / mod / aftertouch), so a bend left mid-glide by the
// previous song can't carry over. Spare channel 10 while a drum groove is looping —
// an all-channels reset would cut the drums for a beat when you press Play.
static void songPrep() {
    if (g_drumPlayer.isPlaying()) {
        for (uint8_t ch = 1; ch <= 16; ++ch) if (ch != 10) g_synthSink->onAllNotesOff(ch);
    } else {
        g_synthSink->onAllNotesOff(0);
    }
#ifdef TDSP_REPLAYGAIN_MULTITIMBRAL
    // A song is multitimbral (each channel runs its own program), so the Tier-1 audition
    // bus trim — set to the last picker voice — no longer describes what's sounding. Reset
    // it to unity; Tier-2 per-GM-program normalization (in the engine/sink) takes over.
    synthAuditionTrim()->setGain(1.0f);
#endif
}

// Play a baked built-in (test sequence or legacy demo) by display name. Returns true
// if a built-in matched; false lets the caller fall back to an SD lookup.
FLASHMEM static bool songStartBuiltin(const char *name) {
    // Rich MPE/MIDI test sequences: set the device mode (MPE tests need per-note
    // expression) then hand the events straight to the player — no expansion needed.
    for (int i = 0; i < testsong::kNumTestSongs; ++i) {
        if (strcasecmp(name, testsong::kTestSongs[i].name) != 0) continue;
        songPrep();
        applyMidiMode(testsong::kTestSongs[i].mpe);
        g_songBpm = testsong::kTestSongs[i].bpm; applyTempos();        // per-song native tempo -> master
        snprintf(g_curSongName, sizeof g_curSongName, "%s", testsong::kTestSongs[i].name);
        snprintf(g_curSongArg,  sizeof g_curSongArg,  "%s", testsong::kTestSongs[i].name);
        // Print the BPM so the app auto-follows the song's tempo (its @STATE/[song] handler
        // sets master BPM from this) — drums + arp then lock to the same grid.
        Serial.printf("[song] %s -> %s (%s, %lu events, %.1f bpm, start)\n", g_curSongName, synthName(),
                      testsong::kTestSongs[i].mpe ? "MPE" : "MIDI", (unsigned long)testsong::kTestSongs[i].count,
                      (double)g_songBpm);
        g_songBpb = 4;   // baked test sequence: no time-sig meta -> common time
        if (g_loop) ensureTransportStarted();   // first player zeroes the grid; a song joining a groove locks in phase
        g_player.play(testsong::kTestSongs[i].ev, testsong::kTestSongs[i].count);
        songApplySync(0.0);   // baked: loopBeats derived from the stream's total ms
        applyMeter();
        g_songBarClock = 0;
        return true;
    }
    // Baked legacy demos (SongEv, tempo estimate). A prior MPE test may have left the
    // device in MPE mode — return to normal MIDI so a multitimbral song plays right.
    for (int i = 0; i < kNumBuiltin; ++i) {
        if (strcasecmp(name, kBuiltinSongs[i].name) != 0) continue;
        songPrep();
        if (g_mpeMode) applyMidiMode(false);
        g_songBpm = kBuiltinSongs[i].bpm;
        uint32_t n = tdsp::expandLegacyNotes(kBuiltinSongs[i].ev, kBuiltinSongs[i].count, g_buf, MAX_EVENTS);
        snprintf(g_curSongName, sizeof g_curSongName, "%s", kBuiltinSongs[i].name);
        snprintf(g_curSongArg,  sizeof g_curSongArg,  "%s", kBuiltinSongs[i].name);
        Serial.printf("[song] %s (%.1f bpm est) -> %s (start)\n", g_curSongName, (double)g_songBpm, synthName());
        g_songBpb = 4;   // baked legacy demo: no time-sig meta -> common time
        if (n) {
            applyTempos();
            if (g_loop) ensureTransportStarted();
            g_player.play(g_buf, n);
            songApplySync(0.0);   // baked: loopBeats derived from the stream's total ms
            applyMeter(); g_songBarClock = 0;
        }
        return true;
    }
    return false;
}

// Play an SD .mid by absolute path (disp = display name, arg = the @SONGF replay arg).
FLASHMEM static bool songStartSd(const char *path, const char *disp, const char *arg) {
    songPrep();
    if (g_mpeMode) applyMidiMode(false);
    g_songBpm = 120.0f; g_songBpb = 4; double parsedLoopBeats = 0.0;
    int got = tdsp::smf::loadSmfFile(path, g_buf, MAX_EVENTS, &g_songBpm, &g_songBpb, &parsedLoopBeats);   // + exact loop length
    if (got <= 0) { Serial.printf("[song] SD load FAILED: %s\n", path); return false; }
    snprintf(g_curSongName, sizeof g_curSongName, "%s", disp);
    snprintf(g_curSongArg,  sizeof g_curSongArg,  "%s", arg);
    Serial.printf("[song] %s (SD, %lu events, %.1f bpm, %u beats/bar, psram=%uMB ocramFree=%luKB) -> %s (start)\n",
                  disp, (unsigned long)got, (double)g_songBpm, (unsigned)g_songBpb,
                  (unsigned)external_psram_size, (unsigned long)(tdsp::smf::ocramHeapFree() / 1024), synthName());
    applyTempos();   // retime the song (and groove) to the master BPM
    if (g_loop) ensureTransportStarted();       // first player zeroes the grid; else join in phase
    g_player.play(g_buf, (uint32_t)got);
    songApplySync(parsedLoopBeats);             // lock a looping song to the grid (exact length from the parse)
    applyMeter();    // bar length from the song's time signature (song = meter master)
    g_songBarClock = 0;
    return true;
}

// Play a song by NAME — the @SONGF primitive (mirrors @DRUMF). `arg` ending in ".mid"
// is an SD file in /songs (or the card root); otherwise it's a built-in display name.
// No index, no registry, no cap.
FLASHMEM static void songStartArg(const char *arg) {
    if (!arg || !*arg) return;
    if (endsWithMid(arg)) {
        char path[128]; char disp[64]; songDisp(disp, sizeof disp, arg);
        snprintf(path, sizeof path, "/songs/%s", arg);
        if (!SD.exists(path)) snprintf(path, sizeof path, "/%s", arg);
        songStartSd(path, disp, arg);
    } else if (!songStartBuiltin(arg)) {
        Serial.printf("[song] not found: %s\n", arg);
    }
}

// Back-compat: @SONG=<i> plays the i-th catalog row (resolved via songs.ndjson).
FLASHMEM static void songStartIndex(int idx) {
    char arg[120];
    if (songByIndex(idx, arg, sizeof arg, nullptr, 0)) songStartArg(arg);
    else Serial.printf("[song] index %d out of range\n", idx);
}
static void songStop() {
    g_songWasPlaying = false;   // a manual stop must NOT trigger the loop-restart
    if (!g_player.isPlaying()) return;
    g_player.stop();
    // recenter bend + kill the song's notes — but spare ch10 so a looping groove keeps going.
    if (g_drumPlayer.isPlaying()) {
        for (uint8_t ch = 1; ch <= 16; ++ch) if (ch != 10) g_synthSink->onAllNotesOff(ch);
    } else {
        g_synthSink->onAllNotesOff(0);
    }
    Serial.println("[song] stopped");
    applyMeter();   // song gave up the meter -> revert to a looping groove's, else 4/4
}

// Called every loop(): if a looping song just ended on its own, restart it. Manual
// stops clear g_songWasPlaying above, so they don't re-trigger.
static void songLoopTick() {
    bool now = g_player.isPlaying();
    if (g_songWasPlaying && !now) {
        if (g_loop) {
            songStartArg(g_curSongArg); // re-arm the same song (also re-applies its MIDI/MPE mode)
            now = g_player.isPlaying();
        } else {
            applyMeter();               // natural end (not looping): song gives up the meter -> groove's, else 4/4
        }
    }
    g_songWasPlaying = now;
}

#if TDSP_VOICE2
// --- Player 2 (voice-2 song player) ---------------------------------------------
// A twin of the voice-1 song helpers above, targeting g_player2 -> g_synthSinkB (engines 4..7)
// with its OWN state (g_curSong2*, g_song2*, g_song2Loop). It never touches voice-1 state, the global
// meter, or the global MPE mode, so a second song plays on the keyboard voice independently and
// stays locked to the same master grid (song2ApplySync joins the running clock in phase — see
// ensureTransportStarted, which now also counts player 2 as holding the grid).
static void song2Prep() { g_synthSinkB->onAllNotesOff(0); }   // silence only voice 2's own notes
static void song2ApplySync(double parsedLoopBeats) {
    g_song2LoopBeats = 0.0;
    if (!g_song2Loop) return;                    // one-shot -> free-running ms engine
    double lb = parsedLoopBeats;
    if (lb <= 0.0)
        lb = tdsp::smf::snapLoopBeatsHalf((double)g_player2.totalMs() * (double)g_song2Bpm / 60000.0);
    if (lb <= 0.0) return;
    g_song2LoopBeats = lb;
    g_player2.setSyncedMode(&g_conductor.clock(), lb, g_song2Bpm);
    Serial.printf("[song2] tick-synced: loop=%.2f beats @ %.1f bpm\n", lb, (double)g_song2Bpm);
}
FLASHMEM static bool song2StartSd(const char *path, const char *disp, const char *arg) {
    song2Prep();
    g_song2Bpm = 120.0f; g_song2Bpb = 4; double parsedLoopBeats = 0.0;
    int got = tdsp::smf::loadSmfFile(path, g_buf2, MAX_EVENTS2, &g_song2Bpm, &g_song2Bpb, &parsedLoopBeats);
    if (got <= 0) { Serial.printf("[song2] SD load FAILED: %s\n", path); return false; }
    snprintf(g_curSong2Name, sizeof g_curSong2Name, "%s", disp);
    snprintf(g_curSong2Arg,  sizeof g_curSong2Arg,  "%s", arg);
    Serial.printf("[song2] %s (SD, %lu events, %.1f bpm) -> voice 2 (start)\n", disp, (unsigned long)got, (double)g_song2Bpm);
    applyTempos();
    if (g_song2Loop) ensureTransportStarted();   // join the running grid in phase (or define it if idle)
    g_player2.play(g_buf2, (uint32_t)got);
    song2ApplySync(parsedLoopBeats);
    return true;
}
FLASHMEM static bool song2StartBuiltin(const char *name) {
    // Baked test sequences (played verbatim; we do NOT flip the global MPE mode — that belongs to
    // voice 1). An MPE test song is an edge case on voice 2 and may render as plain multitimbral.
    for (int i = 0; i < testsong::kNumTestSongs; ++i) {
        if (strcasecmp(name, testsong::kTestSongs[i].name) != 0) continue;
        song2Prep();
        g_song2Bpm = testsong::kTestSongs[i].bpm; g_song2Bpb = 4; applyTempos();
        snprintf(g_curSong2Name, sizeof g_curSong2Name, "%s", testsong::kTestSongs[i].name);
        snprintf(g_curSong2Arg,  sizeof g_curSong2Arg,  "%s", testsong::kTestSongs[i].name);
        if (g_song2Loop) ensureTransportStarted();
        g_player2.play(testsong::kTestSongs[i].ev, testsong::kTestSongs[i].count);
        song2ApplySync(0.0);
        return true;
    }
    for (int i = 0; i < kNumBuiltin; ++i) {   // baked legacy demos (expand into g_buf2)
        if (strcasecmp(name, kBuiltinSongs[i].name) != 0) continue;
        song2Prep();
        g_song2Bpm = kBuiltinSongs[i].bpm; g_song2Bpb = 4;
        uint32_t n = tdsp::expandLegacyNotes(kBuiltinSongs[i].ev, kBuiltinSongs[i].count, g_buf2, MAX_EVENTS2);
        snprintf(g_curSong2Name, sizeof g_curSong2Name, "%s", kBuiltinSongs[i].name);
        snprintf(g_curSong2Arg,  sizeof g_curSong2Arg,  "%s", kBuiltinSongs[i].name);
        if (n) {
            applyTempos();
            if (g_song2Loop) ensureTransportStarted();
            g_player2.play(g_buf2, n);
            song2ApplySync(0.0);
        }
        return true;
    }
    return false;
}
FLASHMEM static void song2StartArg(const char *arg) {
    if (!arg || !*arg) return;
    if (endsWithMid(arg)) {
        char path[128]; char disp[64]; songDisp(disp, sizeof disp, arg);
        snprintf(path, sizeof path, "/songs/%s", arg);
        if (!SD.exists(path)) snprintf(path, sizeof path, "/%s", arg);
        song2StartSd(path, disp, arg);
    } else if (!song2StartBuiltin(arg)) {
        Serial.printf("[song2] not found: %s\n", arg);
    }
}
static void song2Stop() {
    g_song2WasPlaying = false;   // a manual stop must NOT trigger the loop-restart
    if (!g_player2.isPlaying()) return;
    g_player2.stop();
    g_synthSinkB->onAllNotesOff(0);
    Serial.println("[song2] stopped");
}
// Hard restart player 2 from the top on a fresh downbeat (mirrors songRestart for voice 1).
static void song2Restart(const char *arg) {
    if (!arg || !*arg) return;
    g_forceTransportZero = true;
    song2StartArg(arg);
    g_forceTransportZero = false;
}
// Auto-restart a looping player-2 song when it ends on its own (manual stop clears the flag).
static void song2LoopTick() {
    bool now = g_player2.isPlaying();
    if (g_song2WasPlaying && !now && g_song2Loop) { song2StartArg(g_curSong2Arg); now = g_player2.isPlaying(); }
    g_song2WasPlaying = now;
}
#endif  // TDSP_VOICE2

// --- Drum grooves (channel-10 GM percussion) --------------------------------
// A groove is a short, LOOPABLE, channel-10-only .mid on the SD card under
// /drums. A dedicated looping player (g_drumPlayer) streams it into the SAME
// synth sink the melodic voice uses, so a drum backing runs UNDER whatever you
// play live. It only makes sound on a General-MIDI engine (TSF/SF2/OPL3/OPLL) —
// a melodic-only engine (Dexed/Plaits/…) has no drum map, so drums are gated on
// synthIsGM() to avoid ch10 notes ringing out as random melodic pitches.
// Populate /drums with tools/fetch_drums.py (see assets/drums for the samples).
struct DrumRef { char name[48]; char path[96]; };
static DrumRef g_drums[48];
static int     g_numDrums = 0;
static const int MAX_DRUM_EVENTS = 4096;                    // grooves are tiny (a bar or two)
DMAMEM static tdsp::MidiFileEvent g_drumBuf[MAX_DRUM_EVENTS];

// GM drum kits — the "instrument" the Drums menu picks. Selecting one sends a
// program change on channel 10; GM engines (TSF/SF2) switch kit, others ignore.
struct DrumKit { const char *name; uint8_t prog; };
static const DrumKit kDrumKits[] = {
    {"Standard", 0}, {"Room", 8}, {"Power", 16}, {"Electronic", 24}, {"TR-808", 25},
    {"Jazz", 32}, {"Brush", 40}, {"Orchestra", 48}, {"SFX", 56},
};
static const int kNumDrumKits = sizeof(kDrumKits) / sizeof(kDrumKits[0]);

// Catalog DB (CatalogDb.h) bundled-list hook: the engine's compile-time voice names +
// the GM drum-kit table live here, so the indexer calls back into main.cpp to emit
// /tdsp/instruments.ndjson + /tdsp/drumkits.ndjson alongside the SD-scanned sources.
FLASHMEM static void catdbWriteBundled() {
    SD.remove("/tdsp/instruments.ndjson");
    File o = SD.open("/tdsp/instruments.ndjson", FILE_WRITE);
    if (o) {
        // Write every engine's instrument names — INCLUDING GM. The app has no built-in
        // GM-128 table; it only renders what the catalog carries, so GM engines (TSF/SF2)
        // must ship their 128 names here or the Synth/Voices list is empty. The names come
        // from synthInstrumentName(), the same source @DXVOICE selects, so they always agree.
        for (int i = 0; i < synthNumInstruments(); ++i) {
            o.print("{\"i\":"); o.print(i); o.print(",\"name\":");
            tdsp::catdb::jsonStr(o, synthInstrumentName(i)); o.print("}\n");
        }
        o.close();
    }
    SD.remove("/tdsp/drumkits.ndjson");
    File k = SD.open("/tdsp/drumkits.ndjson", FILE_WRITE);
    if (k) {
        for (int i = 0; i < kNumDrumKits; ++i) {
            k.print("{\"name\":"); tdsp::catdb::jsonStr(k, kDrumKits[i].name);
            k.print(",\"prog\":"); k.print(kDrumKits[i].prog); k.print("}\n");
        }
        k.close();
    }
    // songs.ndjson — scanned STRAIGHT off the card (+ baked built-ins), not a capped
    // RAM registry, so there's no song limit. Order: test seqs, then /songs (and card
    // root) *.mid, then baked demos not shadowed by an SD copy. Each row is {name} plus
    // either "file" (SD, play via @SONGF=<file>) or "builtin":true (play via @SONGF=<name>).
    SD.remove("/tdsp/songs.ndjson");
    File so = SD.open("/tdsp/songs.ndjson", FILE_WRITE);
    if (so) {
        for (int i = 0; i < testsong::kNumTestSongs; ++i) {
            so.print("{\"name\":"); tdsp::catdb::jsonStr(so, testsong::kTestSongs[i].name);
            so.print(",\"builtin\":true}\n");
        }
        if (::g_sdReady) {
            if (!SD.exists("/songs")) SD.mkdir("/songs");
            writeSongDir(so, "/songs");
            writeSongDir(so, "/");                              // also list .mid at the card root
        }
        for (int i = 0; i < kNumBuiltin; ++i) {
            if (sdSongExists(kBuiltinSongs[i].name)) continue;  // SD copy wins (tempo-bearing)
            so.print("{\"name\":"); tdsp::catdb::jsonStr(so, kBuiltinSongs[i].name);
            so.print(",\"builtin\":true}\n");
        }
        so.close();
    }
}

// Scan /drums for *.mid (created if missing). Each groove appears in the Drums
// picker; drop a .mid on the card and Refresh to add one with no rebuild.
static void buildDrumList() {
    g_numDrums = 0;
    if (!g_sdReady) return;
    if (!SD.exists("/drums")) SD.mkdir("/drums");
    File d = SD.open("/drums");
    if (!d || !d.isDirectory()) { if (d) d.close(); return; }
    const int cap = (int)(sizeof(g_drums) / sizeof(g_drums[0]));
    for (File f = d.openNextFile(); f && g_numDrums < cap; f = d.openNextFile()) {
        const char *nm = f.name();
        if (!f.isDirectory() && nm && endsWithMid(nm)) {
            DrumRef &r = g_drums[g_numDrums++];
            snprintf(r.path, sizeof(r.path), "/drums/%s", nm);
            size_t copy = strlen(nm) - 4;                   // display name = filename minus ".mid"
            if (copy > sizeof(r.name) - 1) copy = sizeof(r.name) - 1;
            memcpy(r.name, nm, copy); r.name[copy] = 0;
        }
        f.close();
    }
    d.close();
    Serial.printf("[sd] drums: %d grooves\n", g_numDrums);
}

// Does the active engine render channel-10 drums? Drum-capable backends set the
// song-player mask to kMaskAll in synthBegin() (TSF/SF2/OPL3/OPLL); melodic-only
// engines leave kMaskNoDrums. This is the right signal — NOT synthIsGM(), which is
// about streaming 128 GM program NAMES (OPLL reports false yet still plays drums).
static bool drumEngineOk() { return g_engineHasDrums; }

// One place that snapshots this build's catalog capabilities for the catalog builder.
// Adding a capability = one field here + one in EngineCaps + one meta line in CatalogDb.h;
// the @REINDEX / boot-reindex call sites never change.
static tdsp::catdb::EngineCaps engineCaps() {
    return { synthName(), drumEngineOk(), kDrumEngineName, (TDSP_ROLE_BT_RECEIVER != 0),
             kEngineUsesSoundfonts, kEngineUsesDexedLibrary };
}

// While a groove is the drums, mute the SONG's own channel-10 track so a song with
// its own drums (most full .mid) doesn't fight the groove — the groove IS the beat.
// Restored when the groove stops. No-op on engines that don't do drums.
static void muteSongDrums(bool mute) {
    if (!g_engineHasDrums) return;
    g_player.setChannelMask(mute ? tdsp::MidiFilePlayer::kMaskNoDrums
                                 : tdsp::MidiFilePlayer::kMaskAll);
}

static void drumApplyKit() {
#if defined(TDSP_DRUM_TSF)
    g_drumTsfSink.onProgramChange(10, kDrumKits[g_drumKit].prog);   // kit lives on the dedicated drum TSF
#elif defined(TDSP_DRUM_VOICE)
    g_drumVoiceSink.onProgramChange(10, kDrumKits[g_drumKit].prog); // OPLL rhythm ignores it, but stays consistent
#else
    g_synthSink->onProgramChange(10, kDrumKits[g_drumKit].prog);
#endif
}

FLASHMEM // Load + start a groove by its full SD path. Shared by the legacy numeric index
// (flat menu / serial keys) and the browser's play-by-filename (@DRUMF=), which the
// client resolves from catalog.tsv — so playback is decoupled from firmware scan order.
static void drumStartPath(const char* path, const char* disp, bool rezero = true) {
    if (!drumEngineOk()) { Serial.printf("[drum] %s has no channel-10 drum map — use TSF/SF2/OPL3/OPLL\n", synthName()); return; }
    g_drumFileBpm = 120.0f; g_drumBpb = 4; g_drumLoopBeats = 0.0;
    int got = tdsp::smf::loadSmfFile(path, g_drumBuf, MAX_DRUM_EVENTS, &g_drumFileBpm, &g_drumBpb, &g_drumLoopBeats);
    if (got <= 0) { Serial.printf("[drum] load FAILED: %s\n", path); return; }
    drumApplyKit();
    g_drumPlayer.setVelocityScale(g_drumVolPct / 100.0f);
    // A groove starting on its own downbeat with no song running becomes the tempo
    // source: snap the master to the groove's native BPM so it plays at its authored
    // feel (a 95-bpm funk groove -> master 95). When joining an already-running grid
    // (rezero=false, launch-quantize) or under a playing song, the existing tempo owns
    // it — leave the master alone. The app picks up the new BPM on its next @STATE poll.
    if (rezero && !g_player.isPlaying() && g_drumFileBpm > 1.0f) g_masterBpm = g_drumFileBpm;
    applyTempos();   // groove plays at the master BPM (x fine trim)
    // SYNCHRO START (PSS-140 style): arm the groove and let the FIRST live note kick
    // it off on beat 1 (you pick the downbeat by when you play). Otherwise start NOW
    // on beat 1 — immediate, right when you press Play (you time the press).
    if (g_drumSynchro) {
        g_drumArmed = true; g_drumArmedN = (uint32_t)got;
        Serial.printf("[drum] %s SYNCHRO armed @ %.0f bpm — play a note to start\n", disp, (double)g_masterBpm);
        return;
    }
    g_drumArmed = false;
    muteSongDrums(true);                                        // groove is the drums now
    // Zero the master clock to THIS downbeat only if the transport is idle (first
    // player wins the grid); a groove joining a playing song/groove locks in phase
    // instead. Under launch-quantize (rezero=false) we're already firing on a bar
    // edge of the free grid, so never re-zero. Zero BEFORE play()/setSyncedMode so
    // the synced anchor reads the fresh clock. PLAN §5.
    if (rezero) ensureTransportStarted();
    g_drumPlayer.play(g_drumBuf, (uint32_t)got);               // immediate: beat 1 = now
    // Tick-sync the groove to the master clock: drift-free wrap on its EXACT loop
    // length (fractional beats OK). If the parser couldn't derive a length, fall
    // back to the free-running ms engine (play() left it in ms mode). PLAN §4.5.
    if (g_drumLoopBeats > 0.0)
        g_drumPlayer.setSyncedMode(&g_conductor.clock(), g_drumLoopBeats, g_drumFileBpm);
    applyMeter();                                              // bar length from the groove's time-sig (unless a song owns the meter)
    Serial.printf("[drum] %s (%d ev, %.1f bpm, %u beats/bar, loop=%.2f beats %s) kit=%s @ master %.0f bpm vol=%d%%\n",
                  disp, got, (double)g_drumFileBpm, (unsigned)g_drumBpb, g_drumLoopBeats,
                  g_drumPlayer.isSynced() ? "SYNCED" : "ms", kDrumKits[g_drumKit].name, (double)g_masterBpm, g_drumVolPct);
}
static void drumStart(int idx) {   // legacy numeric index (flat menu / serial C/D keys)
    if (g_numDrums == 0) { Serial.println("[drum] no grooves on SD (/drums) — run tools/fetch_drums.py"); return; }
    if (idx < 0) idx = 0;
    if (idx >= g_numDrums) idx = g_numDrums - 1;
    g_drumSel = idx;
    drumStartPath(g_drums[idx].path, g_drums[idx].name);
}
static void drumStartFile(const char* fname, bool rezero = true) {   // by filename — the browser's play path
    char path[128]; snprintf(path, sizeof(path), "/drums/%s", fname);
    char disp[64]; size_t c = strlen(fname);
    if (c > 4 && strcasecmp(fname + c - 4, ".mid") == 0) c -= 4;   // strip .mid for the log
    if (c > sizeof(disp) - 1) c = sizeof(disp) - 1;
    memcpy(disp, fname, c); disp[c] = 0;
    drumStartPath(path, disp, rezero);
}
static void drumStop() {
    g_drumArmed = false;                                       // cancel a synchro-armed groove
    muteSongDrums(false);                                      // give the song back its own drums
    if (!g_drumPlayer.isPlaying()) return;
    g_drumPlayer.stop();                                       // releases the groove's ch10 notes
    Serial.println("[drum] stopped");
    applyMeter();   // groove gave up the meter -> revert to a playing song's, else 4/4
}
static void setDrumKit(int i) {
    if (i < 0) i = 0;
    if (i >= kNumDrumKits) i = kNumDrumKits - 1;
    g_drumKit = i;
    if (drumEngineOk()) drumApplyKit();
    Serial.printf("[drum] kit -> %s (prog %u)\n", kDrumKits[i].name, kDrumKits[i].prog);
}

// --- Launch quantize: the user-facing START entry points. When quantize is on they arm a
// pending launch that loop() fires on the next bar edge; otherwise they start immediately.
static void songLaunch(const char* arg) {
    if (g_launchQuantize && g_conductor.running()) {
        snprintf(g_pendingSongArg, sizeof g_pendingSongArg, "%s", arg);
        g_songLaunchPending = true; g_launchSched.barHit = false;   // wait for the NEXT bar edge
        Serial.printf("[sync] song launch armed: %s -> next bar\n", arg);
        return;
    }
    songStartArg(arg);
}
// Hard restart from the top, in time: re-zero the master transport (downbeat = now) so a
// synced/looping song begins at beat 0 — instead of jumping to the running clock's current
// phase — then play the song from its first event. Bypasses launch-quantize: this press
// DEFINES the downbeat. The re-zero also re-locks a drum groove + held arp chord to the same
// fresh downbeat, so everything restarts in phase. The app's MIDI-player Play / ‹ › use this.
static void songRestart(const char* arg) {
    if (!arg || !*arg) return;
    g_forceTransportZero = true;   // ensureTransportStarted() (called right before play()) re-zeroes even mid-playback
    songStartArg(arg);
    g_forceTransportZero = false;  // safety: clear if songStartArg bailed before consuming it (non-looping / not found)
}
static void drumLaunchFile(const char* fname) {
    if (g_launchQuantize && g_conductor.running()) {
        snprintf(g_pendingDrumFile, sizeof g_pendingDrumFile, "%s", fname);
        g_drumLaunchPending = true; g_launchSched.barHit = false;
        Serial.printf("[sync] drum launch armed: %s -> next bar\n", fname);
        return;
    }
    drumStartFile(fname);
}
// Master tempo (BPM) — one knob retimes BOTH the song and the drum groove, live.
static void setMasterBpm(int bpm) {
    if (bpm < 40) bpm = 40;
    if (bpm > 240) bpm = 240;
    g_masterBpm = (float)bpm;
    applyTempos();
    Serial.printf("[tempo] master %d bpm\n", bpm);
}
static void setDrumVol(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 150) pct = 150;
    g_drumVolPct = pct;
#if defined(TDSP_DRUM_VOICE) || defined(TDSP_DRUM_TSF)
    // The parallel drum voice owns mix slot 2, so scale THAT bus — a clean output
    // attenuation where signal + noise fall together. Velocity scaling made the OPLL
    // rhythm noisy at low levels: its hats/cymbals are noise generators whose level barely
    // tracks velocity, so lowering velocity dropped the tonal drums but not the noise floor.
    const float g = TDSP_DEFAULT_SYNTH_MAKEUP * (pct / 100.0f);   // 100 % == the 0.62 make-up
    outL.gain(2, g); outR.gain(2, g);
#else
    // Drums share the main synth (GM channel 10) — no separate bus, so per-note velocity
    // is the only per-source lever available here.
    g_drumPlayer.setVelocityScale(pct / 100.0f);
#endif
    Serial.printf("[drum] vol -> %d%%\n", pct);
}
// MIDI-player level, independent of the @VOL master. Scales the SYNTH mix bus (slot 3)
// output — a true, patch-independent volume. Velocity scaling (the old approach) had "no
// impact" because many DX7/FM voices have zero velocity->loudness sensitivity, so it only
// shifted timbre. Slot 3 carries the whole melodic synth, so this is the synth-bus fader.
static void setSongVol(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 150) pct = 150;
    g_songVolPct = pct;
#if defined(TDSP_SYNTH_DEXED_POOL) && TDSP_VOICE2
    // Voices-2 pool: apply split-aware so the keyboard voice isn't gated by this fader
    // (voice 1 -> mixA, slot 3 fixed when the pool is split). See applyPoolVols().
    synthSetSongVol(pct);
#else
    const float g = TDSP_DEFAULT_SYNTH_MAKEUP * (pct / 100.0f);
    outL.gain(3, g); outR.gain(3, g);
#endif
    Serial.printf("[song] synth-bus vol -> %d%%\n", pct);
}

// Stream the device catalog (song + instrument names, '|'-delimited) to the ESP32
// over the UART link. The ESP32 serves it on BLE so the app renders its pickers
// from whatever the device reports — adding a song/instrument is then a firmware
// change only, no app update. Sent when the ESP32 asks (@GETCAT, on BLE connect).
FLASHMEM static void sendCatalog(Print& out) {
    out.print("@SONGS=");
    { File sf = SD.open("/tdsp/songs.ndjson");                 // names straight from the catalog (no RAM registry)
      if (sf) { char l[224]; int i = 0;
        while (sf.available()) { int len = sf.readBytesUntil('\n', l, sizeof(l) - 1); l[len] = 0; if (len <= 0) continue;
          char nm[120]; if (jsonStrField(l, "name", nm, sizeof nm)) { if (i++) out.print('|'); out.print(nm); } }
        sf.close(); } }
    out.print('\n');
    // @INSTR carries an optional synth header as its first '|'-field so the app
    // MIDI page labels itself from the engine THIS firmware was built with:
    //   @INSTR=<0x1F><synthName>\t<synthDescription>|inst0|inst1|...
    // The header is marked by a leading 0x1F (unit separator). It must NOT be
    // '@' — the ESP32 relay treats every '@' as a UART line-start (see
    // t-dsp_esp32_bt_receiver), so a '@' inside the value truncates the line and
    // the whole catalog is dropped. 0x1F never appears in patch names.
    out.print("@INSTR=");
    out.write((uint8_t)0x1F);
    out.print(synthName());
    out.print('\t');
    out.print(synthDescription());
    if (synthIsGM()) {
        // A General-MIDI engine uses the 128 STANDARD program names. Streaming ~2 KB
        // of names over the UART would overflow the ESP32's line buffer AND a single
        // BLE characteristic (512 B cap) — so we just flag "GM" (a 3rd \t-field on the
        // header) and the app renders the standard GM 0..127 names itself.
        out.print("\tGM");
    }
    // Drum-capability flag (\t-field on the header, findable regardless of position):
    // lets the clients show the Drums menu as active vs "silent on this engine". This is
    // drumEngineOk() (ch10 render), NOT synthIsGM() — OPLL is not-GM yet plays drums.
    if (drumEngineOk()) out.print("\tDRUMS");
    if (!synthIsGM()) {
        for (int i = 0; i < synthNumInstruments(); ++i) { out.print('|'); out.print(synthInstrumentName(i)); }
    }
    out.print('\n');
    // Drum grooves scanned off /drums — same '|'-delimited contract as @SONGS. The
    // Drums menu's kit list is a fixed GM set (hardcoded in the clients); the
    // firmware just maps @DRUMKIT=<index> to a channel-10 program change.
    out.print("@DRUMS=");
    for (int i = 0; i < g_numDrums; ++i) { if (i) out.print('|'); out.print(g_drums[i].name); }
    out.print('\n');
    // Manifest registry: which catalog SOURCE each surface should browse for the
    // CURRENT synth/context. "file:<path>" is fetched generically via @READ (the
    // client owns all browsing/facets/paging over it); "bundled:<id>" is a static
    // list the client already ships; "engine" means use the @INSTR names above;
    // "none" = unavailable right now. Re-sent on every catalog refresh, so when the
    // synth changes the client re-points at the right manifest with NO hardcoded
    // per-engine paths — this is how each surface "knows which manifest to use".
    out.print("@MANIFESTS=");
    out.print("drums\x1f");  out.print(g_sdReady ? "file:/drums/catalog.tsv" : "none");
    out.print("|drumkit\x1fbundled:gmkits");
    out.print("|instr\x1f"); out.print(synthIsGM() ? "bundled:gm128" : "engine");
    out.print('\n');
    Serial.printf("[cat] catalog sent (synth=%s, %d drums)\n", synthName(), g_numDrums);
}

// Refresh = re-scan the SD card (picking up songs just added over USB / a reader)
// and re-send the catalog. Triggered by the app's Refresh button (@GETCAT) and on
// each BLE connect. Retries SD.begin so a card inserted after boot still mounts.
static void refreshCatalog(Print& out) {
#if TDSP_HAS_SDCARD
    if (!g_sdReady) { g_sdReady = SD.begin(BUILTIN_SDCARD); Serial.printf("[sd] retry: %s\n", g_sdReady ? "ready" : "no card"); }
#endif
    buildDrumList();
    sendCatalog(out);   // @SONGS streams from the existing songs.ndjson; new SD songs appear after @REINDEX
}

// --- Generic chunked file read (surface-agnostic catalog transport) ----------
// Any surface — web over USB CDC, or the app via ESP32/BLE — fetches an SD file
// with @READ=<path>. The file streams back as base64 frames that survive BOTH the
// '@...\n' UART line protocol and the BLE chunker:
//     @FB=<id>\x1f<path>\x1f<bytes>     begin (total byte count)
//     @FD=<id>\x1f<seq>\x1f<b64>        data chunk (360 raw bytes -> 480 b64 chars)
//     @FE=<id>\x1f<count>               end (number of data frames)
//     @FERR=<id>\x1f<reason>            error
// Raw chunk = 576 bytes (a multiple of 3) so every non-final chunk base64-encodes
// with NO '=' padding — the client concatenates all payloads into one valid base64
// string and decodes once. This ONE primitive is the whole catalog transport: a new
// catalog type is just a new file on the card + a client parser, no firmware change.
// The client owns all browsing semantics (genre/pack facets, paging); the firmware
// only serves bytes and plays a groove by name (@DRUMF=).
static const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static uint8_t g_xferId = 0;

// Host->device file WRITE receiver — the push complement of streamFile/@READ.
// A host (push_file_serial.ps1 / Web Serial) sends @WB then a raw binary payload
// that lands straight on the SD over USB CDC: ~5 MB/s, no MTP, no reflash. Wired
// USB-only (the ESP32/BLE relay is 115200 + can't carry a raw byte stream). See
// lib/TDspSdXfer.
static tdsp::SdWriteReceiver g_sdWrite(SD);

static void streamFile(Print& out, const char* path) {
    const uint8_t id = ++g_xferId;
    File f = SD.open(path);
    if (!f || f.isDirectory()) { if (f) f.close(); out.printf("@FERR=%u\x1f%s\n", id, "not found"); return; }
    out.printf("@FB=%u\x1f%s\x1f%lu\n", id, path, (unsigned long)f.size());
    uint8_t raw[360];   // 360 = mult of 3 (no mid-stream b64 pad) AND @FD line fits one ~512 BLE MTU
    char b64[4 * (sizeof(raw) / 3) + 1];
    uint32_t seq = 0;
    for (;;) {
        int n = f.read(raw, sizeof(raw));
        if (n <= 0) break;
        int o = 0, i = 0;
        for (; i + 3 <= n; i += 3) {
            uint32_t v = ((uint32_t)raw[i] << 16) | ((uint32_t)raw[i + 1] << 8) | raw[i + 2];
            b64[o++] = kB64[(v >> 18) & 63]; b64[o++] = kB64[(v >> 12) & 63];
            b64[o++] = kB64[(v >> 6) & 63];  b64[o++] = kB64[v & 63];
        }
        if (n - i == 1) {
            uint32_t v = (uint32_t)raw[i] << 16;
            b64[o++] = kB64[(v >> 18) & 63]; b64[o++] = kB64[(v >> 12) & 63];
            b64[o++] = '='; b64[o++] = '=';
        } else if (n - i == 2) {
            uint32_t v = ((uint32_t)raw[i] << 16) | ((uint32_t)raw[i + 1] << 8);
            b64[o++] = kB64[(v >> 18) & 63]; b64[o++] = kB64[(v >> 12) & 63];
            b64[o++] = kB64[(v >> 6) & 63];  b64[o++] = '=';
        }
        b64[o] = 0;
        out.printf("@FD=%u\x1f%lu\x1f%s\n", id, (unsigned long)seq++, b64);
        // Pace the ESP32/BLE link so the relay + BLE stack drain each frame. The USB
        // CDC path (web page) is flow-controlled, so stream it at full speed — pace
        // anything that ISN'T the USB Serial (i.e. the Serial7 link to the ESP32).
        if (&out != &Serial) delay(6);
        if (n < (int)sizeof(raw)) break;   // final (short) read
    }
    f.close();
    out.printf("@FE=%u\x1f%lu\n", id, (unsigned long)seq);
}

// --- Live MIDI IN (DIN on Serial1 + USB host) -> MPE-aware router -> synth ----
// Both physical sources feed one MidiRouter, which normalizes pitch bend to
// semitones (per-channel range: 2 in MIDI mode, 48 in MPE / RPN), CC74 -> timbre,
// and channel pressure -> pressure. The router then drives the same g_synthSink the
// song player uses. Callbacks are shared by the DIN (MIDI.h) and USB host (MIDIDevice)
// sources — their setHandle* signatures match. Compiled only if at least one
// hardware MIDI input exists (else nothing registers them).
#if TDSP_HAS_DIN_MIDI || TDSP_HAS_USB_MIDI_HOST
// SYNCHRO START (PSS-140 style): the first live note kicks off an armed groove on beat 1
// — you pick the downbeat by when you play. (vel 0 = note-off, ignore.) Shared by the DIN
// and USB-host note-on callbacks so a keyboard press starts the groove either way.
static void maybeSynchroStart(byte vel) {
    if (g_drumArmed && vel > 0) {
        muteSongDrums(true);
        g_conductor.start();                        // this note IS the intentional downbeat: zero here
        g_arpFilter.resyncToGrid();                 // re-lock a held arp chord to the new downbeat
        g_drumPlayer.play(g_drumBuf, g_drumArmedN);
        if (g_drumLoopBeats > 0.0)                  // lock the groove to the just-zeroed grid
            g_drumPlayer.setSyncedMode(&g_conductor.clock(), g_drumLoopBeats, g_drumFileBpm);
        applyMeter();                               // bar length from the groove's time-sig
        g_drumArmed = false;
        Serial.println("[drum] SYNCHRO start (first note)");
    }
}
static void midiNoteOn  (byte ch, byte note, byte vel) { maybeSynchroStart(vel); g_router.handleNoteOn(ch, note, vel); }
static void midiNoteOff (byte ch, byte note, byte vel) { g_router.handleNoteOff(ch, note, vel); }
static void midiCC      (byte ch, byte cc,   byte val) { g_router.handleControlChange(ch, cc, val); }
static void midiPitch   (byte ch, int bend)            { g_router.handlePitchBend(ch, (int16_t)bend); }
static void midiPressure(byte ch, byte pressure)       { g_router.handleChannelPressure(ch, pressure); }

#if TDSP_VOICE2 && TDSP_HAS_USB_MIDI_HOST
// USB-host controller callbacks: identical to the DIN ones EXCEPT that when the Voices-2
// split is on they steer to the keyboard router (g_kbdRouter -> g_synthSinkB) instead of
// the main path. With the split off they behave exactly like the shared callbacks above.
static inline tdsp::MidiRouter& usbRouter() { return g_voice2On ? g_kbdRouter : g_router; }
// Track the USB keyboard's currently-held notes so an owner switch can release them on the
// sink the keyboard is LEAVING (individual note-offs — never a panic, which would cut voice 1's
// song). Without this, a note held across a switch would hang (its key-up goes to the new sink).
static uint8_t g_usbHeldN = 0;
static struct { uint8_t ch, note; } g_usbHeld[24];
static void usbHeldAdd(uint8_t ch, uint8_t note) {
    for (uint8_t i = 0; i < g_usbHeldN; ++i) if (g_usbHeld[i].ch == ch && g_usbHeld[i].note == note) return;
    if (g_usbHeldN < 24) { g_usbHeld[g_usbHeldN].ch = ch; g_usbHeld[g_usbHeldN].note = note; g_usbHeldN++; }
}
static void usbHeldRemove(uint8_t ch, uint8_t note) {
    for (uint8_t i = 0; i < g_usbHeldN; ++i) if (g_usbHeld[i].ch == ch && g_usbHeld[i].note == note) { g_usbHeld[i] = g_usbHeld[--g_usbHeldN]; return; }
}
// Release every held keyboard note on the CURRENT owner's router, then clear. Call BEFORE
// flipping g_voice2On so the note-offs land on the sink being left (no hung notes, no song cut).
static void usbFlushHeld() {
    for (uint8_t i = 0; i < g_usbHeldN; ++i) usbRouter().handleNoteOff(g_usbHeld[i].ch, g_usbHeld[i].note, 0);
    g_usbHeldN = 0;
}
static void usbNoteOn  (byte ch, byte note, byte vel) { maybeSynchroStart(vel); if (vel) usbHeldAdd(ch, note); else usbHeldRemove(ch, note); usbRouter().handleNoteOn(ch, note, vel); }
static void usbNoteOff (byte ch, byte note, byte vel) { usbHeldRemove(ch, note); usbRouter().handleNoteOff(ch, note, vel); }
static void usbCC      (byte ch, byte cc,   byte val) { usbRouter().handleControlChange(ch, cc, val); }
static void usbPitch   (byte ch, int bend)            { usbRouter().handlePitchBend(ch, (int16_t)bend); }
static void usbPressure(byte ch, byte pressure)       { usbRouter().handleChannelPressure(ch, pressure); }
#endif
#endif  // TDSP_HAS_DIN_MIDI || TDSP_HAS_USB_MIDI_HOST

// Switch the device between normal MIDI and MPE (per-note expression). Sets the
// router's per-channel bend range (2 vs the LinnStrument's 48-semi default) and lets
// the backend reconfigure (TSF frees ch10 from drums so it's an MPE member channel).
static void applyMidiMode(bool mpe) {
    g_mpeMode = mpe;
    float range = mpe ? tdsp::MidiRouter::kDefaultPitchBendRange : 2.0f;   // 48 (MPE) vs 2
    for (uint8_t ch = 1; ch <= 16; ch++) g_router.setPitchBendRange(ch, range);
#if TDSP_VOICE2
    // The USB-host keyboard (e.g. LinnStrument) rides its own router. Track the same
    // range so per-note slides aren't clamped to +-2 semis in MPE — see g_kbdRouter setup.
    for (uint8_t ch = 1; ch <= 16; ch++) g_kbdRouter.setPitchBendRange(ch, range);
#endif
    // MPE is single-timbre: a song's per-channel program changes shouldn't apply, so the
    // whole performance (and the MPE test song) uses the SELECTED instrument, not the file's.
    g_player.setProgramChangeEnabled(!mpe);
    synthSetMpeMode(mpe);   // backend hook (no-op except TSF)
    Serial.printf("[mode] %s\n", mpe ? "MPE (per-note bend/pressure)" : "normal MIDI");
}

// Dispatch one '@'-prefixed control line. This is the single source of truth for the
// text protocol, shared by BOTH transports: the ESP32 relay (BLE app -> Serial7) and
// the USB CDC port (a Web Serial browser page, no ESP32 required). `reply` is the
// stream a query answers on (only @GETCAT replies) so each channel gets its own
// catalog. Returns true if the line was a recognized command.
#if defined(TDSP_HAS_REPLAYGAIN) && TDSP_DIAGNOSTICS
static void runGainSweep(int startIdx = 0);   // ReplayGain sweep (any backend); resumable from a voice index
#endif
#if defined(TDSP_SYNTH_DEXED_POOL) && TDSP_DIAGNOSTICS
static void runMpeSweep(int startIdx);        // MPE demo on each instrument; resumable
static void runAxisProof(int axis);           // capture 1 note with an MPE axis at full
static void runMpeCheck(void);                // measure every instrument under MPE; flag silent/clip
#endif
#if defined(TDSP_SYNTH_SF2_TSF) && TDSP_DIAGNOSTICS
static void runAxisProof(int axis);           // MPE axis proof ported to TSF (validates CC#74->cutoff)
#endif

// Dispatch one arpeggiator command against a GIVEN ArpFilter, so the same parser drives
// both the main arp ("@ARP...") and the optional Voices-2 keyboard arp ("@ARP2..."). `P`
// is the command prefix; the suffix after it (ON=, PAT=, RATE=, ...) selects the action.
// Returns true if the line was a recognized arp command for this prefix. Reply/echo lines
// carry the prefix so the app can tell the two arps apart.
FLASHMEM static bool handleArpLine(const char* line, Print& reply, tdsp::ArpFilter& A, const char* P) {
    const size_t pl = strlen(P);
    if (strncmp(line, P, pl) != 0) return false;
    const char* s = line + pl;
    if (strncmp(s, "ON=", 3) == 0) {                        // on (arpeggiate live notes) / off (bypass)
        A.setEnabled(atoi(s + 3) != 0);
        reply.printf("%sON=%d\n", P, A.enabled() ? 1 : 0);
        Serial.printf("[%s] %s\n", P + 1, A.enabled() ? "ON" : "bypass");
    }
    else if (strcmp(s, "RESTART") == 0) { A.restart(); Serial.printf("[%s] restart\n", P + 1); }  // re-trigger the cycle from step 0
    else if (strncmp(s, "PAT=", 4) == 0) {                  // pattern 0..25 (Up/.../Euclidean/UserSequence=25)
        A.setPattern((tdsp::ArpFilter::Pattern)atoi(s + 4));
        reply.printf("%sPAT=%d\n", P, (int)A.pattern());
    }
    else if (strncmp(s, "SEQ=", 4) == 0) {                  // user step-sequence table for PatUserSequence (25)
        char buf[256]; strncpy(buf, s + 4, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
        A.clearSequence();
        uint8_t idx = 0;
        for (char* tok = strtok(buf, " "); tok && idx < tdsp::ArpFilter::kMaxSteps; tok = strtok(nullptr, " ")) {
            int8_t degree;
            if (tok[0] == 'r' || tok[0] == 'R')      degree = tdsp::ArpFilter::SeqRest;
            else if (tok[0] == 'c' || tok[0] == 'C') degree = tdsp::ArpFilter::SeqChord;
            else                                     degree = (int8_t)atoi(tok);
            int oct = 0, vel = 0;
            const char* c1 = strchr(tok, ':');
            if (c1) { oct = atoi(c1 + 1); const char* c2 = strchr(c1 + 1, ':'); if (c2) vel = atoi(c2 + 1); }
            A.setSequenceStep(idx++, degree, (int8_t)oct, (uint8_t)vel);
        }
        reply.printf("%sSEQ=%d\n", P, A.sequenceLength());
    }
    else if (strncmp(s, "RATE=", 5) == 0)  { A.setRate((tdsp::ArpFilter::Rate)atoi(s + 5)); reply.printf("%sRATE=%d\n", P, (int)A.rate()); }
    else if (strncmp(s, "GATE=", 5) == 0)  { A.setGate(atoi(s + 5) / 100.0f); reply.printf("%sGATE=%d\n", P, (int)(A.gate() * 100.0f + 0.5f)); }
    else if (strncmp(s, "SWING=", 6) == 0) { A.setSwing(atoi(s + 6) / 100.0f); reply.printf("%sSWING=%d\n", P, (int)(A.swing() * 100.0f + 0.5f)); }
    else if (strncmp(s, "OCT=", 4) == 0)   { A.setOctaveRange((uint8_t)atoi(s + 4)); reply.printf("%sOCT=%d\n", P, A.octaveRange()); }
    else if (strncmp(s, "LATCH=", 6) == 0) { A.setLatch(atoi(s + 6) != 0); reply.printf("%sLATCH=%d\n", P, A.latch() ? 1 : 0); }
    else if (strncmp(s, "PRESET=", 7) == 0) {              // apply a whole preset atomically (one line = all params)
        char buf[256]; strncpy(buf, s + 7, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
        int applied = 0;
        for (char* tok = strtok(buf, " "); tok; tok = strtok(nullptr, " ")) {
            char* eq = strchr(tok, '=');
            if (!eq) continue;
            *eq = 0;
            const char* k = tok;
            const long v = strtol(eq + 1, nullptr, 10);
            using AF = tdsp::ArpFilter;
            if      (!strcmp(k, "pat"))   A.setPattern((AF::Pattern)v);
            else if (!strcmp(k, "rate"))  A.setRate((AF::Rate)v);
            else if (!strcmp(k, "gate"))  A.setGate(v / 100.0f);
            else if (!strcmp(k, "swing")) A.setSwing(v / 100.0f);
            else if (!strcmp(k, "oct"))   A.setOctaveRange((uint8_t)v);
            else if (!strcmp(k, "octm"))  A.setOctaveMode((AF::OctaveMode)v);
            else if (!strcmp(k, "latch")) A.setLatch(v != 0);
            else if (!strcmp(k, "vel"))   A.setVelMode((AF::VelMode)v);
            else if (!strcmp(k, "vfix"))  A.setFixedVelocity((uint8_t)v);
            else if (!strcmp(k, "vacc"))  A.setAccentVelocity((uint8_t)v);
            else if (!strcmp(k, "mask"))  A.setStepMask((uint32_t)v);
            else if (!strcmp(k, "len"))   A.setStepLength((uint8_t)v);
            else if (!strcmp(k, "mpe"))   A.setMpeMode((AF::MpeMode)v);
            else if (!strcmp(k, "outch")) A.setOutputChannel((uint8_t)v);
            else if (!strcmp(k, "scb"))   A.setScatterBaseChannel((uint8_t)v);
            else if (!strcmp(k, "scc"))   A.setScatterCount((uint8_t)v);
            else if (!strcmp(k, "scale")) A.setScale((AF::Scale)v);
            else if (!strcmp(k, "scroot"))A.setScaleRoot((uint8_t)v);
            else if (!strcmp(k, "tr"))    A.setTranspose((int8_t)v);
            else if (!strcmp(k, "rep"))   A.setRepeat((uint8_t)v);
            else continue;
            ++applied;
        }
        reply.printf("%sPRESET=%d\n", P, applied);
    }
    else return false;
    return true;
}

FLASHMEM static bool handleControlLine(const char* line, Print& reply) {
    if      (strncmp(line, "@VOL=", 5) == 0)      setMasterVolumePct(atoi(line + 5));
#if defined(TDSP_HAS_REPLAYGAIN) && TDSP_DIAGNOSTICS
    else if (strncmp(line, "@GAIN=", 6) == 0)     runGainSweep(atoi(line + 6));   // resume sweep from index
#endif
    else if (strncmp(line, "@DXVOICE=", 9) == 0) { synthSetInstrument(atoi(line + 9));
                                 if (g_mpeMode) synthSetMpeMode(true); }   // re-sync ch10 (MPE member)
#if defined(TDSP_SYNTH_DEXED) || defined(TDSP_SYNTH_DEXED_POOL)
    // --- Paged /dexed subfolder library browser (folder -> cart -> voice) -----
    // Lazy: each command does one on-demand SD read, so the whole ~3,700-cart
    // library is reachable without holding names in RAM. Relative to /dexed.
    else if (strncmp(line, "@DXLS=", 6) == 0) {          // @DXLS=<rel>[\t<page>]
        char rel[160]; strncpy(rel, line + 6, sizeof(rel) - 1); rel[sizeof(rel) - 1] = 0;
        int page = 0;
        char *tab = strchr(rel, '\t');
        if (tab) { *tab = 0; page = atoi(tab + 1); }
        constexpr int kPage = 32;
        tdsp::dexed::SdDirEntry ents[kPage];
        int total = 0;
        int n = tdsp::dexed::sdListDir(rel, page, kPage, ents, &total);
        int npages = (total + kPage - 1) / kPage; if (npages < 1) npages = 1;
        reply.printf("@DXLS=%s\t%d\t%d", rel, page, npages);   // path, page, npages
        for (int i = 0; i < n; ++i) reply.printf("|%c%s", ents[i].isDir ? 'D' : 'F', ents[i].name);
        reply.print('\n');
    }
    else if (strncmp(line, "@DXVL=", 6) == 0) {          // @DXVL=<relCart> -> 32 voice names
        const char *rc = line + 6;
        static char names[tdsp::dexed::kVoicesPerBank][tdsp::dexed::kVoiceNameBufBytes];
        int n = tdsp::dexed::sdCartVoiceNames(rc, names);
        reply.printf("@DXVL=%s", rc);
        for (int i = 0; i < n; ++i) reply.printf("|%s", names[i]);
        reply.print('\n');
    }
    else if (strncmp(line, "@DXPICK=", 8) == 0) {        // @DXPICK=<relCart>\t<voice>
        char buf[160]; strncpy(buf, line + 8, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
        int voice = 0;
        char *tab = strrchr(buf, '\t');
        if (tab) { *tab = 0; voice = atoi(tab + 1); }
        const char *nm = synthPickCartVoice(buf, voice);
        reply.printf("@DXPICKED=%s\t%d\t%s\n", buf, voice, nm ? nm : "?");
    }
#endif
    else if (strncmp(line, "@SONGF=", 7) == 0)    songLaunch(line + 7);     // @SONGF=<filename|name> (play by name — the app's path; bar-quantized if @QUANTIZE=1)
    else if (strncmp(line, "@SONGRESTART=", 13) == 0) songRestart(line + 13);   // hard restart on a fresh downbeat (zeroes the clock, ignores quantize) — the app's Play / ‹ ›
    else if (strncmp(line, "@SONG=", 6) == 0) {
        if (strcmp(line + 6, "stop") == 0) songStop();
        else songStartIndex(atoi(line + 6));   // @SONG=<catalog index> (legacy; resolved via songs.ndjson)
    }
#if TDSP_VOICE2
    // --- Player 2 (voice-2 song player), so a second song plays at the same time. Started
    // immediately (no launch-quantize slot); it still locks to the running grid via sync. ---
    else if (strncmp(line, "@SONG2RESTART=", 14) == 0) song2Restart(line + 14);   // hard restart player 2 on a fresh downbeat
    else if (strncmp(line, "@SONG2F=", 8) == 0)   song2StartArg(line + 8);        // @SONG2F=<filename|name>
    else if (strncmp(line, "@SONG2=", 7) == 0)  { if (strcmp(line + 7, "stop") == 0) song2Stop(); }
    else if (strncmp(line, "@LOOP2=", 7) == 0)  { g_song2Loop = (atoi(line + 7) != 0);
                                 Serial.printf("[song2] loop %s\n", g_song2Loop ? "ON" : "off"); }
#endif
    else if (strcmp(line, "@GETCAT") == 0)        refreshCatalog(reply);   // re-scan SD + send catalog
    else if (strcmp(line, "@REINDEX") == 0)       { tdsp::catdb::buildCatalog(engineCaps(), catdbWriteBundled, millis()); reply.println("@REINDEXED"); }  // rebuild /tdsp/*.ndjson DB (upsert)
    else if (strncmp(line, "@READ=", 6) == 0)     streamFile(reply, line + 6);  // generic file fetch (catalog transport)
    else if (strncmp(line, "@WB=", 4) == 0) {                                    // host->SD file write; raw payload follows. USB CDC only.
        if (&reply != &Serial) reply.println("@WERR=0\x1fusb only");
        else g_sdWrite.begin(line + 4, reply);
    }
    else if (strncmp(line, "@CRC=", 5) == 0) {                                   // checksum an SD file (round-trip verify for @WB)
        uint32_t crc = 0, bytes = 0;
        if (tdsp::SdWriteReceiver::fileCrc32(SD, line + 5, crc, bytes))
            reply.printf("@CRCR=%s\x1f%08lx\x1f%lu\n", line + 5, (unsigned long)crc, (unsigned long)bytes);
        else reply.printf("@CRCERR=%s\n", line + 5);
    }
    else if (strncmp(line, "@DRUM=", 6) == 0) {                            // drum groove play/stop
        if (strcmp(line + 6, "stop") == 0) drumStop();
        else drumStart(atoi(line + 6));   // @DRUM=<groove index> (legacy flat menu)
    }
    else if (strncmp(line, "@DRUMF=", 7) == 0)    drumLaunchFile(line + 7); // @DRUMF=<filename> (browser, via catalog.tsv; bar-quantized if @QUANTIZE=1)
    else if (strncmp(line, "@DRUMKIT=", 9) == 0)   setDrumKit(atoi(line + 9));    // GM kit ("instrument")
    else if (strncmp(line, "@DRUMVOL=", 9) == 0)    setDrumVol(atoi(line + 9));    // 0..150 %
    else if (strncmp(line, "@SONGVOL=", 9) == 0)    setSongVol(atoi(line + 9));    // 0..150 %, MIDI player level (independent of @VOL master)
    else if (strncmp(line, "@BPM=", 5) == 0)        setMasterBpm(atoi(line + 5));  // master tempo (song+drum)
    else if (strncmp(line, "@DRUMSYNCHRO=", 13) == 0) { g_drumSynchro = (atoi(line + 13) != 0);   // start-on-first-note
                                 Serial.printf("[drum] synchro start %s\n", g_drumSynchro ? "ON (play a note to start)" : "off (start on Play)"); }
    else if (strncmp(line, "@QUANTIZE=", 10) == 0) {   // launch quantize: defer song/groove start to the next bar edge
        g_launchQuantize = (atoi(line + 10) != 0);
        if (!g_launchQuantize) { g_songLaunchPending = g_drumLaunchPending = false; }   // dropping the mode cancels any armed launch
        reply.printf("@QUANTIZE=%d\n", g_launchQuantize ? 1 : 0);
        Serial.printf("[sync] launch quantize %s\n", g_launchQuantize ? "ON (starts land on the next bar)" : "off (start now)");
    }
    else if (strncmp(line, "@HPF=", 5) == 0)      setDacHpfMode(atoi(line + 5));
    else if (strncmp(line, "@LOOP=", 6) == 0)   { g_loop = (atoi(line + 6) != 0);
                                 Serial.printf("[song] loop %s\n", g_loop ? "ON" : "off"); }
    else if (strncmp(line, "@SYNCPROBE=", 11) == 0) {   // 1 Hz drift probe: master beat vs each synced player's cursor
        g_syncProbe = (atoi(line + 11) != 0); g_syncProbeClock = 0;
        reply.printf("@SYNCPROBE=%d\n", g_syncProbe ? 1 : 0);
        Serial.printf("[sync] probe %s\n", g_syncProbe ? "ON (1 Hz)" : "off");
    }
#ifdef TDSP_METRONOME
    else if (strncmp(line, "@METRO=", 7) == 0) {   // click on/off; follows master BPM + meter automatically
        metroSetEnabled(atoi(line + 7) != 0);
        reply.printf("@METRO=%d\n", g_metroOn ? 1 : 0);
        Serial.printf("[metro] %s\n", g_metroOn ? "ON" : "off");
    }
    else if (strncmp(line, "@METROSIG=", 10) == 0) {   // metronome time signature = N beats/bar (accent on beat 1)
        int n = atoi(line + 10); if (n < 1) n = 1; if (n > 16) n = 16;
        g_metroBpb = (uint8_t)n;
        g_metroBeatIdx = 0;   // restart the metronome's own bar so the new signature is heard from beat 1
        applyMeter();         // keep the idle @STATE clock.bpb / arp grid consistent with the chosen signature
        reply.printf("@METROSIG=%d\n", g_metroBpb);
        Serial.printf("[metro] time signature = %d/4\n", g_metroBpb);
    }
    else if (strncmp(line, "@METROVOL=", 10) == 0) {   // click level 0..150 %, independent of the @VOL master
        setMetroVol(atoi(line + 10));
        reply.printf("@METROVOL=%d\n", g_metroVolPct);
        Serial.printf("[metro] volume = %d%%\n", g_metroVolPct);
    }
#endif
#if TDSP_RECORDER
    // Beat-aware MIDI loop recorder. @RECV picks the voice; @RECBARS/@RECSIG set the
    // loop length + time signature; @REC arms (replace) / stops; @RECDUB overdubs;
    // @RECCLR wipes the clip; @RECPLAY resumes a stopped clip. Recording begins on the
    // first note press and locks to the bar downbeat (see project_midi_loop_recorder).
    else if (strncmp(line, "@RECV=", 6) == 0) {                 // select target voice (1|2)
        int v = atoi(line + 6); if (v != 2) v = 1;
        g_recVoice = (uint8_t)v;
        reply.printf("@RECV=%d\n", g_recVoice);
    }
    else if (strncmp(line, "@RECBARS=", 9) == 0) {              // loop length: 1,2,4,8 bars
        // PER-VOICE (was: both). Each synth owns its own loop length — synth A can run a 2-bar
        // riff under synth B's 8-bar pad. @RECV selects which one this sets, same as the
        // other record controls. The time signature stays global (it IS the master meter, see
        // @RECSIG below) — loop LENGTH is per-synth, the grid they lock to is shared.
        int b = atoi(line + 9);
        recSel()->setBars((uint8_t)b);
        reply.printf("@RECBARS=%d\n", recSel()->bars());
    }
    else if (strncmp(line, "@RECSIG=", 8) == 0) {               // time signature N/4 -> drives the master meter
        int n = atoi(line + 8); if (n < 1) n = 1; if (n > 16) n = 16;
        g_metroBpb = (uint8_t)n;                                // the record grid rides the metronome meter
        applyMeter();                                           // so the clock bars-up on N/4 while idle
        reply.printf("@RECSIG=%d\n", g_metroBpb);
    }
    else if (strncmp(line, "@REC=", 5) == 0) {                  // 1 = arm (replace), 0 = stop
        if (atoi(line + 5) != 0) { recArmTransport(/*startClick=*/true); recSel()->armRecord(); }
        else recSel()->stop();
        reply.printf("@REC=%d\n", (int)recSel()->state());
    }
    else if (strncmp(line, "@RECDUB=", 8) == 0) {               // 1 = arm overdub onto the existing clip
        if (atoi(line + 8) != 0) { recArmTransport(/*startClick=*/false); recSel()->armOverdub(); }
        else recSel()->stop();
        reply.printf("@REC=%d\n", (int)recSel()->state());
    }
    else if (strcmp(line, "@RECCLR") == 0) {                    // wipe the selected voice's clip
        recSel()->clear();
        reply.printf("@REC=%d\n", (int)recSel()->state());
    }
    else if (strncmp(line, "@RECPLAY=", 9) == 0) {             // 1 = resume a stopped clip, 0 = stop
        if (atoi(line + 9) != 0) { recArmTransport(/*startClick=*/false); recSel()->resume(); }
        else recSel()->stop();
        reply.printf("@REC=%d\n", (int)recSel()->state());
    }
#endif
#if TDSP_AUDIOLOOP
    // Audio loop recorder. @ALSEL picks the loop; @ALBARS/@ALMONO/@ALFOLLOW/@ALLEVEL
    // configure it; @AL arms (replace)/stops; @ALDUB overdubs; @ALCLR wipes; @ALPLAY
    // resumes; @ALSAVE writes /loops/<name>.wav. Recording starts on the next bar downbeat.
    else if (strncmp(line, "@ALSEL=", 7) == 0) {
        int i = atoi(line + 7); if (i < 0) i = 0; if (g_aloopN && i >= g_aloopN) i = g_aloopN - 1;
        g_aloopSel = (uint8_t)i; reply.printf("@ALSEL=%d\n", g_aloopSel);
    }
    else if (strncmp(line, "@ALBARS=", 8) == 0) { alSel()->setBars((uint8_t)atoi(line + 8)); reply.printf("@ALBARS=%d\n", alSel()->bars()); }
    else if (strncmp(line, "@ALMONO=", 8) == 0) { aloopInit(g_aloopSel, atoi(line + 8) != 0); reply.printf("@ALMONO=%d\n", alSel()->mono() ? 1 : 0); }
    else if (strncmp(line, "@ALFOLLOW=", 10) == 0) { bool on = atoi(line + 10) != 0; alSel()->setClockFollow(on); reply.printf("@ALFOLLOW=%d\n", on ? 1 : 0); }
    else if (strncmp(line, "@ALLEVEL=", 9) == 0) { int v = atoi(line + 9); if (v < 0) v = 0; if (v > 100) v = 100; alSel()->setReturnLevel(v / 100.0f); reply.printf("@ALLEVEL=%d\n", v); }
    else if (strncmp(line, "@AL=", 4) == 0) {
        if (g_aloopN == 0) reply.print("@AL=-1\n");
        else { if (atoi(line + 4) != 0) { audioArmTransport(); alSel()->armRecord(); } else alSel()->stop();
               reply.printf("@AL=%d\n", (int)alSel()->state()); }
    }
    else if (strncmp(line, "@ALDUB=", 7) == 0) {
        if (g_aloopN == 0) reply.print("@AL=-1\n");
        else { if (atoi(line + 7) != 0) { audioArmTransport(); alSel()->armOverdub(); } else alSel()->stop();
               reply.printf("@AL=%d\n", (int)alSel()->state()); }
    }
    else if (strcmp(line, "@ALCLR") == 0) { alSel()->clear(); reply.printf("@AL=%d\n", (int)alSel()->state()); }
    else if (strncmp(line, "@ALPLAY=", 8) == 0) {
        if (g_aloopN == 0) reply.print("@AL=-1\n");
        else { if (atoi(line + 8) != 0) { audioArmTransport(); alSel()->resume(); } else alSel()->stop();
               reply.printf("@AL=%d\n", (int)alSel()->state()); }
    }
    else if (strncmp(line, "@ALSAVE=", 8) == 0) {   // save the selected loop as /loops/<name>.wav
        char path[96]; snprintf(path, sizeof(path), "/loops/%s.wav", line + 8);
        SD.mkdir("/loops");
        bool ok = tdsp::saveWavFile(path, *alSel());
        reply.printf("@ALSAVE=%s\t%d\n", line + 8, ok ? 1 : 0);
        Serial.printf("[aloop] save %s -> %d\n", path, ok ? 1 : 0);
    }
#endif
    else if (strncmp(line, "@APP=", 5) == 0) {   // store the opaque app-owned state blob (see g_appState)
        strncpy(g_appState, line + 5, sizeof(g_appState) - 1);
        g_appState[sizeof(g_appState) - 1] = 0;
        reply.printf("@APP=%s\n", g_appState);   // echo so the app can confirm the round-trip
    }
    else if (strcmp(line, "@APP") == 0)          // query: return the stored app-state blob
        reply.printf("@APP=%s\n", g_appState);
#ifdef TDSP_SYNTH_DEXED_POOL
    else if (strncmp(line, "@PRESSURE=", 10) == 0) {   // pressure routing bitmask:
        uint8_t m = (uint8_t)atoi(line + 10);          // 1=VOL 2=BRIGHT 4=VIB 8=TREM (combine)
        g_poolSink.setPressureMask(m);
        Serial.printf("[press] mask=%u  vol=%d bright=%d vib=%d trem=%d\n", m,
                      (m & 1) != 0, (m & 2) != 0, (m & 4) != 0, (m & 8) != 0);
    }
    else if (strncmp(line, "@MODWHEEL=", 10) == 0) {   // mod-wheel routing (VOL bit ignored):
        uint8_t m = (uint8_t)atoi(line + 10);          // 2=BRIGHT 4=VIB 8=TREM (combine)
        g_poolSink.setModMask(m);
        Serial.printf("[mod] mask=%u  bright=%d vib=%d trem=%d\n", g_poolSink.modMask(),
                      (m & 2) != 0, (m & 4) != 0, (m & 8) != 0);
    }
    else if (strncmp(line, "@TIMBRE=", 8) == 0) {      // CC74 timbre (MPE Y) routing (VOL ignored):
        uint8_t m = (uint8_t)atoi(line + 8);           // 2=BRIGHT 4=VIB 8=TREM (combine)
        g_poolSink.setTimbreMask(m);
        Serial.printf("[timbre] mask=%u  bright=%d vib=%d trem=%d\n", g_poolSink.timbreMask(),
                      (m & 2) != 0, (m & 4) != 0, (m & 8) != 0);
    }
#if TDSP_DIAGNOSTICS
    else if (strncmp(line, "@MPESWEEP=", 10) == 0) runMpeSweep(atoi(line + 10));   // MPE demo on each instrument from <start>
    else if (strncmp(line, "@PROOF=", 7) == 0)     runAxisProof(atoi(line + 7));   // capture 1 note w/ axis at full (0=press 1=timbre 2=bend 3=neutral)
    else if (strcmp(line, "@MPECHECK") == 0)       runMpeCheck();                  // QA every instrument under MPE (silent/clip)
#endif
    else if (strncmp(line, "@LFOMODE=", 9) == 0) {     // 0 = respect patch LFO, 1 = force LFO
        bool force = atoi(line + 9) != 0;
        g_poolSink.setLfoForce(force);
        synthSetInstrument(g_synthInstrument);         // reload so RESPECT restores the patch's own LFO
        Serial.printf("[lfo] mode = %s\n", force ? "FORCE (vib/trem on any patch)" : "RESPECT patch LFO");
    }
#endif
#if defined(TDSP_SYNTH_SF2_TSF) && TDSP_DIAGNOSTICS
    else if (strncmp(line, "@PROOF=", 7) == 0)     runAxisProof(atoi(line + 7));   // capture 1 note w/ axis at full (0=press 1=timbre 2=bend 3=neutral)
#endif
    else if (strncmp(line, "@MIDIMODE=", 10) == 0) applyMidiMode(atoi(line + 10) != 0);
#ifdef TDSP_HAS_REPLAYGAIN
    else if (strncmp(line, "@RG=", 4) == 0) {          // ReplayGain master switch (Tier-1 + Tier-2)
        tdsp::g_replayGainOn = (atoi(line + 4) != 0);
        // Re-apply the Tier-1 audition trim under the new state — but only when NOT mid-song:
        // a playing song already neutralizes Tier-1, and its Tier-2 per-channel trims re-gate
        // on the next Program Change (gmProgramTrim() honors the switch), so re-selecting the
        // instrument here would needlessly stomp the song's per-channel programs.
        if (!g_player.isPlaying()) synthSetInstrument(g_synthInstrument);
#if defined(TDSP_SYNTH_DEXED_POOL) && TDSP_VOICE2
        // Voice 2 carries its OWN Tier-1 trim (dxpTrimB), so re-gate it too — otherwise the
        // toggle would only re-gain synth A. Gain-only (no reload), so it's safe mid-play.
        synthReapplyVoice2Trim();
#endif
        reply.printf("@RG=%d\n", tdsp::g_replayGainOn ? 1 : 0);
        Serial.printf("[synth] ReplayGain %s\n", tdsp::g_replayGainOn ? "ON" : "off");
    }
    else if (strcmp(line, "@RG") == 0) reply.printf("@RG=%d\n", tdsp::g_replayGainOn ? 1 : 0);  // query
#endif
    else if (strncmp(line, "@CAP", 4) == 0) {          // capture output samples -> PC (tools/capture_analyze.py)
        int n = (line[4] == '=') ? atoi(line + 5) : OutCaptureProbe_F32::kCapN;
        if (n < 1) n = 1;
        if (n > OutCaptureProbe_F32::kCapN) n = OutCaptureProbe_F32::kCapN;
        g_outCap.arm();
        uint32_t t0 = millis();
        while (!g_outCap.done() && millis() - t0 < 2000) delay(1);   // wait for the buffer to fill
        int got = g_outCap.count(); if (got > n) got = n;
        reply.printf("[cap] begin %d rate %d\n", got, (int)AUDIO_SAMPLE_RATE_EXACT);
        const float *c = g_outCap.data();
        char lb[220];
        for (int i = 0; i < got; ) {
            int p = 0;
            for (int k = 0; k < 16 && i < got; k++, i++) p += snprintf(lb + p, sizeof(lb) - p, "%.6g ", (double)c[i]);
            reply.println(lb);
        }
        reply.println("[cap] end");
    }
    // --- Arpeggiator (lib/TDspArp) — steps at the master BPM via the Conductor. Parsed by
    // handleArpLine() so the main arp ("@ARP...") and the optional Voices-2 keyboard arp
    // ("@ARP2...") share one implementation. Try the longer "@ARP2" prefix FIRST so an
    // "@ARP2..." line isn't swallowed by the "@ARP" matcher.
#if TDSP_VOICE2 && TDSP_ARP2
    else if (handleArpLine(line, reply, g_arpFilter2, "@ARP2")) { /* handled */ }
#endif
    else if (handleArpLine(line, reply, g_arpFilter, "@ARP")) { /* handled */ }
#if TDSP_VOICE2
    // --- Voices 2: a second Dexed voice on the keyboard half of the pool (engines 4..7) ---
    else if (strncmp(line, "@VOICE2=", 8) == 0) {          // move the USB keyboard's owner (seamless)
        bool on = (atoi(line + 8) != 0);
        if (on != g_voice2On) {
#if TDSP_HAS_USB_MIDI_HOST
            usbFlushHeld();          // release held keyboard notes on the sink being LEFT (no hang, no song cut)
#endif
            g_voice2On = on;         // usbRouter() now steers the keyboard to the new voice
            synthSetVoice2Enabled(on);   // clear the keyboard voice's engines (never touches voice 1)
        }
        reply.printf("@VOICE2=%d\n", g_voice2On ? 1 : 0);
    }
    else if (strncmp(line, "@VOICE2VOL=", 11) == 0) {      // Voices-2 level 0..150 %
        synthSetVoice2Vol(atoi(line + 11));
        reply.printf("@VOICE2VOL=%d\n", g_voice2VolPct);
    }
    else if (strncmp(line, "@DXVOICE2=", 10) == 0) synthSetInstrument2(atoi(line + 10));   // bundled voice for the keyboard half
    else if (strncmp(line, "@DXPICK2=", 9) == 0) {         // @DXPICK2=<relCart>\t<voice> for the keyboard half
        char buf[160]; strncpy(buf, line + 9, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
        int voice = 0;
        char *tab = strrchr(buf, '\t');
        if (tab) { *tab = 0; voice = atoi(tab + 1); }
        const char *nm = synthPickCartVoice2(buf, voice);
        reply.printf("@DXPICKED2=%s\t%d\t%s\n", buf, voice, nm ? nm : "?");
    }
#endif
    // Full current-state snapshot (one JSON line) so the app can hydrate every card on
    // connect instead of assuming defaults. Reports what the device actually knows —
    // i.e. what's ACTIVE, not a UI "selection" the firmware never sees.
    else if (strcmp(line, "@STATE") == 0) {
        int volPct = g_appMasterPct;   // app-facing master is the digital gain
        if (volPct < 0) volPct = 0; if (volPct > 100) volPct = 100;
        reply.printf("@STATE={\"vol\":%d,\"hpf\":%d,\"bpm\":%d,\"loop\":%d,\"quant\":%d,", volPct, g_hpf, (int)(g_masterBpm + 0.5f), g_loop ? 1 : 0, g_launchQuantize ? 1 : 0);
        reply.printf("\"arp\":{\"on\":%d,\"pat\":%d,\"rate\":%d,\"oct\":%d,\"latch\":%d},",
                     g_arpFilter.enabled() ? 1 : 0, (int)g_arpFilter.pattern(), (int)g_arpFilter.rate(),
                     g_arpFilter.octaveRange(), g_arpFilter.latch() ? 1 : 0);
        reply.printf("\"song\":{\"playing\":%d,\"p\":%d,\"sync\":%d,\"vol\":%d,\"name\":", g_player.isPlaying() ? 1 : 0, g_player.positionPermille(), g_player.isSynced() ? 1 : 0, g_songVolPct);
        tdsp::catdb::jsonStr(reply, g_curSongName); reply.print("},");
        reply.printf("\"drums\":{\"kit\":%d,\"playing\":%d,\"sync\":%d,\"vol\":%d},", g_drumKit, g_drumPlayer.isPlaying() ? 1 : 0, g_drumPlayer.isSynced() ? 1 : 0, g_drumVolPct);
#ifdef TDSP_METRONOME
        reply.printf("\"metro\":%d,\"metrosig\":%d,\"metrovol\":%d,", g_metroOn ? 1 : 0, g_metroBpb, g_metroVolPct);   // metronome on/off + time sig + click level (feature build only)
#endif
        // Master-clock beat/bar so the app can show a downbeat indicator: beat is
        // 1-based (1 == the downbeat), bpb = beats per bar (from the content's time
        // signature), barp = 0..1000 permille through the bar, run = clock running.
        { tdsp::Clock &clk = g_conductor.clock();
          reply.printf("\"clock\":{\"beat\":%d,\"bpb\":%d,\"barp\":%d,\"run\":%d},",
                       clk.beatInBar() + 1, clk.beatsPerBar(),
                       (int)(clk.barPhase() * 1000.0f + 0.5f), clk.running() ? 1 : 0); }
#if TDSP_RECORDER
        // Loop recorder, per voice: loop length (bars1/bars2), state (0=idle 1=armed 2=recording
        // 3=overdub 4=playing) and progress permille. Each synth's MIDI player owns its own
        // recorder, so every field is per-voice; `v` is just which one the @REC* commands
        // currently target (each player aims it at its own voice before acting).
        reply.printf("\"rec\":{\"v\":%d,\"bars1\":%d,\"st1\":%d,\"p1\":%d",
                     g_recVoice, g_loop1.bars(), (int)g_loop1.state(), g_loop1.positionPermille());
#if TDSP_VOICE2
        reply.printf(",\"bars2\":%d,\"st2\":%d,\"p2\":%d",
                     g_loop2.bars(), (int)g_loop2.state(), g_loop2.positionPermille());
#endif
        reply.print("},");
#endif
#if TDSP_AUDIOLOOP
        // Audio loops: selected slot, how many actually allocated (n), the selected
        // loop's config + state (same 0..4 codes as "rec") + progress permille, and its
        // capacity in tenths of a second so the app can grey out bars that can't fit.
        reply.printf("\"aloop\":{\"sel\":%d,\"n\":%d,\"bars\":%d,\"mono\":%d,\"follow\":%d,\"st\":%d,\"p\":%d,\"cap\":%d},",
                     g_aloopSel, g_aloopN, alSel()->bars(), alSel()->mono() ? 1 : 0,
                     alSel()->clockFollow() ? 1 : 0, (int)alSel()->state(),
                     alSel()->positionPermille(), (int)(alSel()->capSeconds() * 10.0f + 0.5f));
#endif
        reply.print("\"voice\":{");
#if defined(TDSP_SYNTH_DEXED) || defined(TDSP_SYNTH_DEXED_POOL)
        if (g_curCartRel[0]) {   // last pick was a /dexed cart voice (@DXPICK)
            reply.print("\"cart\":"); tdsp::catdb::jsonStr(reply, g_curCartRel);
            reply.printf(",\"cv\":%d,\"name\":", g_curCartVoice); tdsp::catdb::jsonStr(reply, g_curCartName);
        } else
#endif
        { reply.printf("\"i\":%d,\"name\":", synthInstrument()); tdsp::catdb::jsonStr(reply, synthInstrumentName(synthInstrument())); }
        reply.print("}");   // close "voice"
#if TDSP_VOICE2
        // Voices 2: the keyboard-half state (split on/off, level, and its selected voice)
        // so the app rehydrates the second card on connect.
        reply.printf(",\"voice2\":{\"on\":%d,\"vol\":%d,", g_voice2On ? 1 : 0, g_voice2VolPct);
        if (g_curCart2Rel[0]) {
            reply.print("\"cart\":"); tdsp::catdb::jsonStr(reply, g_curCart2Rel);
            reply.printf(",\"cv\":%d,\"name\":", g_curCart2Voice); tdsp::catdb::jsonStr(reply, g_curCart2Name);
        } else {
            reply.printf("\"i\":%d,\"name\":", g_synthInstrument2); tdsp::catdb::jsonStr(reply, synthInstrumentName(g_synthInstrument2));
        }
        reply.print("}");
#if TDSP_ARP2
        reply.printf(",\"arp2\":{\"on\":%d,\"pat\":%d,\"rate\":%d,\"oct\":%d,\"latch\":%d}",
                     g_arpFilter2.enabled() ? 1 : 0, (int)g_arpFilter2.pattern(), (int)g_arpFilter2.rate(),
                     g_arpFilter2.octaveRange(), g_arpFilter2.latch() ? 1 : 0);
#endif
        // Player 2 (voice-2 song player): playing/position/loop + name, so the app rehydrates the
        // second MIDI-player card. Its level shares the voice-2 bus (voice2.vol above).
        reply.printf(",\"song2\":{\"playing\":%d,\"p\":%d,\"sync\":%d,\"loop\":%d,\"name\":",
                     g_player2.isPlaying() ? 1 : 0, g_player2.positionPermille(), g_player2.isSynced() ? 1 : 0, g_song2Loop ? 1 : 0);
        tdsp::catdb::jsonStr(reply, g_curSong2Name); reply.print("}");
#endif
        // Build-time capabilities so the app SHOWS the Voices-2 / arp-2 cards only on builds
        // that have them compiled in (both are pool-only, build-flag gated).
        // caps.audioloop = the number of audio loops that ACTUALLY allocated (0 = the board
        // couldn't spare the RAM -> the app hides the card), not just the build flag.
        reply.printf(",\"caps\":{\"voice2\":%d,\"arp2\":%d,\"rec\":%d,\"audioloop\":%d}",
                     TDSP_VOICE2 ? 1 : 0, (TDSP_VOICE2 && TDSP_ARP2) ? 1 : 0, TDSP_RECORDER ? 1 : 0,
#if TDSP_AUDIOLOOP
                     (int)g_aloopN
#else
                     0
#endif
                     );
        reply.print("}\n");   // close root object
        reply.printf("@APP=%s\n", g_appState);   // opaque app-owned state, emitted with @STATE so one connect rehydrates both
    }
    else return false;
    return true;
}

FLASHMEM void setup() {
    hardResetCodecPower();

    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 1500) {}

    Serial.println();
    if (CrashReport) { Serial.println("!!! CRASH REPORT (previous run) !!!"); Serial.print(CrashReport); }
    Serial.println("=== spike_esp32_bt_spdif_mix_kit (TDspProgrammingKit) ===");
    Serial.printf("[psram] external PSRAM: %u MB\n", external_psram_size);
    Serial.println("MIX: (A) ESP32 A2DP  +  (B) S/PDIF optical loopback tone  -> TAC5212.");
    Serial.println("Connect a TOSLINK cable pin14(OUT)->pin15(IN). Pair 'T-DSP' and play.");

    // Pause the audio graph while flashing the ESP32 so the USB<->ESP32 passthrough
    // isn't CPU-starved into dropping bytes.
    kit.onFlashEnter([] { AudioNoInterrupts(); });

    // Boot the ESP32 into its A2DP app FIRST (frees the shared I2C bus), holding EN+IO0
    // high. kit.begin() also sets up the LED (heartbeat).
    Serial.println("[setup] kit.begin() -> boot ESP32 into app (EN+IO0 held)..."); Serial.flush();
    kit.begin();
    delay(300);

    Serial.println("[setup] i2c bus recover..."); Serial.flush();
    i2cBusRecover();
    pinMode(18 /*SDA0*/, INPUT_PULLUP); delayMicroseconds(20);
    if (digitalRead(18) == LOW) {
        g_codecOk = false; g_codecMsg = "i2c wedged - skipped";
        Serial.println("[setup] !! I2C SDA STILL LOW -> SKIP codec init. "
                       "Use 'i' after the bus frees.");
        Serial.flush();
    } else {
        Serial.println("[setup] Wire.begin..."); Serial.flush();
        Wire.begin();
        Wire.setClock(100000);
        tdspMuxAutoSelectCodec(TAC5212_I2C_ADDRESS);
        setupCodec();
        Serial.println("[setup] codec init done"); Serial.flush();
    }

    // SD card (Teensy 4.1 built-in slot): scan /songs/*.mid so songs can be added
    // by copying files to the card. Falls back to the built-in songs if no card.
#if TDSP_HAS_SDCARD
    // Retry SD.begin a few times: a card can need a moment after power-up, so a single
    // attempt at boot often false-reports "no card" for a perfectly good card (it then
    // mounts on a later @GETCAT/Refresh). Looping here mounts it at boot instead.
    for (int i = 0; i < 10 && !g_sdReady; ++i) { g_sdReady = SD.begin(BUILTIN_SDCARD); if (!g_sdReady) delay(40); }
    Serial.printf("[sd] card %s\n", g_sdReady ? "ready" : "not present");
    // MTP: present the SD to the host over USB so songs can be dropped into /songs
    // without pulling the card. Serial (debug + ESP32 flash bridge) is unaffected.
    // Only in the Serial+MTP USB build. A plain USB_SERIAL build (e.g. the Linux
    // flash host, where the MTP composite's DTR handshake fails on cdc_acm and the
    // DTR-gated Serial never transmits) skips MTP; files reach the SD over @WB instead.
#ifdef USB_MTPDISK_SERIAL
    MTP.begin();
    if (g_sdReady) MTP.addFilesystem(SD, "T-DSP Songs");
#endif
#endif
    // Songs are catalog-backed (no RAM registry); seed the current-song slot + browse
    // cursor from songs.ndjson if a catalog exists, so @STATE and the dev keys have a
    // starting point before the app picks one. New songs appear after @REINDEX.
    { char nm[64]; if (songByIndex(0, g_curSongArg, sizeof g_curSongArg, nm, sizeof nm)) snprintf(g_curSongName, sizeof g_curSongName, "%s", nm); }
    Serial.printf("[sd] songs: %d in catalog\n", songCatalogCount());
    buildDrumList();   // scan /drums for loopable channel-10 grooves

    // Two pools now: the int16 pool feeds Dexed, the BT resampler, the optical-out
    // tone, and the input side of the convert blocks; the F32 pool feeds the mix
    // bus, converts, S/PDIF-in and the TDM output.
    AudioMemory(80);   // headroom for up to 4 OPM banks (ymfm multitimbral); Dexed uses far less
#if TDSP_AUDIOLOOP
    // The audio loops add nodes to the F32 graph (N loopers + the final L/R mix), each
    // allocating blocks per update — give the pool headroom or they starve and drop out.
    AudioMemory_F32(60 + 6 + 4 * TDSP_AUDIOLOOP_N);
#else
    AudioMemory_F32(60);
#endif
    setMix(1.0f, 0.0f, 1.0f);
    outL.gain(3, TDSP_DEFAULT_SYNTH_MAKEUP);  outR.gain(3, TDSP_DEFAULT_SYNTH_MAKEUP);  // synth (slot 3) mix make-up in the
                                                 // F32 domain, where there's real headroom.
    testTone.frequency(440.0f);  testTone.amplitude(0.0f);
    spdifTone.frequency(1000.0f); spdifTone.amplitude(0.25f);
    if (g_codecOk) applyVol();   // codec at its fixed board level (g_dvol)
    applyAppMaster();            // digital app master start (TDSP_DEFAULT_APP_VOL_PCT)

#if TDSP_HAS_DIN_MIDI
    // Physical MIDI IN on Serial1 (pin 0), omni, soft-thru off -> the router.
    MIDI.begin(MIDI_CHANNEL_OMNI);
    MIDI.turnThruOff();
    MIDI.setHandleNoteOn(midiNoteOn);
    MIDI.setHandleNoteOff(midiNoteOff);
    MIDI.setHandlePitchBend(midiPitch);
    MIDI.setHandleControlChange(midiCC);
#endif
#if TDSP_HAS_USB_MIDI_HOST
    // USB host: a controller (LinnStrument) plugged into the Teensy 4.1 host port. On
    // Voices-2 builds it uses the split-aware usb* callbacks (steer to the keyboard router
    // when @VOICE2=1); otherwise the shared DIN callbacks (main path).
    g_usbHost.begin();
#if TDSP_VOICE2
    g_usbMidi.setHandleNoteOn(usbNoteOn);
    g_usbMidi.setHandleNoteOff(usbNoteOff);
    g_usbMidi.setHandleControlChange(usbCC);
    g_usbMidi.setHandlePitchChange(usbPitch);
    g_usbMidi.setHandleAfterTouchChannel(usbPressure);
#else
    g_usbMidi.setHandleNoteOn(midiNoteOn);
    g_usbMidi.setHandleNoteOff(midiNoteOff);
    g_usbMidi.setHandleControlChange(midiCC);
    g_usbMidi.setHandlePitchChange(midiPitch);
    g_usbMidi.setHandleAfterTouchChannel(midiPressure);   // channel pressure = MPE Z-axis
#endif
#endif

    // Live MIDI -> arp -> synth. The arp is a router sink; in bypass (default) it
    // forwards every event verbatim to its downstream synth sink, so behaviour is
    // identical until @ARPON=1. It steps on the router's onClock() (fed by the
    // Conductor's 24-PPQN tick hook), so its rate divisions lock to the master BPM.
    // The song + drum players call g_synthSink DIRECTLY (below), bypassing the arp,
    // so only LIVE keyboard/app notes are arpeggiated — never the backing groove.
    g_arpFilter.setClock(&g_conductor.clock());
    g_arpFilter.addDownstream(g_synthSink);
    g_router.addSink(&g_arpFilter);
#if TDSP_AUDIOLOOP
    audioLoopSetup();   // allocate the audio-loop buffers (PSRAM else OCRAM) + final-mix gains
#endif
#if TDSP_RECORDER
    // Voice-1 loop recorder: tap the arp downstream (captures the BAKED note stream
    // the synth hears) and play the loop back into the synth sink directly.
    g_loop1.begin(&g_conductor.clock(), g_synthSink);
    g_arpFilter.addDownstream(&g_loop1);
#endif

#if TDSP_VOICE2
    // Voices 2: the USB-host keyboard's own router -> its own synth sink (engines 4..7),
    // separate from the main path so the song/arp/drums keep running on voice 1. The
    // per-channel bend range is owned by applyMidiMode() (2 normal / 48 MPE, matching
    // g_router) so an MPE controller's per-note slides aren't clamped to +-2 semis; the
    // startup applyMidiMode() call below sets it. With TDSP_ARP2, an independent arp sits
    // in front of the keyboard sink (bypassed by default, so still a live instrument until
    // @ARP2ON=1); it steps on the keyboard router's onClock (driven by the same Conductor).
#if TDSP_ARP2
    g_arpFilter2.setClock(&g_conductor.clock());
    g_arpFilter2.addDownstream(g_synthSinkB);
    g_kbdRouter.addSink(&g_arpFilter2);
#else
    g_kbdRouter.addSink(g_synthSinkB);
#endif
#if TDSP_RECORDER
    // Voice-2 loop recorder: tap wherever the keyboard's baked output lands (arp2
    // downstream when TDSP_ARP2, else the keyboard router directly).
    g_loop2.begin(&g_conductor.clock(), g_synthSinkB);
#if TDSP_ARP2
    g_arpFilter2.addDownstream(&g_loop2);
#else
    g_kbdRouter.addSink(&g_loop2);
#endif
#endif
#endif

    // Route the song player into the build-selected synth via its shared sink.
    // Omni so every song channel (and live MIDI on any channel) reaches the one
    // patch; the player's default mask still skips channel 10 (drums), matching
    // a single melodic engine. synthBegin() sets gain + loads the default patch.
    g_player.setSink(&g_arpFilter);   // song notes go through the arp too (bypassed when arp off = normal playback)
    // Dedicated drum-groove player: channel 10 only, loops, and ignores the file's
    // program changes (we own the kit via @DRUMKIT). Feeds the same GM sink so a
    // groove backs whatever the melodic voice/keyboard plays.
    g_drumPlayer.setSink(g_synthSink);
    g_drumPlayer.setChannelMask((uint16_t)(1u << 9));   // MIDI channel 10 (index 9)
    g_drumPlayer.setProgramChangeEnabled(false);
    g_drumPlayer.setLooping(true);
    // The song player must NEVER panic ch10 on stop/restart, or it cuts a looping
    // groove for a beat when you press Play/Stop on a song. (Drums are the groove's.)
    g_player.setPanicMask(tdsp::MidiFilePlayer::kMaskNoDrums);
#if TDSP_VOICE2
    // Player 2 -> voice 2 (engines 4..7), THROUGH arp-2 when present — mirroring voice 1's
    // g_player -> g_arpFilter. Two reasons: parity (the arp treats a song the same on both
    // synths, and is bypassed when off), and it puts player 2's song on the arp downstream where
    // g_loop2 taps — so voice 2's loop recorder captures the SAME combined post-arp stream as
    // voice 1 (song + live keyboard) instead of the keyboard alone. Without arp-2 compiled in
    // there's no filter to pass through, so go straight to the sink.
#if TDSP_ARP2
    g_player2.setSink(&g_arpFilter2);
#else
    g_player2.setSink(g_synthSinkB);
#endif
    // Melodic voice: skip ch10, never panic it.
    g_player2.setChannelMask(tdsp::MidiFilePlayer::kMaskNoDrums);
    g_player2.setPanicMask(tdsp::MidiFilePlayer::kMaskNoDrums);
#endif

    // --- Master clock wiring --------------------------------------------------
    // Register the song + drum players as tempo followers so the one BPM knob
    // (applyTempos) retimes both. Internal source = free-running at the master
    // BPM (no external gear assumed). The internal 24-PPQN tick is fanned through
    // the router so an arp / onClock() sink drops in with no extra wiring. The
    // ClockSink on the router is the external-MIDI-clock seam: it's dormant until
    // real-time (0xF8/Start/Stop) handlers are added and the source is switched
    // to Clock::External (see lib/TDspTempo/README.md §6).
    g_conductor.begin(g_masterBpm);
    g_conductor.addFollower(&g_songFollow);
    g_conductor.addFollower(&g_drumFollow);
#if TDSP_VOICE2
    g_conductor.addFollower(&g_songFollow2);   // player 2 retimes with the master BPM too
#endif
    g_conductor.addFollower(&g_launchSched);   // flags bar edges so loop() can fire quantized launches
    g_router.addSink(&g_clockSink);
    g_conductor.setTickHook(+[](void*){
        g_router.handleClock();
#if TDSP_VOICE2
        g_kbdRouter.handleClock();   // step the keyboard-path arp (arp2) on the master grid
#endif
    }, nullptr);
    synthBegin();
    // Capture the engine's drum capability NOW (synthBegin set the song mask to
    // kMaskAll on drum-capable engines). drumEngineOk() reads this, so we're free to
    // toggle g_player's live channel mask later to mute a song's drums under a groove.
    g_engineHasDrums = (g_player.channelMask() == tdsp::MidiFilePlayer::kMaskAll);
#ifdef TDSP_DRUM_TSF
    // Melodic engine + dedicated GM-drum TSF: bring up the drum engine, route the groove
    // player's channel 10 to it (instead of the melodic sink), and mark drums available
    // regardless of the melodic engine's own no-drum song mask.
    if (drumTsfBegin()) {
        g_drumPlayer.setSink(&g_drumTsfSink);
        g_engineHasDrums = true;
    }
#endif
#ifdef TDSP_DRUM_VOICE
    // Same idea with the OPLL rhythm voice (no PSRAM): route ch10 to it + mark drums OK.
    if (drumVoiceBegin()) {
        g_drumPlayer.setSink(&g_drumVoiceSink);
        g_engineHasDrums = true;
    }
#endif
    applyMidiMode(TDSP_DEFAULT_MPE != 0);   // start mode (board-configurable; default normal MIDI, after synthBegin)

#if TDSP_HAS_SDCARD
    // Auto-heal a stale browse catalog. /tdsp/index.ndjson persists on the SD across
    // flashes, so after reflashing this board to a DIFFERENT synth the app keeps showing
    // the previous engine until a manual Refresh. If the stored engine no longer matches
    // the running one (or no catalog exists yet), rebuild it now — engine capability is
    // finalized above, so drumEngineOk() is valid here. Unchanged engine -> no rebuild.
    if (g_sdReady) {
        char stored[64]; int storedVer = 0;
        bool have = tdsp::catdb::readStoredEngine(stored, sizeof stored, &storedVer);
        bool engineChanged  = !have || strcmp(stored, synthName()) != 0;
        bool versionChanged = storedVer != tdsp::catdb::kCatalogVersion;   // builder layout/content bumped
        if (engineChanged || versionChanged) {
            Serial.printf("[catdb] catalog stale (engine %s->%s, v %d->%d) -> auto-reindex\n",
                          have ? stored : "(none)", synthName(), storedVer, tdsp::catdb::kCatalogVersion);
            tdsp::catdb::buildCatalog(engineCaps(), catdbWriteBundled, millis());
        }
    }
#endif

    Serial.println("running -- cmds: t=DACtone a=BT+SPDIF mix  s=SPDIF-only  m=BT-only");
    Serial.println("                 x=toggle SPDIF tone  +/-=vol  d=dump  i=re-init codec");
    Serial.println("                 W=play/stop song  S=next song  V=next instrument   MIDI-IN pin0");
    Serial.println("                 D=play/stop drums  C=next groove (GM engines only)");
    Serial.println("      ESP32/kit:  r=reset  g=flash mode  @BOOTAPP@=exit flash  U=Teensy prog");
    Serial.println("                 P=ESP32 pairing mode  F=ESP32 forget bond + pair");

    // LATE, SETTLED reset — the automatic "press BOOT for you" once everything's configured.
    Serial.println("[setup] settle 2.5s, then late kit.bootApp()..."); Serial.flush();
    delay(2500);
    kit.bootApp();
}


// --- Developer bench diagnostics / self-tests (opt-in) ----------------------
// Relocated to Diagnostics.inc.h. Compiled in only when TDSP_DIAGNOSTICS is set
// (default on; a lean product build uses -D TDSP_DIAGNOSTICS=0 to drop them).
#if TDSP_DIAGNOSTICS
#include "Diagnostics.inc.h"
#endif

void loop() {
    // Flash-mode passthrough owns the loop (also handles @BOOTAPP@); in run mode this
    // ticks the slow LED heartbeat and returns false.
    if (kit.service(Serial)) return;

#if TDSP_HAS_SDCARD && defined(USB_MTPDISK_SERIAL)
    MTP.loop();   // service USB file transfers to/from the SD (host drag-and-drop)
#endif

    // Advance the master clock BEFORE draining MIDI. In Internal mode it emits
    // catch-up 24-PPQN ticks (fanned to onClock() consumers via the tick hook)
    // and fires bar-edge callbacks to followers; in External mode this is the
    // stall watchdog. Cheap when nothing's due.
    g_conductor.update(micros());

#ifdef TDSP_METRONOME
    metroPoll();   // strike/decay the on-beat click (consumes the clock's beat latch)
#endif

    beatEmitPoll();   // @BEAT=<beatInBar>/<beatsPerBar> once per beat, for the app's beat lights

    // Launch quantize: fire any armed song/groove exactly on a bar edge (flagged by
    // g_launchSched during the update() above), so they land on the shared downbeat. The
    // groove fires with rezero=false so it aligns to the free grid instead of resetting it.
    if (g_launchSched.barHit) {
        g_launchSched.barHit = false;
        if (g_songLaunchPending) { g_songLaunchPending = false; songStartArg(g_pendingSongArg); }
        if (g_drumLaunchPending) { g_drumLaunchPending = false; drumStartFile(g_pendingDrumFile, /*rezero=*/false); }
    }

    // Live MIDI: drain DIN + USB-host controllers, then advance the (non-blocking) song.
#if TDSP_HAS_DIN_MIDI
    while (MIDI.read()) { /* handlers fire per message */ }
#endif
#if TDSP_HAS_USB_MIDI_HOST
    g_usbHost.Task();
    while (g_usbMidi.read()) { /* USB-host MIDI handlers fire per message */ }
#endif
    g_player.tick();
#if TDSP_VOICE2
    g_player2.tick();      // advance the second (voice-2) song player
#endif
    g_drumPlayer.tick();   // loops internally (setLooping), so no external re-arm needed
    g_arpFilter.tick(micros());   // drain the arp's gate-off queue (note steps fire on onClock)
#if TDSP_VOICE2 && TDSP_ARP2
    g_arpFilter2.tick(micros());  // same for the keyboard-path arp
#endif
#if TDSP_RECORDER
    g_loop1.poll(); g_loop1.tick();   // close the record window on the beat + advance loop playback
#if TDSP_VOICE2
    g_loop2.poll(); g_loop2.tick();
#endif
    recPollClick();   // stop the count-in click the instant the loop is captured
#endif
#if TDSP_AUDIOLOOP
    // Audio loops: foreground service (starts capture on the bar downbeat when armed and
    // snapshots the clock-follow rate). Playback itself runs in the audio ISR (update()).
    for (uint8_t i = 0; i < g_aloopN; i++) g_aloop[i].poll();
#endif
    songLoopTick();   // auto-restart the song if loop mode is on and it just ended
#if TDSP_VOICE2
    song2LoopTick();  // same for player 2
#endif

    // @SYNCPROBE: once/second, print the master beat next to each synced player's
    // loop-relative cursor. Relative phase must stay CONSTANT for a drift-free lock
    // (PLAN §9) — watch drum-vs-song over a long run to confirm ~0 drift.
    if (g_syncProbe && g_syncProbeClock >= 1000) {
        g_syncProbeClock = 0;
        Serial.printf("[sync] master=%.4f  drum{sync=%d cur=%.4f loop=%.2f}  song{sync=%d cur=%.4f loop=%.2f}\n",
                      g_conductor.clock().positionBeats(),
                      g_drumPlayer.isSynced() ? 1 : 0, g_drumPlayer.syncCursorBeat(), g_drumPlayer.syncLoopBeats(),
                      g_player.isSynced() ? 1 : 0, g_player.syncCursorBeat(), g_player.syncLoopBeats());
    }

    // Push song-playback position to the app (drives the MIDI Player progress bar).
    // ~2.5x/sec while playing; one "@SONGP=-1" on the falling edge resets the bar and
    // clears the ♪ flag. Runs AFTER songLoopTick() so a loop re-arm keeps us "playing"
    // (no spurious -1 at the loop seam).
    {
        static elapsedMillis songPosClock;
        static bool          songPosPrev = false;
        const bool songPosNow = g_player.isPlaying();
        if (songPosNow) {
            if (songPosClock >= 400) { songPosClock = 0; Serial.printf("@SONGP=%u\n", g_player.positionPermille()); }
        } else if (songPosPrev) {
            Serial.println("@SONGP=-1");
        }
        songPosPrev = songPosNow;
    }
#if TDSP_VOICE2
    // Same position feed for player 2 (@SONG2P=), driving the second MIDI-player card's bar.
    {
        static elapsedMillis song2PosClock;
        static bool          song2PosPrev = false;
        const bool song2PosNow = g_player2.isPlaying();
        if (song2PosNow) {
            if (song2PosClock >= 400) { song2PosClock = 0; Serial.printf("@SONG2P=%u\n", g_player2.positionPermille()); }
        } else if (song2PosPrev) {
            Serial.println("@SONG2P=-1");
        }
        song2PosPrev = song2PosNow;
    }
#endif

#if TDSP_RECORDER
    // Live loop-recorder telemetry: "@RECP=<st1>,<p1>,<st2>,<p2>" (state 0=idle 1=armed
    // 2=recording 3=overdub 4=playing; p = permille) ~4x/sec while any looper is active,
    // plus one edge frame when it goes idle. Same USB-only push as @SONGP/@BEAT (the app
    // reflects taps optimistically, so BLE stays responsive without an ESP32 relay case).
    {
        static elapsedMillis recPosClock;
        static bool          recPrev = false;
        int st1 = (int)g_loop1.state(), p1 = g_loop1.positionPermille(), st2 = 0, p2 = 0;
#if TDSP_VOICE2
        st2 = (int)g_loop2.state(); p2 = g_loop2.positionPermille();
#endif
        const bool recNow = (st1 != (int)tdsp::MidiLooper::Idle) || (st2 != (int)tdsp::MidiLooper::Idle);
        if ((recNow && recPosClock >= 250) || (recNow != recPrev)) {
            recPosClock = 0;
            Serial.printf("@RECP=%d,%d,%d,%d\n", st1, p1, st2, p2);
        }
        recPrev = recNow;
    }
#endif

#if TDSP_AUDIOLOOP
    // Live audio-loop telemetry: "@ALP=<st0>,<p0>[,<st1>,<p1>...]" (state 0=idle 1=armed
    // 2=recording 3=overdub 4=playing; p = permille) ~4x/sec while any loop is active,
    // plus one edge frame when they all go idle. USB-only push, same as @RECP/@SONGP.
    {
        static elapsedMillis alClock;
        static bool          alPrev = false;
        bool active = false;
        for (uint8_t i = 0; i < g_aloopN; i++)
            if (g_aloop[i].state() != tdsp::AudioLooper::Idle) { active = true; break; }
        if ((active && alClock >= 250) || (active != alPrev)) {
            alClock = 0;
            char b[80]; int o = snprintf(b, sizeof(b), "@ALP=");
            for (uint8_t i = 0; i < TDSP_AUDIOLOOP_N && o < (int)sizeof(b) - 12; i++) {
                const int st = (i < g_aloopN) ? (int)g_aloop[i].state() : 0;
                const int p  = (i < g_aloopN) ? g_aloop[i].positionPermille() : 0;
                o += snprintf(b + o, sizeof(b) - o, "%s%d,%d", i ? "," : "", st, p);
            }
            Serial.println(b);
        }
        alPrev = active;
    }
#endif

    g_sdWrite.tick(Serial, millis());   // abort a stalled @WB transfer (watchdog)

    // USB CDC input serves two roles: '@'-prefixed control LINES (the same protocol
    // the ESP32 relays from BLE — lets a Web Serial browser page drive the device with
    // NO ESP32 attached) and single debug KEYS (t/a/s/W/...). A byte of '@' starts a
    // command line; anything else is a key. They can't collide (keys are never '@').
    static char usbLine[288];   // 288 fits a full 32-step @ARPSEQ line; other cmds are far shorter
    static size_t usbN = 0;
    static bool usbInCmd = false;
    while (Serial.available()) {
        // While an @WB write is in flight, incoming bytes are the raw payload —
        // route them straight to the SD before the line assembler sees them.
        if (g_sdWrite.receiving()) { g_sdWrite.pump(Serial, Serial); if (g_sdWrite.receiving()) break; else continue; }
        int c = Serial.read();
        if (usbInCmd) {
            if (c == '\n' || usbN >= sizeof(usbLine) - 1) {
                usbLine[usbN] = 0;
                if (!handleControlLine(usbLine, Serial)) Serial.printf("[usb] ? %s\n", usbLine);
                usbN = 0; usbInCmd = false;
            } else if (c != '\r') {
                usbLine[usbN++] = (char)c;
            }
            continue;
        }
        if (c == '@') { usbInCmd = true; usbN = 0; usbLine[usbN++] = '@'; continue; }
        if (!kit.handleChar(Serial, c)) {     // g / r / U handled by the kit
            if (c == 'P') { kit.uart().write('p'); Serial.println("[cmd] -> ESP32: ENTER pairing mode"); }
            else if (c == 'F') { kit.uart().write('f'); Serial.println("[cmd] -> ESP32: FORGET bond + pairing mode"); }
            else if (c == 'X') { kit.uart().write('x'); Serial.println("[cmd] -> ESP32: DISCONNECT A2DP source"); }
            else if (c == 't') { testTone.amplitude(0.4f); setMix(0.0f, 1.0f, 0.0f);
                                 Serial.println("[cmd] local DAC tone 440Hz -> BOTH"); }
            else if (c == 'a') { testTone.amplitude(0.0f); setMix(1.0f, 0.0f, 1.0f);
                                 Serial.println("[cmd] MIX: BT + S/PDIF"); }
            else if (c == 's') { testTone.amplitude(0.0f); setMix(0.0f, 0.0f, 1.0f);
                                 Serial.println("[cmd] S/PDIF-in only"); }
            else if (c == 'm') { testTone.amplitude(0.0f); setMix(1.0f, 0.0f, 0.0f);
                                 Serial.println("[cmd] Bluetooth only"); }
            else if (c == 'x') { static bool on = true; on = !on;
                                 spdifTone.amplitude(on ? 0.25f : 0.0f);
                                 Serial.printf("[cmd] S/PDIF out tone %s\n", on ? "ON" : "OFF"); }
            else if (c == '+') { setMasterVolumePct(g_appMasterPct + 5); }   // app master +5%
            else if (c == '-') { setMasterVolumePct(g_appMasterPct - 5); }   // app master -5%
            else if (c == 'd') { Serial.printf("[reg] RX_OFF(26)=%02X RX_CH1(28)=%02X RX_CH2(29)=%02X "
                                 "CH_EN(76)=%02X PWR(78)=%02X\n",
                                 g_codec.readRegister(0, 0x26), g_codec.readRegister(0, 0x28),
                                 g_codec.readRegister(0, 0x29), g_codec.readRegister(0, 0x76),
                                 g_codec.readRegister(0, 0x78)); }
            else if (c == 'i') { Serial.println("[cmd] re-init codec"); setupCodec();
                                 if (g_codecOk) applyVol(); applyAppMaster();
                                 Serial.printf("[cmd] codec=%s (%s), out %.0f dB, app master %d%%\n",
                                               g_codecOk ? "OK" : "FAIL", g_codecMsg, (double)g_dvol, g_appMasterPct); }
            else if (c == 'W') { if (g_player.isPlaying()) songStop();                       // play/stop
                                 else if (g_curSongArg[0]) songStartArg(g_curSongArg);
                                 else songStartIndex(0); }
            else if (c == 'S') { int n = songCatalogCount();                                 // browse to the next song (no play)
                                 if (n > 0) { g_songBrowse = (g_songBrowse + 1) % n;
                                   char nm[64]; if (songByIndex(g_songBrowse, g_curSongArg, sizeof g_curSongArg, nm, sizeof nm)) {
                                     snprintf(g_curSongName, sizeof g_curSongName, "%s", nm);
                                     Serial.printf("[song] selected: %s\n", g_curSongName); } } }
            else if (c == 'D') { if (g_drumPlayer.isPlaying()) drumStop(); else drumStart(g_drumSel); }  // drums play/stop
            else if (c == 'C') { if (g_numDrums) g_drumSel = (g_drumSel + 1) % g_numDrums;   // Cycle groove
                                 Serial.printf("[drum] selected: %s\n", g_drums[g_drumSel].name);
                                 if (g_drumPlayer.isPlaying()) drumStart(g_drumSel); }
            else if (c == 'V') { synthSetInstrument((synthInstrument() + 1) % synthNumInstruments());
                                 if (g_mpeMode) synthSetMpeMode(true); }   // re-sync ch10 (MPE member)
            else if (c == 'M') { Serial.printf("[mem] external PSRAM: %u MB\n", external_psram_size); }
#if TDSP_DIAGNOSTICS
            else if (c == 'T') { runInstrumentSelfTest(); }   // exercise all 128 GM + drums, log peaks
            else if (c == 'B') { runPitchBendTest(); }         // audible pitch-bend sweep on ch1
#endif
            else if (c == 'E') { applyMidiMode(!g_mpeMode); }  // toggle MIDI <-> MPE mode locally
            else if (c == 'O') { g_loop = !g_loop; Serial.printf("[song] loop %s\n", g_loop ? "ON" : "off"); }  // lOop toggle
#if TDSP_DIAGNOSTICS
            else if (c == 'A') { runMpeTest(); }               // simulate an MPE note (bend + pressure)
#endif
#if defined(TDSP_SYNTH_DEXED_POOL) && TDSP_DIAGNOSTICS
            else if (c == 'K') { runPizzClipTest(273); }       // pizz clip probe: is the attack snap clipping?
            else if (c == 'J') { runPizzCapture(273, 60, 110); } // capture onset waveform -> serial (aliasing/zero-cross)
            else if (c == 'R') { dxpClip.resetWorst(); adcProbe.resetWorst(); Serial.println("[jump] worst-discontinuity detectors reset (digital + analog)"); }
            else if (c == 'G') { dumpWorstJump(); }             // dump worst DIGITAL step captured during playback
            else if (c == 'H') { dumpAdcWorstJump(); }          // dump worst ANALOG (loopback) step during playback
            else if (c == 'L') { runLoopbackCapture(13, 60, 110); }  // capture digital + analog loopback (13 JUPITER exemplifies the snap)
            else if (c == 'Y') { runSlotScan(); }                    // scan all 8 TDM-in slots for the ADC loopback signal
            else if (c == 'Q') { runPressureProof(); }               // capture a full-pressure note (prove vibrato/tremolo)
            else if (c == 'Z') { runMpeSweep(synthInstrument()); }   // MPE demo on every instrument from the current one
#endif
#if defined(TDSP_HAS_REPLAYGAIN) && TDSP_DIAGNOSTICS
            else if (c == 'N') { runGainSweep(); }              // ReplayGain: sweep every voice, print trim table
#endif
        }
    }

    // Mirror the ESP32's UART log to USB, line-buffered with an [esp] prefix.
    static char line[288];   // 288 fits a full 32-step @ARPSEQ line relayed from the BLE app
    static size_t n = 0;
    while (kit.uart().available()) {
        char c = (char)kit.uart().read();
        if (c == '\n' || n >= sizeof(line) - 1) {
            line[n] = 0;
            if (n) {
                // Control lines from the ESP32 (relayed from the BLE app) are acted
                // on here; everything else is just mirrored to USB with an [esp] tag.
                // @GETCAT replies back to the ESP32 over its UART.
                if (!handleControlLine(line, kit.uart())) Serial.printf("[esp] %s\n", line);
            }
            n = 0;
        } else if (c != '\r') {
            line[n++] = c;
        }
    }

    // Status heartbeat print (the LED itself is driven by kit.service()).
    if (hb >= 1000) {
        hb = 0;
#if TDSP_ROLE_BT_RECEIVER
        float pbt = peakBt.available()    ? peakBt.read()    : 0.0f;
#else
        float pbt = 0.0f;   // BT receiver gated out
#endif
        float psp = peakSpdif.available() ? peakSpdif.read() : 0.0f;
        float po  = peakOut.available()   ? peakOut.read()   : 0.0f;
        Serial.printf("alive up=%lus  bpm=%.0f(%s)  codec=%s(%s)  spdif=%s inFreq=%.0f  "
                      "btPeak=%.3f spdifPeak=%.3f outPeak=%.3f  cpuMax=%.1f%% memMax=%u\n",
                      (unsigned long)(millis() / 1000),
                      (double)g_masterBpm, g_conductor.running() ? "run" : "idle",
                      g_codecOk ? "OK" : "FAIL", g_codecMsg,
                      AsyncAudioInputSPDIF3::isLocked() ? "LOCKED" : "no-signal",
#if TDSP_SPDIF_IN
                      spdifIn.getInputFrequency(),
#else
                      0.0f,   // optical IN compiled out (TDSP_NO_SPDIF_IN)
#endif
                      pbt, psp, po,
                      AudioProcessorUsageMax(), AudioMemoryUsageMax());
        AudioProcessorUsageMaxReset();   // make cpuMax a per-second rolling peak
        AudioMemoryUsageMaxReset();
#ifdef TDSP_SYNTH_DEXED_POOL
        // Synth-sum clip watch (pre-0.62 mix): shows per-engine int16 railing that the
        // final outPeak hides. During real song playback, railed>0 == audible clipping.
        Serial.printf("  [synth] sumPeak=%.4f railed=%lu/%lu  maxJump=%.4f worstJump=%.4f\n",
                      (double)dxpClip.peak(), (unsigned long)dxpClip.clipped(), (unsigned long)dxpClip.total(),
                      (double)dxpClip.maxJump(), (double)dxpClip.worstJump());
        // Analog-loopback discontinuity watch: adcMaxJump >> synth maxJump == a pop the
        // codec/DAC added that isn't in the digital sum. Baseline ~0.03 (bright-edge slew).
        Serial.printf("  [adc]   maxJump=%.4f worstJump=%.4f\n",
                      (double)adcProbe.maxJump(), (double)adcProbe.worstJump());
        dxpClip.reset(); dxpClip.resetPeriod();
        adcProbe.resetPeriod();
#endif
    }
}

