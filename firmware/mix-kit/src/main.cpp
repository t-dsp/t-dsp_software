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
// FX (hexefx plate reverb, lib/TDspHexeFx) as a master INSERT — build-flag gated so non-FX
// builds are byte-identical. TDSP_FX_PLATE implies the TDSP_FX umbrella. Its ~190 KB delay
// buffers malloc from RAM2 (measured — planning/plate-reverb-fx/COST.md), so only RAM2-
// headroom envs enable it. Include the SPECIFIC effect header (not the pruned umbrella).
#if (defined(TDSP_FX_PLATE) && TDSP_FX_PLATE) || (defined(TDSP_FX_SPRING) && TDSP_FX_SPRING)
#undef TDSP_FX
#define TDSP_FX 1
#endif
#ifndef TDSP_FX
#define TDSP_FX 0
#endif
#if TDSP_FX_PLATE
#include "effect_platereverb_F32.h"   // rich, ~190 KB RAM2 heap (PSRAM boards, or PSRAM-alloc patch)
#elif TDSP_FX_SPRING
#include "effect_springreverb_F32.h"  // ~37 KB heap — fits no-PSRAM boards
#endif
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
#include "DrumNoteMap.h"         // ch10 Roland/TD-11 -> GM note remap shim (rescues GMD hi-hats 22/26)
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
// Track: one voice's whole stack (player+arp+router+follow+looper+sink+state) as a single
// binding, so Voice 1 & Voice 2 share ONE helper family. Phase 1 of the Tracks refactor —
// see Track.h / planning/tracks/DESIGN.md. Forward-declares only; no behavior change yet.
#include "Track.h"
#include "TrackEngine.h"   // ITrackEngine: uniform per-track engine surface (adapters land in TrackEngineImpl.inc.h)

#ifdef TDSP_FLASHERX
#include "FlasherXUpdate.inc.h"   // @FXUP -> OTA self-update (lib/FlasherX). Opt-in.
#endif

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

// (E) USB Audio interface (host <-> Teensy), F32-native 24-bit / 48 kHz. Build-flag
// gated: needs the USB_MIDI_AUDIO_SERIAL descriptor + -D AUDIO_SUBSLOT_SIZE=3 (which
// arm the patched core's Q31 rings) — the env sets both. See lib/USB_Audio_F32_24 and
// planning/usb-audio-mixkit/DESIGN.md.
#ifndef TDSP_USB_AUDIO
#define TDSP_USB_AUDIO 0
#endif
// External-input sub-mixer: USB, optical S/PDIF-in, and Bluetooth are collected into ONE
// DRY bus (extL/extR) that rejoins the signal path AFTER the reverb insert — so external
// audio BYPASSES the reverb by default (project decision). Auto-on with USB audio; a build
// can also force it with -D TDSP_EXT_MIX=1 to move BT/optical out of the reverb path.
#ifndef TDSP_EXT_MIX
#define TDSP_EXT_MIX TDSP_USB_AUDIO
#endif
#if TDSP_USB_AUDIO
#include <AudioInputUSB_F32.h>
#include <AudioOutputUSB_F32.h>
AudioInputUSB_F32  usbIn;                    // host -> Teensy  (into the external sub-mix)
AudioOutputUSB_F32 usbOut;                   // Teensy -> host  (records the instrument mix)
static int      g_usbGainPct  = 100;         // USB-in return level 0..150 % (@USBGAIN); slot-2 gain of extL/extR
static uint32_t g_usbLastPop  = 0;           // last-seen AudioInputUSB_F32::pop_ok, for the @STATE "active" delta
#endif

// --- Synth-voice count (Phase 3): how many independent Dexed synth voices this build has. Default
// preserves today (1 unified, or 2 on a split/voice2 build); a 4-voice env sets -D TDSP_SYNTH_VOICES=4.
// The per-voice objects below are declared as arrays of this size; g_player/g_player2 alias [0]/[1]
// so all existing (voice 0/1) references keep working unchanged, while g_tracks[] binds every index.
#ifndef TDSP_VOICE2
#define TDSP_VOICE2 0
#endif
#ifndef TDSP_SYNTH_VOICES
#define TDSP_SYNTH_VOICES (TDSP_VOICE2 ? 2 : 1)
#endif
static const int kSynthVoices = TDSP_SYNTH_VOICES;

// --- Heterogeneous engine inventory (Phase 4, Thread D) ------------------------------------------
// Normally every synth voice is the SAME engine (the Dexed pool). A HETERO build declares a COUNT
// per engine kind (build flags), and the voices split by kind: voices [0, kDexedVoices) are Dexed
// pool windows; voices [kDexedVoices, kSynthVoices) are melodic OPLL engines (HeteroOpll.h). A Track
// binds to whichever — proving "N synths of DIFFERENT kinds." See planning/tracks/DESIGN.md.
#ifndef TDSP_HETERO
#define TDSP_HETERO 0
#endif
#ifndef TDSP_DEXED_VOICES
#define TDSP_DEXED_VOICES (TDSP_VOICE2 ? 2 : 1)   // Dexed pool windows (== kSynthVoices on a non-hetero build)
#endif
#ifndef TDSP_OPLL_ENGINES
#define TDSP_OPLL_ENGINES 0                       // melodic OPLL voices appended after the Dexed ones
#endif
#ifndef TDSP_PLAITS_ENGINES
#define TDSP_PLAITS_ENGINES 0                     // melodic Plaits tracks appended after the OPLL ones (HeteroPlaits.h)
#endif
#if TDSP_HETERO
static const int kDexedVoices = TDSP_DEXED_VOICES;     // tracks [0, kDexedVoices)                        = Dexed
static const int kOpllVoices  = TDSP_OPLL_ENGINES;     // tracks [kDexedVoices, +kOpllVoices)             = OPLL
// tracks [kDexedVoices + kOpllVoices, kSynthVoices) = Plaits (TDSP_PLAITS_ENGINES of them)
static_assert(TDSP_DEXED_VOICES + TDSP_OPLL_ENGINES + TDSP_PLAITS_ENGINES == TDSP_SYNTH_VOICES,
              "TDSP_SYNTH_VOICES must equal DEXED + OPLL + PLAITS engine counts");
#else
static const int kDexedVoices = kSynthVoices;          // homogeneous: every voice is Dexed (or the sole backend)
static const int kOpllVoices  = 0;
#endif
// Compile-time engine-kind predicates for a track index (per-track dispatch + inventory binding).
static inline bool voiceIsOpll(int v)   { return TDSP_HETERO && v >= kDexedVoices && v < kDexedVoices + kOpllVoices; }
static inline bool voiceIsPlaits(int v) { return TDSP_HETERO && v >= kDexedVoices + kOpllVoices; }

tdsp::MidiFilePlayer   g_playerV[kSynthVoices];   // one song player per synth voice
tdsp::MidiFilePlayer  &g_player = g_playerV[0];   // alias: voice 0 (all existing g_player refs)
tdsp::ArpFilter        g_arpFilterV[kSynthVoices];   // one arp per synth voice (bypassed by default)
tdsp::MidiRouter       g_routerV[kSynthVoices];      // one live-MIDI router per synth voice
tdsp::MidiFilePlayer   g_drumPlayer;         // dedicated LOOPING drum-groove player (channel 10)
tdsp::DrumNoteMapper   g_drumNoteMapper;     // ch10 note-map shim between g_drumPlayer and the real drum sink

// Live MIDI: a USB-host controller (LinnStrument etc.) + the DIN MIDI IN both feed
// one MPE-aware router that normalizes bend/timbre/pressure into the synth sink.
#if TDSP_HAS_USB_MIDI_HOST
USBHost                g_usbHost;
MIDIDevice             g_usbMidi(g_usbHost);
#endif
tdsp::MidiRouter      &g_router = g_routerV[0];   // alias: voice 0 live-MIDI router

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
// Note-editor clip dump/load (@RECDUMP/@RECLOAD). Ships with the recorder by default, but is a
// separable surface so a board can have the recorder without the editor (DESIGN §9.8).
#ifndef TDSP_RECORDER_EDIT
#define TDSP_RECORDER_EDIT TDSP_RECORDER
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
tdsp::MidiRouter      &g_kbdRouter = g_routerV[1];   // alias: voice 1 (keyboard) router -> (arp2 ->) g_synthSinkB
tdsp::MidiFilePlayer  &g_player2 = g_playerV[1];   // alias: voice 1's song player is g_playerV[1]
static bool            g_voice2On = false;   // runtime split enable (@VOICE2=1)
#if TDSP_ARP2
tdsp::ArpFilter       &g_arpFilter2 = g_arpFilterV[1];   // alias: voice 1 arp (keyboard/Voices-2 path)
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
#if TDSP_SYNTH_VOICES >= 3
tdsp::PlayerFollower   g_songFollow3{g_playerV[2]};   // voice index 2 (4-voice pool OR the hetero OPLL voice) retimes to the grid
#endif
#if TDSP_SYNTH_VOICES >= 4
tdsp::PlayerFollower   g_songFollow4{g_playerV[3]};
#endif
tdsp::ClockSink        g_clockSink{&g_conductor.clock()};
tdsp::ArpFilter       &g_arpFilter = g_arpFilterV[0];        // alias: voice 0 arp (live MIDI -> arp -> synth, bypass by default)
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

// The tracks: [0] = Voice 1, [1] = Voice 2 (VOICE2 builds). A thin binding view over the
// per-voice objects above + per-voice state; populated by tracksInit() in setup(). Phase 1
// only DECLARES these; nothing reads them yet (see Track.h / planning/tracks/DESIGN.md).
static const int kNumTracks = kSynthVoices;   // one Track per synth voice (1 / 2 / 4)
static Track g_tracks[kNumTracks];

AudioMixer4_F32        outL, outR;           // F32 mix: 0=BT, 1=local tone, 2=S/PDIF-in, 3=synth
#if TDSP_EXT_MIX
// External inputs collected here (DRY): 0=BT, 1=S/PDIF-in, 2=USB-in. On TDSP_EXT_MIX builds
// BT and S/PDIF move OFF outL slots 0/2 into this bus; the bus rejoins post-reverb (see the
// master-out mixer below), so external audio is never reverbed. Its slot-2 (USB) level is @USBGAIN.
AudioMixer4_F32        extL, extR;
#endif
#if TDSP_FX
// FX master insert (INSERT topology): the stereo reverb sits between the master sum
// (finalL/finalR when audioloop is on, else outL/outR) and tdmOut. Constructed AFTER tdmOut
// so tdmOut keeps update_responsibility (project_f32_update_order). State/commands: Fx.inc.h.
#if TDSP_FX_PLATE
AudioEffectPlateReverb_F32  g_fxReverb;   // rich; ~190 KB RAM2 heap
#elif TDSP_FX_SPRING
AudioEffectSpringReverb_F32 g_fxReverb;   // ~37 KB heap — fits no-PSRAM
#endif
#if TDSP_FX_SEND
// Aux SEND matrix (per-voice): each track sends into this cascaded FX-input mixer, which feeds the
// reverb (100% wet); the wet return rejoins at the master-out (post) mixer, so a track is DRY unless
// its send is raised. fxIn1 = Synth A/B/C/D sends; fxIn2 = fxIn1 + Drums + Audio-loop sends. Send
// gains start at 0 (all dry) — @TRK<i>.FXSEND raises them. See planning/plate-reverb-fx/SEND_MATRIX.md.
AudioMixer4_F32 fxIn1L, fxIn1R, fxIn2L, fxIn2R;
AudioConnection_F32 c_fxCascL(fxIn1L, 0, fxIn2L, 0);   // A/B/C/D sub-sum -> fxIn2 slot 0
AudioConnection_F32 c_fxCascR(fxIn1R, 0, fxIn2R, 0);
AudioConnection_F32 c_fxRevInL(fxIn2L, 0, g_fxReverb, 0);
AudioConnection_F32 c_fxRevInR(fxIn2R, 0, g_fxReverb, 1);
#endif
#endif
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
#if TDSP_EXT_MIX
AudioConnection_F32 c_btL    (btToF32L,   0, extL, 0);   // BT -> external sub-mix slot 0 (dry, post-reverb)
AudioConnection_F32 c_btR    (btToF32R,   0, extR, 0);
#else
AudioConnection_F32 c_btL    (btToF32L,   0, outL, 0);   // BT -> mix slot 0
AudioConnection_F32 c_btR    (btToF32R,   0, outR, 0);
#endif
#endif
#if !TDSP_METRONOME
AudioConnection_F32 c_toneL  (testTone,   0, outL, 1);   // local self-test tone on mix slot 1
AudioConnection_F32 c_toneR  (testTone,   0, outR, 1);
#endif
#if TDSP_SPDIF_IN
#if TDSP_EXT_MIX
AudioConnection_F32 c_spL    (spdifIn,    0, extL, 1);   // optical -> external sub-mix slot 1 (dry, post-reverb)
AudioConnection_F32 c_spR    (spdifIn,    1, extR, 1);
#else
AudioConnection_F32 c_spL    (spdifIn,    0, outL, 2);
AudioConnection_F32 c_spR    (spdifIn,    1, outR, 2);
#endif
#endif
#if TDSP_USB_AUDIO
AudioConnection_F32 c_usbL   (usbIn,      0, extL, 2);   // USB host audio -> external sub-mix slot 2
AudioConnection_F32 c_usbR   (usbIn,      1, extR, 2);
#endif
#if TDSP_METRONOME || TDSP_EXT_MIX || TDSP_FX_SEND
// Master-out mixer. The instrument chain reaches the DAC through slot 0; the metronome click
// (slot 1) and the external inputs (slot 2) are injected HERE — AFTER the reverb insert AND
// after the audio-loop record-bus tap — so NEITHER is reverbed nor captured in audio loops.
// The click volume is postL/postR slot-1 gain (metroApplyMute). This one mixer serves both
// the metronome and the external sub-mix; a build with just one of them uses only its slot.
AudioMixer4_F32     postL, postR;
#if TDSP_METRONOME
AudioConnection_F32 c_metL  (testTone, 0, postL, 1);   // metronome click (dry, post-reverb)
AudioConnection_F32 c_metR  (testTone, 0, postR, 1);
#endif
#if TDSP_EXT_MIX
AudioConnection_F32 c_extPL (extL,     0, postL, 2);   // external inputs (USB/optical/BT), DRY
AudioConnection_F32 c_extPR (extR,     0, postR, 2);
#endif
#if TDSP_FX_SEND
AudioConnection_F32 c_fxRetL(g_fxReverb, 0, postL, 3);   // reverb wet RETURN (post-everything); level = postL.gain(3) via @FX.RETURN
AudioConnection_F32 c_fxRetR(g_fxReverb, 1, postR, 3);
#endif
AudioConnection_F32 c_postL (postL, 0, tdmOut, 0);
AudioConnection_F32 c_postR (postR, 0, tdmOut, 1);
#define TDSP_DAC_L_NODE postL
#define TDSP_DAC_L_SLOT 0
#define TDSP_DAC_R_NODE postR
#define TDSP_DAC_R_SLOT 0
#else
#define TDSP_DAC_L_NODE tdmOut
#define TDSP_DAC_L_SLOT 0
#define TDSP_DAC_R_NODE tdmOut
#define TDSP_DAC_R_SLOT 1
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
#if TDSP_FX && !TDSP_FX_SEND
AudioConnection_F32 c_fxInL  (finalL, 0, g_fxReverb, 0);  // final mix -> reverb insert
AudioConnection_F32 c_fxInR  (finalR, 0, g_fxReverb, 1);
AudioConnection_F32 c_finOutL(g_fxReverb, 0, TDSP_DAC_L_NODE, TDSP_DAC_L_SLOT);  // reverb -> post/DAC
AudioConnection_F32 c_finOutR(g_fxReverb, 1, TDSP_DAC_R_NODE, TDSP_DAC_R_SLOT);
#define TDSP_INSTR_L_NODE g_fxReverb
#define TDSP_INSTR_L_SLOT 0
#define TDSP_INSTR_R_NODE g_fxReverb
#define TDSP_INSTR_R_SLOT 1
#else
AudioConnection_F32 c_finOutL(finalL, 0, TDSP_DAC_L_NODE, TDSP_DAC_L_SLOT);      // final mix -> post/DAC
AudioConnection_F32 c_finOutR(finalR, 0, TDSP_DAC_R_NODE, TDSP_DAC_R_SLOT);
#define TDSP_INSTR_L_NODE finalL
#define TDSP_INSTR_L_SLOT 0
#define TDSP_INSTR_R_NODE finalR
#define TDSP_INSTR_R_SLOT 0
#endif
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
#if TDSP_FX && !TDSP_FX_SEND
AudioConnection_F32 c_fxInL  (outL,       0, g_fxReverb, 0);   // master sum -> reverb insert
AudioConnection_F32 c_fxInR  (outR,       0, g_fxReverb, 1);
AudioConnection_F32 c_outL   (g_fxReverb, 0, TDSP_DAC_L_NODE, TDSP_DAC_L_SLOT);   // reverb -> post/DAC
AudioConnection_F32 c_outR   (g_fxReverb, 1, TDSP_DAC_R_NODE, TDSP_DAC_R_SLOT);
#define TDSP_INSTR_L_NODE g_fxReverb
#define TDSP_INSTR_L_SLOT 0
#define TDSP_INSTR_R_NODE g_fxReverb
#define TDSP_INSTR_R_SLOT 1
#else
AudioConnection_F32 c_outL   (outL,       0, TDSP_DAC_L_NODE, TDSP_DAC_L_SLOT);
AudioConnection_F32 c_outR   (outR,       0, TDSP_DAC_R_NODE, TDSP_DAC_R_SLOT);
#define TDSP_INSTR_L_NODE outL
#define TDSP_INSTR_L_SLOT 0
#define TDSP_INSTR_R_NODE outR
#define TDSP_INSTR_R_SLOT 0
#endif
#endif   // TDSP_AUDIOLOOP
#if TDSP_USB_AUDIO
// USB record send: the host records the INSTRUMENT mix (synths/drums, incl. reverb) — tapped
// BEFORE the external-input sum (TDSP_INSTR_* = whatever feeds the DAC), so the computer's own
// audio is NOT echoed back to it. Fan-out from the same source that drives the master out.
AudioConnection_F32 c_usbOutL(TDSP_INSTR_L_NODE, TDSP_INSTR_L_SLOT, usbOut, 0);
AudioConnection_F32 c_usbOutR(TDSP_INSTR_R_NODE, TDSP_INSTR_R_SLOT, usbOut, 1);
#endif
#if TDSP_ROLE_BT_RECEIVER
AudioConnection_F32 c_pkBt   (btToF32L,   0, peakBt,    0);
#endif
#if TDSP_SPDIF_IN
AudioConnection_F32 c_pkSp   (spdifIn,    0, peakSpdif, 0);
#endif
AudioConnection_F32 c_pkOut  (outL,       0, peakOut,   0);

#if TDSP_FX
// FX state + @FX.* command surface + "fx" @STATE object (g_fxReverb declared above).
#include "Fx.inc.h"
#endif

// --- Development output capture (build-agnostic) -----------------------------
// A capture-only probe on the FINAL digital output (same tap as peakOut / the DAC).
// `@CAP[=<n>]` arms it, records the next samples of the actual DAC-bound signal into a
// DMAMEM buffer, and dumps them over USB serial for tools/capture_analyze.py (FFT /
// spectral-centroid "brightness" / RMS / WAV / plot). It taps the output BUS, not any
// engine, so it works on every build now and in the future — a permanent dev instrument.
#if defined(TDSP_LEAN_RAM) && !defined(TDSP_CAP_FULL)
DMAMEM static float g_capBuf[64];       // @CAP capture stubbed on lean-RAM builds (frees ~64 KB OCRAM for the SD-song heap)
#else
DMAMEM static float g_capBuf[16384];    // full buffer: non-lean builds, or a lean build that sets -D TDSP_CAP_FULL (e.g. the board-test env)
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
#if (defined(TDSP_DRUM_TSF) + defined(TDSP_DRUM_VOICE) + defined(TDSP_DRUM_PLAITS) + defined(TDSP_DRUM_SD)) > 1
  #error "TDSP_DRUM_TSF / TDSP_DRUM_VOICE / TDSP_DRUM_PLAITS / TDSP_DRUM_SD are mutually exclusive — they all own the drum mix slot; pick one drum voice."
#endif
#ifdef TDSP_DRUM_TSF
  #include "DrumTsf.h"      // sampled full GM kit; needs PSRAM (resident font)
#endif
#ifdef TDSP_DRUM_VOICE
  #include "DrumVoice.h"    // OPLL 5-sound rhythm; ~9 KB, no PSRAM
#endif
#ifdef TDSP_DRUM_PLAITS
  #include "DrumPlaits.h"   // authentic Plaits drum models (13/14/15) as a small pool; no PSRAM
#endif
#ifdef TDSP_DRUM_SD
  #include "DrumSampler.h"  // SD-streaming one-shot kit (8 AudioPlaySdResmp voices); no PSRAM
#endif
#if TDSP_OPLL_ENGINES >= 1
  #include "HeteroOpll.h"   // Thread D: melodic OPLL voice(s) ALONGSIDE the primary (heterogeneous inventory)
                            // Gated on OPLL_ENGINES (not just HETERO): a hetero inventory may have ZERO
                            // hetero-OPLL voices (e.g. a 0-Dexed OPLL-primary + Plaits build).
#endif
#if TDSP_HETERO_PLAITS
  #include "HeteroPlaits.h" // a melodic Plaits track ALONGSIDE the others (generalized inventory)
#endif

#if TDSP_FX_SEND
// Per-voice SEND taps -> the FX-input cascade (declared with g_fxReverb above). Each tap taps a
// track's PRE-master-sum node (so it can be sent independently of its dry level); the SEND LEVEL is
// the fxIn mixer's per-slot gain (0 = dry, set by @TRK<i>.FXSEND / fxSendApply). Dexed + OPLL engines
// are mono -> feed both fxIn L and R. Guarded per source so a build only wires the sends it has.
#if defined(TDSP_SYNTH_DEXED_POOL)
AudioConnection_F32 c_sendAl(dxpTrim,          0, fxIn1L, 0), c_sendAr(dxpTrim,          0, fxIn1R, 0);   // Synth A (Dexed engines 0-3)
#if TDSP_VOICE2
AudioConnection_F32 c_sendBl(dxpTrimB,         0, fxIn1L, 1), c_sendBr(dxpTrimB,         0, fxIn1R, 1);   // Synth B (Dexed engines 4-7)
#endif
#endif
#if TDSP_OPLL_ENGINES >= 1
AudioConnection_F32 c_sendCl(g_hoOpllTrim[0],  0, fxIn1L, 2), c_sendCr(g_hoOpllTrim[0],  0, fxIn1R, 2);   // Synth C (OPLL 0)
#if TDSP_OPLL_ENGINES >= 2
AudioConnection_F32 c_sendDl(g_hoOpllTrim[1],  0, fxIn1L, 3), c_sendDr(g_hoOpllTrim[1],  0, fxIn1R, 3);   // Synth D (OPLL 1)
#endif
#endif
#if TDSP_DRUM_TSF
AudioConnection_F32 c_sendDrl(g_drumTsfToF32L, 0, fxIn2L, 1), c_sendDrr(g_drumTsfToF32R, 0, fxIn2R, 1);   // Drums (TSF, stereo)
#elif TDSP_DRUM_VOICE
AudioConnection_F32 c_sendDrl(g_drumVoiceToF32, 0, fxIn2L, 1), c_sendDrr(g_drumVoiceToF32, 0, fxIn2R, 1);  // Drums (OPLL rhythm, mono -> L+R)
#elif TDSP_DRUM_PLAITS
AudioConnection_F32 c_sendDrl(g_drumPlaitsToF32, 0, fxIn2L, 1), c_sendDrr(g_drumPlaitsToF32, 0, fxIn2R, 1); // Drums (Plaits pool, mono -> L+R)
#elif TDSP_DRUM_SD
AudioConnection_F32 c_sendDrl(g_drumSdToF32L, 0, fxIn2L, 1), c_sendDrr(g_drumSdToF32R, 0, fxIn2R, 1);       // Drums (SD sampler, stereo)
#endif
#if TDSP_AUDIOLOOP
AudioConnection_F32 c_sendLpl(g_aloop[0],      0, fxIn2L, 2), c_sendLpr(g_aloop[0],      1, fxIn2R, 2);   // Audio loop 0
#endif
#endif  // TDSP_FX_SEND

#include "CatalogDb.h"   // on-demand /tdsp/ catalog database (@REINDEX); client-triggered
// Parallel drum-voice label for the catalog header (a melodic synth + a dedicated drum engine).
#if defined(TDSP_DRUM_VOICE)
  static const char *kDrumEngineName = "OPLL";
#elif defined(TDSP_DRUM_TSF)
  static const char *kDrumEngineName = "TSF";
#elif defined(TDSP_DRUM_PLAITS)
  static const char *kDrumEngineName = "Plaits";
#elif defined(TDSP_DRUM_SD)
  static const char *kDrumEngineName = "Sampler";
#else
  static const char *kDrumEngineName = "";   // GM engines render their own ch10 drums
#endif
// Does picking a "kit" actually change the drum sound? GM sampled engines (a TSF drum font, or a GM
// synth rendering ch10) switch kit via a channel-10 program change. The OPLL rhythm voice (DrumVoice.h)
// has a FIXED 5-sound set (BD / SD / TOM / TOP-CYM / HH) and IGNORES the program (see drumApplyKit) —
// so the app must not offer GM kits there. Reported to the app as caps.drumkitsel.
#if defined(TDSP_DRUM_VOICE) || defined(TDSP_DRUM_PLAITS)
  static const bool kDrumKitSelectable = false;   // OPLL rhythm / Plaits models: fixed sounds; @DRUMKIT is a no-op
#else
  static const bool kDrumKitSelectable = true;    // TSF / GM: kit = a ch10 program change
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
// Anti-zipper glide for the master DAC gain: @VOL / the +/- keys set the TARGET, and masterVolTick()
// (from loop(), self-gated ~1 kHz) glides tdmOut.setGain toward it — so a fast volume slider drag
// fades smoothly instead of stepping. Boot fades up from 0 (cur starts at 0): a clean soft start.
static float g_masterGainTarget = 0.0f, g_masterGainCur = 0.0f;
static void applyAppMaster() { g_masterGainTarget = appPctToGain(g_appMasterPct); }
static void masterVolTick() {
    static elapsedMicros t;
    if (t < 1000) return;                                                    // ~1 kHz glide update
    t = 0;
    if (fabsf(g_masterGainCur - g_masterGainTarget) > 0.0002f) {
        g_masterGainCur += (g_masterGainTarget - g_masterGainCur) * 0.15f;   // one-pole, ~30 ms settle
        tdmOut.setGain(g_masterGainCur);
    }
}

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
#if !defined(TDSP_DRUM_TSF) && !defined(TDSP_DRUM_VOICE) && !defined(TDSP_DRUM_SD)
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

// Metronome (opt-in: -D TDSP_METRONOME). The metronome is now the AUDIBLE FACE of the master
// transport: the click is driven straight off the Conductor clock (g_conductor), the same grid
// the song players, drums, and arp lock to — NOT a private accumulator — so it can never disagree
// with them. It is MUTED by default: the transport (clock) runs whenever you press Play, but you
// only hear the click after unmuting (@METROMUTE=0). The accent lands on the shared bar's downbeat
// (clk.beatInBar()==0). Slot-free: reuses the local test-tone oscillator (testTone, mix slot 1).
// The @BEAT light feed comes from beatEmitPoll() (the single clock-driven emitter), not from here.
#ifdef TDSP_METRONOME
static bool          g_metroMuted   = true;  // is the click AUDIBLE? default MUTED (transport runs silently)
static float         g_metroPeak    = 0.0f;  // current click's peak amplitude (0 = idle)
static elapsedMillis g_metroAge;             // ms since the current click fired
static uint32_t      g_metroLastBeat = 0xFFFFFFFFu;   // beatCount the click last struck on
static constexpr float    kMetroGain      = 0.9f;    // mix-slot-1 gain while unmuted
static constexpr float    kMetroAccentAmp = 0.85f;   // beat 1 (downbeat) click level
static constexpr float    kMetroBeatAmp   = 0.30f;   // other beats (well below the accent)
static constexpr float    kMetroAccentHz  = 2093.0f; // C7 — accent pitch (an octave over the beat)
static constexpr float    kMetroBeatHz    = 1047.0f; // C6 — normal pitch
static constexpr float    kMetroDecayMs   = 45.0f;   // percussive click decay
static int           g_metroVolPct = 100;  // click level 0..150 (% of the default gain), independent of @VOL master
static float metroSlotGain() { return kMetroGain * (g_metroVolPct / 100.0f); }   // slot-1 gain scaled by the volume

// Open/close the click's audio slot (mix slot 1, shared with the test tone) to match the mute
// state. When muted, also kill any decaying click so it goes silent immediately.
static void metroApplyMute() {
    const float g = g_metroMuted ? 0.0f : metroSlotGain();
    postL.gain(1, g); postR.gain(1, g);   // metronome level on the POST mixer (post-reverb, post-loop-tap)
    if (g_metroMuted) { g_metroPeak = 0.0f; testTone.amplitude(0.0f); }
}
static void metroSetMuted(bool m) { g_metroMuted = m; metroApplyMute(); }

// Call once per loop(). The click follows the MASTER clock: on each new beat, while the transport
// runs and the click is unmuted, strike (accent on the bar downbeat). No private timebase — the
// click is sample-aligned with whatever the players/drums/arp are doing.
static void metroPoll() {
    tdsp::Clock &clk = g_conductor.clock();
    if (clk.running() && !g_metroMuted) {
        const uint32_t bc = clk.beatCount();
        if (bc != g_metroLastBeat) {                          // a new beat just happened
            g_metroLastBeat = bc;
            const bool accent = (clk.beatInBar() == 0);        // beat 1 of the shared bar
            testTone.frequency(accent ? kMetroAccentHz : kMetroBeatHz);
            g_metroPeak = accent ? kMetroAccentAmp : kMetroBeatAmp;
            g_metroAge  = 0;
        }
    } else {
        g_metroLastBeat = 0xFFFFFFFFu;   // re-arm so the next running & unmuted beat clicks
    }
    if (g_metroPeak > 0.0f) {                                 // percussive linear decay
        float a = g_metroPeak * (1.0f - (float)g_metroAge / kMetroDecayMs);
        if (a <= 0.001f) { a = 0.0f; g_metroPeak = 0.0f; }
        testTone.amplitude(a);
    }
}

// Metronome click level (0..150 %). Scales slot-1 gain; applied live when unmuted.
static void setMetroVol(int pct) {
    if (pct < 0) pct = 0; if (pct > 150) pct = 150;
    g_metroVolPct = pct;
    if (!g_metroMuted) { outL.gain(1, metroSlotGain()); outR.gain(1, metroSlotGain()); }
}
#endif  // TDSP_METRONOME

// --- Beat position emit (@BEAT) ---------------------------------------------
// The SINGLE @BEAT source: drives the app's visual beat lights off the master clock whenever
// the transport runs. While a song/groove plays it reflects the content's grid (real downbeat +
// meter); while idle-but-running it uses the @METROSIG time signature (clk.beatsPerBar() falls
// back to g_metroBpb via applyMeter). The audible click (metroPoll) reads the same clock but
// emits no @BEAT, so the two can't fight. Watches beatCount() change (does NOT consume the beat
// latch) and emits non-blocking via emitBeat(), so the light feed can never stall loop().
static uint32_t g_lastBeatEmit = 0xFFFFFFFFu;   // force an emit on the first beat seen
static void beatEmitPoll() {
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

// Songs live on the SD card (/midi/songs/**.mid) plus a handful of baked demo/test
// sequences in flash. Nothing but the ONE currently-playing song is held in RAM
// (parsed into g_buf on play) — exactly like the drum grooves. The browsable list is
// the catalog (/tdsp/songs.ndjson, built by @REINDEX); the app plays a song by NAME
// via @SONGF (an SD filename, or a built-in's display name). There is NO fixed-size
// registry and therefore NO song-count cap.
//   built-in test seq : baked MidiFileEvent[] (mev), flips MPE mode via `mpe`
//   built-in demo     : baked legacy SongEv[] (ev), tempo estimate
//   SD song           : /midi/songs/<path>.mid, parsed on play (real tempo)
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
#if TDSP_SYNTH_VOICES >= 3
// Voices 3/4 (and the hetero OPLL voice at index 2) each need their OWN event buffer (the player
// holds a pointer, no copy). These are the EXTRA voices — primarily live-played (a keyboard on
// voice 2/3), so their song player is secondary. Keep the buffers SMALL: g_buf(24000)+g_buf2(12000)+
// two big buffers here would fill OCRAM and starve the lean-RAM SD-song heap (~82 KB) → boot crash.
// A short backing loop fits 2000 events; a longer song truncates on voices 3/4 only (voices 0/1
// keep their full buffers).
static const int MAX_EVENTS3 = 2000;
DMAMEM static tdsp::MidiFileEvent g_buf3[MAX_EVENTS3];
#if TDSP_SYNTH_VOICES >= 4
DMAMEM static tdsp::MidiFileEvent g_buf4[MAX_EVENTS3];
#endif
#endif

static bool endsWithMid(const char *s) {
    size_t n = strlen(s);
    return n > 4 && strcasecmp(s + n - 4, ".mid") == 0;
}
// Generic case-insensitive ".<ext>" suffix test for the @LS lister. `ext` carries NO dot
// (e.g. "mid"); an empty/null ext matches every file. Sibling of endsWithMid.
static bool endsWithExt(const char *name, const char *ext) {
    if (!ext || !*ext) return true;
    size_t n = strlen(name), e = strlen(ext);
    if (n < e + 1 || name[n - e - 1] != '.') return false;
    return strcasecmp(name + n - e, ext) == 0;
}
// Display name for a song path: take the BASENAME (strip any directory) then drop a
// trailing ".mid". Handles both a bare filename and a full "/midi/songs/Foo.mid" path.
static void songDisp(char *out, size_t n, const char *fname) {
    const char *base = strrchr(fname, '/');
    base = base ? base + 1 : fname;
    size_t c = strlen(base);
    if (c > 4 && strcasecmp(base + c - 4, ".mid") == 0) c -= 4;
    if (c > n - 1) c = n - 1;
    memcpy(out, base, c); out[c] = 0;
}
// True if an SD .mid with this display name exists (so a baked built-in defers to the
// tempo-bearing SD copy). Checked against the card, not a RAM list.
static bool sdSongExists(const char *disp) {
    if (!::g_sdReady) return false;
    char p[128];
    snprintf(p, sizeof p, "/midi/songs/%s.mid", disp); return SD.exists(p);
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
// Write a directory tree's *.mid rows to songs.ndjson ({name, file}). RECURSIVE: walks
// subdirs so /midi/songs/<genre>/Foo.mid is indexed too. `file` is the FULL SD path
// (e.g. "/midi/songs/Foo.mid") so @SONG=<index>/trackPreload resolve it verbatim; `name`
// is the basename display. Used by the catalog builder — a direct SD scan, uncapped.
static void writeSongDir(Print &so, const char *dir) {
    File d = SD.open(dir);
    if (!d || !d.isDirectory()) { if (d) d.close(); return; }
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
        const char *nm = f.name();
        if (nm && nm[0] != '.') {
            char full[160]; snprintf(full, sizeof full, "%s/%s", dir, nm);
            if (f.isDirectory()) { f.close(); writeSongDir(so, full); continue; }   // recurse into subfolders
            if (endsWithMid(nm)) {
                char disp[64]; songDisp(disp, sizeof disp, nm);
                so.print("{\"name\":"); tdsp::catdb::jsonStr(so, disp);
                so.print(",\"file\":"); tdsp::catdb::jsonStr(so, full); so.print("}\n");
            }
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
#if TDSP_SYNTH_VOICES >= 3
// Voice index 2 (the 4-voice pool's voice 3, OR the hetero OPLL voice): an independent song
// player -> its own name/arg/loop/tempo/meter/sync/launch state (mirrors the voice-1/2 slots).
// Bound to tracks[2] in tracksInit.
static char   g_curSong3Name[64] = "";
static char   g_curSong3Arg[100] = "";
static bool   g_song3Loop = false;
static bool   g_song3WasPlaying = false;
static float  g_song3Bpm = 120.0f;
static uint8_t g_song3Bpb = 4;
static double g_song3LoopBeats = 0.0;
static bool   g_song3LaunchPending = false;
#endif
#if TDSP_SYNTH_VOICES >= 4
// Voice index 3 (4-voice pool's voice 4).
static char   g_curSong4Name[64] = "";
static char   g_curSong4Arg[100] = "";
static bool   g_song4Loop = false;
static bool   g_song4WasPlaying = false;
static float  g_song4Bpm = 120.0f;
static uint8_t g_song4Bpb = 4;
static double g_song4LoopBeats = 0.0;
static bool   g_song4LaunchPending = false;
#endif
static bool          g_syncProbe = false;   // @SYNCPROBE: 1 Hz drift probe (PLAN §9)
static elapsedMillis g_syncProbeClock;      // throttle for the probe print

// @DRUMJIT: drum-timing jitter probe. Measures how late each drum note-ON fires vs its
// scheduled beat (dispatch lateness = loop()-polling latency + loop() stalls) plus the
// worst loop()-to-loop() gap. Emits a @JIT line once/second while enabled. See
// tools/drum_jitter_test.py + planning docs on the audio-locked sequencer.
static bool          g_jitProbe = false;
static elapsedMillis g_jitClock;            // 1 Hz emit throttle
static uint32_t      g_lastLoopUs = 0;      // for loop()-to-loop() gap
static uint32_t      g_maxLoopGapUs = 0;    // worst gap in the current 1 s window

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
static void trackEnginesSetMpe(bool mpe);   // defined in TrackEngineImpl.inc.h; MPE toggle across ALL track engines
static void drumApplyKit();            // defined below; the drum Track's prep applies the GM kit
static void muteSongDrums(bool mute);  // defined below; drum-track start/stop mutes the song's ch10
static bool drumEngineOk();            // defined below; does the active engine render ch10 drums?
static void trackLaunch(Track &t, const char *arg);   // defined below; launch-quantize-aware start (drum uses it)

// Drum controls. g_drumSel / g_drumKit are used by the drum section further below.
static int  g_drumSel      = 0;     // selected / currently-playing groove index
static int  g_drumKit      = 0;     // index into kDrumKits ("instrument")
static int  g_drumVolPct   = 100;   // drum level 0..150 (% of file velocity)
static int  g_songVolPct   = 100;   // MIDI-player level 0..150 (% of file velocity), independent of @VOL master
static bool g_drumSynchro  = false; // SYNCHRO START (PSS-140 style): groove starts on your first note
static bool g_engineHasDrums = false;// engine renders ch10 (captured once at setup; not the live mask)
#if defined(TDSP_DRUM_TSF)
static int  g_drumFontCount  = 0;   // # of swappable drum fonts on the card (cached at boot); >1 -> caps.drumfontsel
#endif

// LAUNCH QUANTIZE (opt-in, default off): defer a song/groove START to the next bar edge of
// the free-running master clock — the SAME grid the arp already steps on — so songs, grooves
// and the arp all share one downbeat and stay locked. A launch only ever waits ≤ 1 bar. The
// per-loop restart of a looping song is NOT quantized (it re-arms via trackStartArg directly),
// so a loop never grows a bar-long gap. This aligns to the free grid without re-zeroing it.
static bool g_launchQuantize   = false;
static bool g_songLaunchPending = false;   // player 1 (@SONGF/@SONGRESTART) armed for the next bar
static bool g_drumLaunchPending = false;   // drum groove armed for the next bar (g_drumTrack.launchPending)
#if TDSP_VOICE2
static bool g_song2LaunchPending = false;   // player 2 (@SONG2) armed for the next bar (see trackRestart)
#endif
// A pending song launch (either voice) now stashes its PRELOADED stream in the Track (t.preEv/…),
// not a filename to re-parse on the beat: the (blocking) SD parse runs when the launch is ARMED, so
// firing on the bar edge is just play()+sync and never stalls loop() when players need their downbeat
// note (that stall was dropping notes). See trackPreload()/trackFire().
// Set true just before a bar-quantized launch fires, so songApplySync/song2ApplySync anchor the
// player at the downbeat FROM ITS TOP (never re-zeroing the shared clock under a running player).
static bool g_syncAnchorNow = false;
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
// Tempo auto-follow lock (the metronome's lock-icon button; default OFF). When OFF, the FIRST piece
// of content to start on an otherwise-empty transport (song / loop / groove) snaps the master BPM to
// its own native tempo — the box plays at the content's authored feel. Once anything else is already
// playing (a second source keeps the established grid) OR the lock is ON, the tempo is held. @METROLOCK.
static bool          g_tempoLock     = false;
static float         g_songBpm       = 120.0f;  // playing/last song NATIVE tempo
static float         g_drumFileBpm   = 120.0f;  // selected groove's NATIVE tempo
static uint8_t       g_songBpb       = 4;       // playing/last song's beats-per-bar (quarter beats)
static uint8_t       g_drumBpb       = 4;       // selected groove's beats-per-bar
static double        g_songLoopBeats = 0.0;     // playing/last song's exact loop length (quarter beats); 0 = unknown
static double        g_drumLoopBeats = 0.0;     // selected groove's exact loop length (quarter beats); 0 = unknown
static elapsedMillis g_songBarClock;            // ms since the playing song's beat 1
static bool          g_drumArmed     = false;   // SYNCHRO: groove preloaded (g_drumTrack), waiting for the first live note

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

// Push the current master BPM to the app so its tempo readout tracks a firmware-side change (the
// auto-follow snap, below). The app parses an incoming "@BPM=<n>" line into its BPM display; @BEAT/
// @SONGP already prove firmware->app pushes relay over both USB and BLE. App-initiated @BPM= changes
// don't need this echo (the app already knows the value it sent), so only the auto-follow path emits.
static void emitMasterBpm() { Serial.printf("@BPM=%d\n", (int)(g_masterBpm + 0.5f)); }

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
// is set, i.e. the user explicitly asked to restart the song from the top (see trackRestart).
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
// True when SOMETHING already holds the master grid — any song player, the drum groove, or a
// live/looping recorder loop. This is the "is anything else playing" test shared by the transport
// zero (the first starter defines the downbeat) and the tempo auto-follow (only the SOLE content
// snaps the master BPM; a second source keeps the established tempo). A bare metronome click is NOT
// content — it just ticks the shared clock, so it does not count here (the lock is the way to pin a
// hand-dialled tempo). Call BEFORE the newly-firing track's play(), so it reflects only the OTHERS.
static bool transportHasContent() {
    // EVERY synth voice's player holds the grid, not just voice 0/1 — the hetero OPLL/Plaits tracks
    // (voices 2+) count too. Missing them re-zeroed the conductor when a new synth started while an
    // extra voice was looping, so the looping voice stalled until the clock circled back (the "Plaits
    // freezes for N measures" bug). g_playerV[] spans all voices (g_player==[0], g_player2==[1]).
    bool any = g_drumPlayer.isPlaying();
    for (int v = 0; v < kSynthVoices; v++) if (g_playerV[v].isPlaying()) { any = true; break; }
#if TDSP_RECORDER
    // A running loop holds the grid too. Without this, arming synth B's recorder while only synth
    // A's loop is going looks "idle" and re-zeroes the clock — restarting A's loop. Each recorder
    // anchors to the bar of its own first note, so the two loops may sit out of phase with each
    // other; that's fine and intended. They still share the one bar grid.
    any = any || loopHoldsGrid(g_loop1);
#if TDSP_VOICE2
    any = any || loopHoldsGrid(g_loop2);
#endif
#endif
    return any;
}
static void ensureTransportStarted() {
    if (g_forceTransportZero || !transportHasContent()) {
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
    if (startClick && g_metroMuted && !g_player.isPlaying() && !g_drumPlayer.isPlaying()) {
        metroSetMuted(false);     // count-in: temporarily un-mute the click (transport is now running)
        g_recClickAuto = true;    // remember WE un-muted it, so we may re-mute after capture
    }
#endif
    (void)startClick;
}

// Auto-stop the count-in click the instant the loop finishes recording (state -> Playing),
// but only if we started it — never kill a click the user turned on themselves. Call from loop().
static void recPollClick() {
#ifdef TDSP_METRONOME
    if (g_recClickAuto && !recFreshCapturing()) {
        metroSetMuted(true);      // re-mute the count-in click we un-muted (never touch a user un-mute)
        g_recClickAuto = false;
    }
#endif
}

// --- Load a loop .mid off SD straight into a voice's MidiLooper clip -----------
// The file complement of the @RECLOAD stream path (which sources an edited clip
// as base64 bytes). Here the source is a Standard MIDI File on the card — the
// short melody/bass loops tools/fetch_loops.py drops in /midi/loops. We parse the
// raw SMF ticks and map each note onto the looper's 24-PPQN clip grid, then feed
// the SAME beginClipLoad/pushLoadEvent/endClipLoad hooks the recorder uses. So a
// "selected loop" plays through the track's OWN synth via its existing looper —
// no new track type, no new player (see project_midi_loops). Note on/off only:
// the fetched loops are single melodic/bass lines, and program/expression are
// intentionally dropped so the loop renders on the track's current patch.
// Runs from the command handler (main loop) only — SD I/O + malloc, never the ISR.
static bool loadLoopFileIntoLooper(const char *path, tdsp::MidiLooper *L, bool play) {
    if (!L) return false;
    const tdsp::MidiLooper::State st = L->state();
    if (st == tdsp::MidiLooper::Armed || st == tdsp::MidiLooper::Recording ||
        st == tdsp::MidiLooper::Overdub)
        return false;                                  // never mutate a clip mid-capture

    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    size_t len = f.size();
    if (len < 14 || len > 128UL * 1024) { f.close(); return false; }   // loops are tiny
    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf) { f.close(); return false; }
    size_t got = f.read(buf, len);
    f.close();
    if (got != len || memcmp(buf, "MThd", 4) != 0) { free(buf); return false; }

    uint16_t div = ((uint16_t)buf[12] << 8) | buf[13];
    if (div == 0 || (div & 0x8000)) { free(buf); return false; }        // 0/SMPTE division
    const double loopBeats = tdsp::smf::initialLoopBeats(buf, len);     // exact end-of-track beats
    uint8_t bpb = tdsp::smf::initialBeatsPerBar(buf, len);
    if (bpb == 0) bpb = 4;
    if (loopBeats <= 0.0) { free(buf); return false; }
    const uint32_t loopTicks = (uint32_t)(loopBeats * 24.0 + 0.5);      // 24-PPQN clip length
    if (loopTicks == 0 || loopTicks > 0xFFFF) { free(buf); return false; }
    uint8_t bars = (uint8_t)(loopBeats / (double)bpb + 0.5);            // cosmetic; loopTicks rules
    if (bars != 1 && bars != 2 && bars != 4 && bars != 8) bars = 4;

    L->beginClipLoad((uint16_t)loopTicks, bpb, bars);

    // Walk every MTrk, mapping raw absolute ticks onto the 24-PPQN grid. pushLoadEvent()
    // (LoopClip::insert) keeps the clip sorted, so tracks can be emitted independently.
    int emitted = 0; bool full = false;
    for (size_t p = 14; p + 8 <= len && !full; ) {
        if (memcmp(buf + p, "MTrk", 4) != 0) break;
        uint32_t tl = ((uint32_t)buf[p + 4] << 24) | ((uint32_t)buf[p + 5] << 16) |
                      ((uint32_t)buf[p + 6] << 8) | buf[p + 7];
        size_t i = p + 8, e = i + tl; if (e > len) e = len;
        uint32_t tick = 0; uint8_t status = 0;
        while (i < e) {
            tick += tdsp::smf::readVar(buf, e, &i); if (i >= e) break;
            uint8_t b0 = buf[i]; if (b0 & 0x80) { status = b0; i++; }
            if (status == 0xFF) { if (i >= e) break; i++; uint32_t ml = tdsp::smf::readVar(buf, e, &i); i += ml; continue; }
            if (status == 0xF0 || status == 0xF7) { uint32_t sl = tdsp::smf::readVar(buf, e, &i); i += sl; continue; }
            uint8_t hi = status & 0xF0, ch = status & 0x0F;
            if (hi == 0xC0 || hi == 0xD0) { if (i < e) i += 1; continue; }   // 1-byte msgs: skip
            if (i + 2 > e) break;
            uint8_t n = buf[i], vel = buf[i + 1]; i += 2;
            if (hi != 0x90 && hi != 0x80) continue;    // only note on/off (drop CC/bend for v1)
            if (ch == 9) continue;                     // skip any drum channel (melodic loops)
            if (n == 0) continue;                      // skip the fetcher's silent barline marker
            uint32_t t24 = (uint32_t)((double)tick * 24.0 / (double)div + 0.5);
            if (t24 >= loopTicks) t24 = loopTicks - 1; // clamp a seam note-off inside [0,loopTicks)
            tdsp::LoopEvent ev;
            ev.tick = (uint16_t)t24; ev.channel = 1; ev.d1 = n; ev.d2 = vel; ev.bend = 0;
            ev.type = (hi == 0x90 && vel > 0) ? tdsp::LNoteOn : tdsp::LNoteOff;
            if (!L->pushLoadEvent(ev)) { full = true; break; }   // clip full (>=1024 events)
            emitted++;
        }
        p = e;
    }
    free(buf);
    if (emitted == 0) { L->endClipLoad(false); return false; }
    if (play) recArmTransport(/*startClick=*/false);   // make sure the beat grid is running
    L->endClipLoad(play);
    return true;
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
// Grid-lock a track's player to the master clock (unified — replaces songApplySync/song2ApplySync).
// ALWAYS syncs, looping or not, so all players + drums share one phase-aligned, drift-free grid.
// loopBeats = exact loop length (SD parse) else derived from the stream's ms at native tempo; a
// non-looping song plays through once and stops at that boundary (tickSynced honours loop_). No
// usable length -> falls back to the ms engine.
static void songApplySync(Track &t, double parsedLoopBeats) {
    *t.loopBeats = 0.0;
    double lb = parsedLoopBeats;
    if (lb <= 0.0)                               // baked/full stream (no loop meta): derive from ms
        lb = tdsp::smf::snapLoopBeatsHalf((double)t.player->totalMs() * (double)*t.bpm / 60000.0);
    if (lb <= 0.0) return;
    *t.loopBeats = lb;
    const bool anchorNow = g_syncAnchorNow; g_syncAnchorNow = false;
    t.player->setSyncedMode(&g_conductor.clock(), lb, *t.bpm, anchorNow);
    Serial.printf("[song] grid-locked: len=%.2f beats @ %.1f bpm (%s%s)\n", lb, (double)*t.bpm, *t.loop ? "loop" : "one-shot", anchorNow ? ", from top" : "");
}

// Clean slate before starting ANY song: silence sounding notes + clear latched
// per-engine expression (bend / mod / aftertouch), so a bend left mid-glide by the
// previous song can't carry over. Spare channel 10 while a drum groove is looping —
// an all-channels reset would cut the drums for a beat when you press Play.
// Clean slate before a track starts a song (unified — replaces songPrep/song2Prep). Voice 1
// (caps.prepSpecial) shares its sink with the ch10 drum groove so it spares ch10, and resets the
// multitimbral audition trim (a song is multitimbral; the last-picker trim no longer describes it).
// Voice 2 has a private sink -> a bare all-notes-off.
static void songPrep(Track &t) {
    if (t.caps.appliesKit) {                    // drum track: own patch = the GM kit; NO note panic
        drumApplyKit();                         // (an all-notes-off here would cut the melodic voice on a shared sink)
        t.player->setVelocityScale(g_drumVolPct / 100.0f);   // per-note drum level (shared-sink lever)
        return;
    }
    if (t.caps.prepSpecial && g_drumPlayer.isPlaying()) {
        for (uint8_t ch = 1; ch <= 16; ++ch) if (ch != 10) t.sink->onAllNotesOff(ch);
    } else {
        t.sink->onAllNotesOff(0);
    }
#ifdef TDSP_REPLAYGAIN_MULTITIMBRAL
    if (t.caps.prepSpecial) synthAuditionTrim()->setGain(1.0f);
#endif
}

// Both per-voice name/arg buffers are the same size; the unified helpers write through the Track's
// name/arg pointers with these caps (g_curSong{,2}Name are [64], g_curSong{,2}Arg are [100]).
static constexpr size_t kSongNameCap = sizeof g_curSongName;
static constexpr size_t kSongArgCap  = sizeof g_curSongArg;

// --- Unified START path (P1.4): one preload -> fire Track& path for EVERY voice ----------------
// Replaces the old song*/song2* start families. LOAD (the slow, blocking part) runs in trackPreload;
// trackFire is fast play()+sync so it can land ON a bar downbeat without stalling loop() (that stall
// dropped the downbeat note — the whole reason voice 2 preloaded). Folding voice 1 onto this pattern
// is the deliberate P1.4 improvement: voice 1's start no longer parses SD on the beat. The three
// voice-1-only side effects (global MIDI/MPE mode, meter ownership, the special prep) are caps-gated.

// trackPreload: load `arg` into the track's stash (t.preEv/preCount/preLoopBeats + how to set the
// device mode on fire) with NO sink/clock side effects — safe to call while other players run, off
// the downbeat. `arg` ending in ".mid" is an SD file in /songs (or the card root); otherwise a baked
// built-in (test sequence or legacy demo) by display name. Returns false if not found / load failed.
FLASHMEM static bool trackPreload(Track &t, const char *arg) {
    if (!arg || !*arg) return false;
    if (t.caps.drumGated && !drumEngineOk()) {   // a groove needs a channel-10-capable engine
        Serial.printf("[%s] %s has no channel-10 drum map — use TSF/SF2/OPL3/OPLL\n", t.tag, synthName());
        return false;
    }
    *t.bpm = 120.0f; *t.bpb = 4;
    t.preEv = nullptr; t.preCount = 0; t.preLoopBeats = 0.0;
    t.preForceMode = false; t.preMpe = false;   // default (SD / legacy): force normal MIDI only if currently MPE
    if (endsWithMid(arg)) {
        // Hard cut to /midi paths: the app sends a full SD path ("/midi/songs/Foo.mid" or
        // "/midi/drums/Groove.mid" for the drum track). A leading '/' is used verbatim; a bare
        // name is rooted at '/' (no /songs fallback).
        char path[160]; char disp[64]; songDisp(disp, sizeof disp, arg);
        if (arg[0] == '/') snprintf(path, sizeof path, "%s", arg);
        else               snprintf(path, sizeof path, "/%s", arg);
        double plb = 0.0;
        int got = tdsp::smf::loadSmfFile(path, t.buf, t.bufCap, t.bpm, t.bpb, &plb);   // + exact loop length
        if (got <= 0) { Serial.printf("[%s] SD load FAILED: %s\n", t.tag, path); return false; }
        snprintf(t.name, kSongNameCap, "%s", disp);
        snprintf(t.arg,  kSongArgCap,  "%s", arg);
        t.preEv = t.buf; t.preCount = (uint32_t)got; t.preLoopBeats = plb;
        // The "[song] … bpm" line the app parses (Reset->song bpm) — emitted at preload, not fire.
        Serial.printf("[%s] %s (SD, %lu events, %.1f bpm, %u beats/bar, psram=%uMB ocramFree=%luKB) -> %s (preloaded)\n",
                      t.tag, disp, (unsigned long)got, (double)*t.bpm, (unsigned)*t.bpb,
                      (unsigned)external_psram_size, (unsigned long)(tdsp::smf::ocramHeapFree() / 1024), synthName());
        return true;
    }
    // Rich MPE/MIDI test sequences: play straight from flash (no expansion). A test sets the device
    // mode on fire (preForceMode) so an MPE test gets per-note expression.
    for (int i = 0; i < testsong::kNumTestSongs; ++i) {
        if (strcasecmp(arg, testsong::kTestSongs[i].name) != 0) continue;
        *t.bpm = testsong::kTestSongs[i].bpm; *t.bpb = 4;
        snprintf(t.name, kSongNameCap, "%s", testsong::kTestSongs[i].name);
        snprintf(t.arg,  kSongArgCap,  "%s", testsong::kTestSongs[i].name);
        t.preEv = testsong::kTestSongs[i].ev; t.preCount = testsong::kTestSongs[i].count;
        t.preForceMode = true; t.preMpe = testsong::kTestSongs[i].mpe;
        Serial.printf("[%s] %s (%s, %lu events, %.1f bpm) -> %s (preloaded)\n", t.tag, testsong::kTestSongs[i].name,
                      testsong::kTestSongs[i].mpe ? "MPE" : "MIDI", (unsigned long)t.preCount, (double)*t.bpm, synthName());
        return true;
    }
    // Baked legacy demos (SongEv, tempo estimate) — expand into the track's buffer. A prior MPE test
    // may have left MPE on; fire returns to normal MIDI so a multitimbral song plays right (default preMpe).
    for (int i = 0; i < kNumBuiltin; ++i) {
        if (strcasecmp(arg, kBuiltinSongs[i].name) != 0) continue;
        *t.bpm = kBuiltinSongs[i].bpm; *t.bpb = 4;
        uint32_t n = tdsp::expandLegacyNotes(kBuiltinSongs[i].ev, kBuiltinSongs[i].count, t.buf, t.bufCap);
        if (!n) return false;
        snprintf(t.name, kSongNameCap, "%s", kBuiltinSongs[i].name);
        snprintf(t.arg,  kSongArgCap,  "%s", kBuiltinSongs[i].name);
        t.preEv = t.buf; t.preCount = n;
        Serial.printf("[%s] %s (%.1f bpm est) -> %s (preloaded)\n", t.tag, kBuiltinSongs[i].name, (double)*t.bpm, synthName());
        return true;
    }
    Serial.printf("[%s] not found: %s\n", t.tag, arg);
    return false;
}

// trackFire: PLAY the preloaded stream — fast + non-blocking, so it can fire ON the downbeat without
// stalling loop(). anchorNow=true starts it from its top at the current beat (a quantized bar launch);
// false joins the running grid in phase. The voice-1-only global-mode/meter effects are caps-gated.
static void trackFire(Track &t, bool anchorNow) {
    if (!t.preEv || t.preCount == 0) return;
    // Content that DEFINES the downbeat also owns the tempo: snap the master BPM to this song/groove's
    // native BPM so the box plays at its authored feel, and push the new tempo to the app. Gated four
    // ways: (1) the track opts in (caps.tempoSourceWhenIdle — every song + the groove); (2) it's an
    // immediate/synchro start, not a quantized bar-join (anchorNow joins a running grid, keep its
    // tempo); (3) it's the SOLE content — nothing else already playing, so a second source keeps the
    // established grid; (4) the tempo isn't LOCKED (the metronome lock-icon pins a hand-dialled tempo).
    if (t.caps.tempoSourceWhenIdle && !anchorNow && !g_tempoLock && *t.bpm > 1.0f && !transportHasContent()) {
        g_masterBpm = *t.bpm;
        emitMasterBpm();   // reflect the followed tempo in the app's BPM readout (applyTempos() pushes it to the clock)
    }
    songPrep(t);                                       // drum: applies the kit (no note panic)
    if (t.caps.ownsGlobalMode && (t.preForceMode || g_mpeMode != t.preMpe)) applyMidiMode(t.preMpe);
    if (t.caps.mutesSongDrums) muteSongDrums(true);    // the groove IS the beat -> mute the song's own ch10
    applyTempos();              // retime this player (and the groove) to the master BPM
    ensureTransportStarted();   // define the grid if idle, else join the running clock in phase
    t.player->play(t.preEv, t.preCount);
    g_syncAnchorNow = anchorNow;
    songApplySync(t, t.preLoopBeats);   // grid-lock (exact length from the parse, else derived from ms)
    // Loop SEAMLESSLY when loop is on: the player wraps itself on the exact bar boundary (tickSynced),
    // so there is NO stop/re-arm/re-zero at the seam. That re-arm re-zeroed the grid + resynced the arp
    // every loop, racing the re-armed song's first-note dispatch -> the arp missed its downbeat step
    // (the dropped-first-beat bug). Seamless keeps the grid continuous so the arp steps through the seam.
    t.player->setLooping(*t.loop);
    if (t.caps.ownsMeter) { applyMeter(); g_songBarClock = 0; }   // meter master (voice 1 / a groove while it plays)
}

// trackStartArg: immediate start, in phase with the running grid — preload then fire from the current
// beat. The non-quantized entry (a loop re-arm, @SONG2F, or launch-quantize off). Voice 2's
// split-guard (caps.splitGuarded) blocks it until Synth B is enabled.
FLASHMEM static void trackStartArg(Track &t, const char *arg) {
#if TDSP_VOICE2
    if (t.caps.splitGuarded && !g_voice2Split) {   // Synth B off -> engines 4..7 belong to Synth A
        Serial.println("[song2] ignored: enable Synth B (@VOICE2=1) first");
        return;
    }
#endif
    if (trackPreload(t, arg)) trackFire(t, /*anchorNow=*/false);
}

// Back-compat: @SONG=<i> plays the i-th catalog row (resolved via songs.ndjson).
FLASHMEM static void trackStartIndex(Track &t, int idx) {
    char arg[120];
    if (songByIndex(idx, arg, sizeof arg, nullptr, 0)) trackStartArg(t, arg);
    else Serial.printf("[%s] index %d out of range\n", t.tag, idx);
}
// Stop a track's song (unified — replaces songStop()/song2Stop()). Silence it, disarm the
// loop-restart, and revert the meter if this track owns it. Voice 1 shares its sink with the ch10
// drum groove so it spares ch10 (caps.prepSpecial) — voice 2 has a private sink -> plain reset.
static void songStop(Track &t) {
    *t.wasPlaying = false;   // a manual stop must NOT trigger the loop-restart
    if (!t.player->isPlaying()) return;
    t.player->stop();
    if (t.caps.prepSpecial && g_drumPlayer.isPlaying()) {
        for (uint8_t ch = 1; ch <= 16; ++ch) if (ch != 10) t.sink->onAllNotesOff(ch);
    } else {
        t.sink->onAllNotesOff(0);
    }
    Serial.printf("[%s] stopped\n", t.tag);
    if (t.caps.ownsMeter) applyMeter();   // gave up the meter -> revert (voice 1 / a groove owns it while it plays)
}

// Called every loop() per track: if a looping song just ended on its own, restart it. A manual stop
// clears *t.wasPlaying (in songStop), so it doesn't re-trigger. Unified across voices — replaces
// songLoopTick/song2LoopTick. Voice 1 (caps.ownsMeter) also reverts the meter on a natural non-loop
// end; voice 2 doesn't own the meter, so that branch is a no-op for it.
static void trackLoopTick(Track &t) {
    bool now = t.player->isPlaying();
    if (*t.wasPlaying && !now) {
        if (*t.loop) {
            trackStartArg(t, t.arg);   // re-arm the same song (also re-applies its MIDI/MPE mode if owned)
            now = t.player->isPlaying();
        } else if (t.caps.ownsMeter) {
            applyMeter();              // natural end (not looping): song gives up the meter -> groove's, else 4/4
        }
    }
    *t.wasPlaying = now;
}

// Push a track's playback position to the app (@SONGP / @SONG2P), ~2.5x/sec while playing + one
// "=-1" edge frame when it stops (resets the app's bar + clears the ♪). Unified from the twin
// per-voice feeds; the caller owns the throttle clock + edge state (block-static, per track). Call
// AFTER trackLoopTick() so a loop re-arm keeps us "playing" (no spurious -1 at the loop seam).
static void emitTrackPos(Track &t, const char *cmd, elapsedMillis &clk, bool &prev) {
    const bool now = t.player->isPlaying();
    if (now) { if (clk >= 400) { clk = 0; Serial.printf("%s=%u\n", cmd, t.player->positionPermille()); } }
    else if (prev) { Serial.printf("%s=-1\n", cmd); }
    prev = now;
}

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

// Drum-track state (Phase 2): the drum groove becomes g_drumTrack, a Track peer of the two synth
// voices, so it runs the ONE quantized launch/stop/sync path instead of the special drumStart*.
// These mirror the g_curSong{,2}* / g_song{,2}* state the synth tracks bind. g_drumTrack is bound
// in tracksInit() and (Phase 2) routed through trackPreload/trackFire. Loop is always on (a groove
// is a loop); wasPlaying feeds trackLoopTick (a no-op while loopsSeamless keeps the player running).
static char g_curDrumName[64] = "";     // current groove display name
static char g_curDrumArg[100] = "";     // current groove replay arg (SD path / filename)
static bool g_drumWasPlaying  = false;
static bool g_drumLoop        = true;   // a groove always loops (pinned; the drum Track's *loop)
static Track g_drumTrack;               // bound in tracksInit(); NOT in g_tracks[] yet (own tick/@STATE until P2.4)

// GM drum kits — the "instrument" the Drums menu picks. Selecting one sends a
// program change on channel 10; GM engines (TSF/SF2) switch kit, others ignore.
struct DrumKit { const char *name; uint8_t prog; };
static const DrumKit kDrumKits[] = {
    {"Standard", 0}, {"Room", 8}, {"Power", 16}, {"Electronic", 24}, {"TR-808", 25},
    {"Jazz", 32}, {"Brush", 40}, {"Orchestra", 48}, {"SFX", 56},
};
static const int kNumDrumKits = sizeof(kDrumKits) / sizeof(kDrumKits[0]);

// Runtime drum-kit table. When an ACOUSTIC multi-kit drum font (/sf2/drumkits.sf2,
// fetch_drumkits.py) is loaded, its kits are bank-128 presets addressed by program — the
// same mechanism as GM kits — so we swap the Drums menu to that font's kit list, read from
// /sf2/drumkits.tsv (cols: program, name, license, pieces, display). g_numRtKits > 0 means
// "use this table instead of kDrumKits[]"; it stays 0 on GM/OPLL builds so behavior is
// unchanged there. Accessors below funnel every kDrumKits reader through one seam.
struct RtDrumKit { char name[24]; uint8_t prog; };
static RtDrumKit g_rtKits[24];
static int       g_numRtKits = 0;
#if defined(TDSP_DRUM_SD)
// SD sampler: a "kit" is a subfolder of /drums (scanned by DrumSampler). The kit list IS the scanned
// folder list, and the "program" is just the folder index — drumApplyKit() sends it as a ch10 program
// change that DrumSamplerSink::onProgramChange maps back to setKit(). No GM kDrumKits table here.
static inline int         numDrumKits()      { return g_drumSamplerSink.numKits(); }
static inline const char *drumKitName(int i) { return g_drumSamplerSink.kitName(i); }
static inline uint8_t     drumKitProg(int i) { return (uint8_t)i; }
#else
static inline int         numDrumKits()      { return g_numRtKits > 0 ? g_numRtKits : kNumDrumKits; }
static inline const char *drumKitName(int i) { return g_numRtKits > 0 ? g_rtKits[i].name : kDrumKits[i].name; }
static inline uint8_t     drumKitProg(int i) { return g_numRtKits > 0 ? g_rtKits[i].prog : kDrumKits[i].prog; }
#endif

#if TDSP_HAS_SDCARD
// Populate g_rtKits from a font's sibling <font>.tsv (written by fetch_drumkits.py /
// build_mars_kits.py alongside the font: /sf2/drumkits.sf2 -> /sf2/drumkits.tsv,
// /sf2/mars_909.sf2 -> /sf2/mars_909.tsv). The manifest cols are program, name, license,
// pieces, display. Called at boot for the resident font and again on a runtime @DRUMFONT
// swap. Resets g_numRtKits first, so a font WITHOUT a sibling .tsv falls back to the GM kit
// names (g_numRtKits == 0) instead of keeping the previous font's kits.
static void loadDrumKitsTsv(const char *fontPath) {
    g_numRtKits = 0;                                       // fall back to GM names if the sibling .tsv is absent
    // Derive "<font>.tsv" from the font path (replace a trailing .sf2, else append .tsv).
    char tsvPath[80];
    strncpy(tsvPath, fontPath, sizeof(tsvPath) - 1); tsvPath[sizeof(tsvPath) - 1] = 0;
    char *dot = strrchr(tsvPath, '.');
    if (dot && strcasecmp(dot, ".sf2") == 0) { strcpy(dot, ".tsv"); }
    else { size_t L = strlen(tsvPath); if (L + 4 < sizeof(tsvPath)) strcpy(tsvPath + L, ".tsv"); }
    File f = SD.open(tsvPath);
    if (!f) { Serial.printf("[drum] %s missing -> keeping GM kit names\n", tsvPath); return; }
    int n = 0;
    while (f.available() && n < (int)(sizeof(g_rtKits) / sizeof(g_rtKits[0]))) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line[0] == '#') continue;      // skip blank + header comment
        // program \t name \t license \t pieces \t display
        int t1 = line.indexOf('\t');            if (t1 < 0) continue;
        int prog = line.substring(0, t1).toInt();
        int tLast = line.lastIndexOf('\t');      // display is the final column
        String disp = (tLast > t1) ? line.substring(tLast + 1) : line.substring(t1 + 1);
        disp.trim();
        if (disp.length() == 0 || prog < 0 || prog > 127) continue;
        strncpy(g_rtKits[n].name, disp.c_str(), sizeof(g_rtKits[n].name) - 1);
        g_rtKits[n].name[sizeof(g_rtKits[n].name) - 1] = 0;
        g_rtKits[n].prog = (uint8_t)prog;
        ++n;
    }
    f.close();
    g_numRtKits = n;
    Serial.printf("[drum] loaded %d acoustic kit(s) from %s\n", n, tsvPath);
}
#endif

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
        for (int i = 0; i < numDrumKits(); ++i) {
            k.print("{\"name\":"); tdsp::catdb::jsonStr(k, drumKitName(i));
            k.print(",\"prog\":"); k.print(drumKitProg(i)); k.print("}\n");
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
            if (!SD.exists("/midi/songs")) SD.mkdir("/midi/songs");
            writeSongDir(so, "/midi/songs");                    // recursive (walks /midi/songs/<genre>/…)
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
    if (!SD.exists("/midi/drums")) SD.mkdir("/midi/drums");
    File d = SD.open("/midi/drums");
    if (!d || !d.isDirectory()) { if (d) d.close(); return; }
    const int cap = (int)(sizeof(g_drums) / sizeof(g_drums[0]));
    for (File f = d.openNextFile(); f && g_numDrums < cap; f = d.openNextFile()) {
        const char *nm = f.name();
        if (!f.isDirectory() && nm && endsWithMid(nm)) {
            DrumRef &r = g_drums[g_numDrums++];
            snprintf(r.path, sizeof(r.path), "/midi/drums/%s", nm);
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
    const uint8_t prog = drumKitProg(g_drumKit);
#if defined(TDSP_DRUM_TSF)
    g_drumTsfSink.onProgramChange(10, prog);   // kit lives on the dedicated drum TSF
#elif defined(TDSP_DRUM_VOICE)
    g_drumVoiceSink.onProgramChange(10, prog); // OPLL rhythm ignores it, but stays consistent
#elif defined(TDSP_DRUM_SD)
    g_drumSamplerSink.onProgramChange(10, prog); // prog = folder index -> setKit()
#else
    g_synthSink->onProgramChange(10, prog);
#endif
}

#if defined(TDSP_DRUM_TSF)
// Count the swappable drum fonts on the card: rows in /sf2/fonts.tsv if present, else /sf2/*.sf2,
// always including the boot default /sf2/drumkits.sf2. Cached at boot into g_drumFontCount and used
// only for caps.drumfontsel (the app shows the Drum Font picker when there's more than one). FLASHMEM:
// cold path, keep it out of ITCM (see reference_itcm_boundary_cliff).
FLASHMEM static int drumFontCount() {
    if (!g_sdReady) return 0;
    int n = 0; bool haveKits = false;
    File mf = SD.open("/sf2/fonts.tsv");
    if (mf) {
        while (mf.available()) {
            String line = mf.readStringUntil('\n'); line.trim();
            if (line.length() == 0 || line[0] == '#') continue;
            int t1 = line.indexOf('\t');
            String p = (t1 < 0) ? line : line.substring(0, t1); p.trim();
            if (p.length() == 0) continue;
            if (p == DRUM_KITS_FONT_PATH) haveKits = true;
            ++n;
        }
        mf.close();
    } else {
        File d = SD.open("/sf2");
        if (d && d.isDirectory()) {
            for (File f = d.openNextFile(); f; f = d.openNextFile()) {
                const char *nm = f.name();
                if (!f.isDirectory() && nm && endsWithExt(nm, "sf2")) {
                    if (strcasecmp(nm, "drumkits.sf2") == 0) haveKits = true;
                    ++n;
                }
                f.close();
            }
        }
        if (d) d.close();
    }
    if (!haveKits && SD.exists((char *)DRUM_KITS_FONT_PATH)) ++n;
    return n;
}

// Runtime drum-font swap (see planning/drums-from-mars/RUNTIME_FONT_SWAP.md). Runs in the command
// handler (loop context), NEVER the audio ISR. The whole SF2 is PSRAM-resident, so this frees the old
// font and loads a new one live. ISR-SAFETY is the #1 risk: the audio ISR both RENDERS from the tsf
// (AudioSynthTsf::update reads m_tsf) AND MUTATES it (TsfSink derefs *g_drumTsfHandle). BOTH must be
// detached from the live handle before tsf_close(), or the ISR reads freed PSRAM -> hardfault. On a
// load failure we reload the previous font — never leave a freed/null handle live with the bus open.
// FLASHMEM: cold path (ITCM budget).
FLASHMEM static bool drumFontSwap(const char *path) {
    if (!g_sdReady) { Serial.println("[drumfont] no SD card"); return false; }
    if (!path || !path[0] || !SD.exists((char *)path)) {
        Serial.printf("[drumfont] not found: %s\n", path ? path : "(null)");
        return false;
    }
    // Remember the current font so a load failure can roll back to it.
    char prevPath[sizeof(g_drumFontPath)];
    strncpy(prevPath, g_drumFontPath, sizeof(prevPath)); prevPath[sizeof(prevPath) - 1] = 0;
    tsf *old = g_drumTsfHandle;

    // 1) Silence the drum bus (mix slot 2) and DETACH the handle from the audio ISR. Both the render
    //    source (AudioSynthTsf) and the note sink (TsfSink) deref the handle in the ISR, so null BOTH
    //    under AudioNoInterrupts before we free anything.
    outL.gain(2, 0.0f); outR.gain(2, 0.0f);
    AudioNoInterrupts();
    g_drumTsfHandle = nullptr;      // TsfSink::_t deref -> note ops no-op
    g_drumTsf.begin(nullptr);       // AudioSynthTsf::update sees null m_tsf -> renders silence
    AudioInterrupts();

    // 2) The ISR can no longer touch it — free the old font and load the new one.
    if (old) tsf_close(old);
    tsf *nu = tsfLoadFromSD(path);
    if (!nu) {
        Serial.printf("[drumfont] load FAILED: %s -> restoring %s\n", path, prevPath);
        nu = (prevPath[0]) ? tsfLoadFromSD(prevPath) : nullptr;   // roll back
        path = prevPath;
        if (!nu) {                  // even the previous font won't reload -> leave drums idle (bus stays silent)
            Serial.println("[drumfont] rollback FAILED too -> drums idle");
            g_drumFontIsKits = false; g_drumFontPath[0] = 0; g_drumFontDisplay[0] = 0;
            g_engineHasDrums = false;
            return false;
        }
    }

    // 3) Re-apply the EXACT setup drumTsfBegin() does — the font is fresh, nothing carries over.
    tsf_set_output(nu, TSF_STEREO_UNWEAVED, (int)AUDIO_SAMPLE_RATE_EXACT, -4.0f);
    tsf_set_max_voices(nu, 24);
    for (int ch = 0; ch < 16; ch++) {
        tsf_channel_set_presetnumber(nu, ch, 0, ch == 9 ? 1 : 0);
        tsf_channel_set_pitchrange(nu, ch, 48.0f);
    }

    // 4) Republish the handle to the ISR + restore levels.
    AudioNoInterrupts();
    g_drumTsfHandle = nu;
    g_drumTsf.begin(nu);
    AudioInterrupts();
    g_drumTsf.setGain(1.0f);
    outL.gain(2, 0.62f); outR.gain(2, 0.62f);

    // 5) Track the new font, reload its sibling kit list, reset to kit 0, mark drums available.
    strncpy(g_drumFontPath, path, sizeof(g_drumFontPath) - 1);
    g_drumFontPath[sizeof(g_drumFontPath) - 1] = 0;
    drumFontDeriveDisplay(g_drumFontPath, g_drumFontDisplay, sizeof(g_drumFontDisplay));
    loadDrumKitsTsv(g_drumFontPath);
    g_drumFontIsKits = (g_numRtKits > 0);
    g_drumKit = 0; drumApplyKit();
    g_engineHasDrums = true;

    Serial.printf("[drumfont] loaded %s (%d presets, %d kits)\n",
                  g_drumFontPath, tsf_get_presetcount(nu), numDrumKits());
    return true;
}

// Emit the available-fonts list for the app's Drum Font picker. Mirrors the @DRUMS/@MANIFESTS wire
// framing: '|'-separated items, each item's fields joined by US (0x1f) as path\x1fdisplay\x1fkits\x1fcurrent
// (current = 1 for the resident font). Source: /sf2/fonts.tsv (cols path\tdisplay\tkits\tbytes, '#'
// header) when present, else a scan of /sf2/*.sf2; the boot default /sf2/drumkits.sf2 is always included.
FLASHMEM static void sendFonts(Print &out) {
    out.print("@FONTS=");
    int emitted = 0; bool haveKits = false;
    auto emitFont = [&](const char *p, const char *disp, const char *kits) {
        if (emitted++) out.print('|');
        out.print(p);    out.write((uint8_t)0x1f);
        out.print(disp); out.write((uint8_t)0x1f);
        out.print(kits); out.write((uint8_t)0x1f);
        out.print(strcmp(p, g_drumFontPath) == 0 ? 1 : 0);
    };
    File mf = g_sdReady ? SD.open("/sf2/fonts.tsv") : File();
    if (mf) {
        while (mf.available()) {
            String line = mf.readStringUntil('\n'); line.trim();
            if (line.length() == 0 || line[0] == '#') continue;
            // path \t display \t kits \t bytes
            int t1 = line.indexOf('\t'); if (t1 < 0) continue;
            String p = line.substring(0, t1); p.trim();
            String rest = line.substring(t1 + 1);
            int t2 = rest.indexOf('\t');
            String disp = (t2 < 0) ? rest : rest.substring(0, t2); disp.trim();
            String kits = "0";
            if (t2 >= 0) { String r2 = rest.substring(t2 + 1); int t3 = r2.indexOf('\t');
                           kits = (t3 < 0) ? r2 : r2.substring(0, t3); kits.trim(); }
            if (p.length() == 0) continue;
            if (disp.length() == 0) { char d[40]; drumFontDeriveDisplay(p.c_str(), d, sizeof d); disp = d; }
            if (p == DRUM_KITS_FONT_PATH) haveKits = true;
            emitFont(p.c_str(), disp.c_str(), kits.c_str());
        }
        mf.close();
    } else if (g_sdReady) {
        File d = SD.open("/sf2");
        if (d && d.isDirectory()) {
            for (File f = d.openNextFile(); f; f = d.openNextFile()) {
                const char *nm = f.name();
                if (!f.isDirectory() && nm && endsWithExt(nm, "sf2")) {
                    char p[80]; snprintf(p, sizeof p, "/sf2/%s", nm);
                    char disp[40]; drumFontDeriveDisplay(p, disp, sizeof disp);
                    if (strcasecmp(nm, "drumkits.sf2") == 0) haveKits = true;
                    emitFont(p, disp, "0");
                }
                f.close();
            }
        }
        if (d) d.close();
    }
    if (!haveKits && g_sdReady && SD.exists((char *)DRUM_KITS_FONT_PATH)) {
        char disp[40]; drumFontDeriveDisplay(DRUM_KITS_FONT_PATH, disp, sizeof disp);
        emitFont(DRUM_KITS_FONT_PATH, disp, "0");
    }
    out.print('\n');
}
#endif  // TDSP_DRUM_TSF

// Launch a groove (by full SD path) through the drum Track — the SAME preload->fire path the song
// players use, so a groove lands on the bar downbeat (launch-quantize aware) instead of overtaking a
// running player, and the (blocking) SD parse runs off the beat. The drum caps carry the specials:
// drumGated (engine must render ch10), appliesKit (prep sets the GM kit), mutesSongDrums (mute the
// song's ch10 while the groove plays), tempoSourceWhenIdle (a groove defining the grid sets the BPM).
// SYNCHRO START (PSS-140): arm — preload now and let the first live note be the downbeat (maybeSynchroStart).
static void drumLaunchPath(const char* path) {
    g_drumArmed = false;
    if (g_drumSynchro) {
        if (trackPreload(g_drumTrack, path)) {   // stash the groove; the first live note fires it
            g_drumArmed = true;
            Serial.printf("[drum] %s SYNCHRO armed @ %.0f bpm — play a note to start\n", g_curDrumName, (double)g_masterBpm);
        }
        return;
    }
    trackLaunch(g_drumTrack, path);              // launch-quantize aware: preload+arm on the next bar, else immediate
}
static void drumStart(int idx) {   // legacy numeric index (flat menu / serial C/D keys)
    if (g_numDrums == 0) { Serial.println("[drum] no grooves on SD (/drums) — run tools/fetch_drums.py"); return; }
    if (idx < 0) idx = 0;
    if (idx >= g_numDrums) idx = g_numDrums - 1;
    g_drumSel = idx;
    drumLaunchPath(g_drums[idx].path);
}
static void drumStartFile(const char* fname) {   // @DRUMF: by filename (or a full /path from the browser)
    // A leading '/' (e.g. "/midi/drums/Foo.mid" from the browser) is used verbatim; a bare filename
    // is rooted at /midi/drums. Routes through the drum Track (quantized launch via drumLaunchPath).
    if (fname[0] == '/') { drumLaunchPath(fname); return; }
    char path[160]; snprintf(path, sizeof(path), "/midi/drums/%s", fname);
    drumLaunchPath(path);
}
static void drumStop() {
    g_drumArmed = false;                 // cancel a synchro-armed groove (not a Track concern yet)
    muteSongDrums(false);                // give the song back its own drums (even if the groove wasn't playing)
    songStop(g_drumTrack);               // stop + "[drum] stopped" + meter revert, via the unified Track path
}

#ifdef TDSP_METRONOME
// --- Global transport: the metronome IS the master clock --------------------
// Play defines the shared downbeat and runs the clock; everything (both song players, drums,
// arps) locks to it. Stop halts the clock and clears the stage — both players, drums, and every
// held note — so nothing hangs (this is also the clean-silence path for the stuck-note case).
static void transportPlay() {
    if (g_conductor.running()) return;      // already running -> don't move the grid
    g_conductor.start();                    // zero the tick counter -> beat 1 downbeat, clock runs
    g_arpFilter.resyncToGrid();
#if TDSP_ARP2
    g_arpFilter2.resyncToGrid();
#endif
#if TDSP_SYNTH_VOICES >= 3
    for (int v = 2; v < kSynthVoices; ++v) if (g_tracks[v].arp) g_tracks[v].arp->resyncToGrid();  // extra voices' arps lock to the same downbeat
#endif
    Serial.println("[transport] PLAY (downbeat defined)");
}
static void transportStop() {
    songStop(g_tracks[0]);
#if TDSP_VOICE2
    songStop(g_tracks[1]);
#endif
    drumStop();
    g_conductor.stop();                     // halt the master clock (players/drums already stopped)
    g_synthSink->onAllNotesOff(0);          // silence voice 1
#if TDSP_VOICE2
    g_synthSinkB->onAllNotesOff(0);         // silence voice 2
#endif
    g_arpFilter.panic();                    // clear any arp-held / pending gate-offs
#if TDSP_ARP2
    g_arpFilter2.panic();
#endif
#if TDSP_SYNTH_VOICES >= 3
    // Extra voices beyond A/B — the 4-way Dexed pool's voices 2/3, OR the hetero OPLL voice (Synth C).
    // Master stop must clear EVERY voice, not just A/B, or a started voice (e.g. the OPLL) keeps ringing
    // with no way to stop it. Stop each player, silence its sink, and panic its arp (mirrors voices 0/1).
    for (int v = 2; v < kSynthVoices; ++v) {
        songStop(g_tracks[v]);
        if (g_tracks[v].sink) g_tracks[v].sink->onAllNotesOff(0);
        if (g_tracks[v].arp)  g_tracks[v].arp->panic();
    }
#endif
    Serial.println("[transport] STOP (all silenced)");
}
#endif  // TDSP_METRONOME
static void setDrumKit(int i) {
    if (i < 0) i = 0;
    if (i >= numDrumKits()) i = numDrumKits() - 1;
    g_drumKit = i;
    if (drumEngineOk()) drumApplyKit();
    Serial.printf("[drum] kit -> %s (prog %u)\n", drumKitName(i), drumKitProg(i));
}

// --- Launch quantize: the user-facing START entry points. When quantize is on they PRELOAD now
// (off the beat) and arm a pending launch that loop() FIRES on the next bar edge; otherwise they
// start immediately. Unified Track& path — replaces songLaunch/songRestart + song2StartArg/song2Restart.
// trackLaunch: the app's play-by-name entry (@SONGF/@SONG2F). With launch-quantize on and the clock
// running, preload + arm; otherwise start immediately.
static void trackLaunch(Track &t, const char* arg) {
    if (g_launchQuantize && g_conductor.running()) {
#if TDSP_VOICE2
        if (t.caps.splitGuarded && !g_voice2Split) { Serial.println("[song2] ignored: enable Synth B (@VOICE2=1) first"); return; }
#endif
        if (!trackPreload(t, arg)) return;                          // load off the beat; nothing to fire if it failed
        *t.launchPending = true; g_launchSched.barHit = false;      // wait for the NEXT bar edge
        Serial.printf("[sync] %s launch armed (preloaded): %s -> next bar\n", t.tag, arg);
        return;
    }
    trackStartArg(t, arg);
}
// trackRestart: hard restart from the top, in time (the app's MIDI-player Play / ‹ ›). If the master
// clock is already running (a player, a groove, or just the metronome owns the grid), DON'T re-zero it
// — that would strand whatever's running. PRELOAD now (off the downbeat, so the SD parse never stalls
// loop() on the beat) and FIRE on the NEXT bar edge, from the top, while everything else keeps running.
// Only an idle transport defines the downbeat (re-zero also re-locks a groove + held arp chord to the
// fresh downbeat). Replaces songRestart/song2Restart.
static void trackRestart(Track &t, const char* arg) {
    if (!arg || !*arg) return;
#if TDSP_VOICE2
    if (t.caps.splitGuarded && !g_voice2Split) { Serial.println("[song2] ignored: enable Synth B (@VOICE2=1) first"); return; }
#endif
    if (g_conductor.running()) {
        if (!trackPreload(t, arg)) return;
        *t.launchPending = true; g_launchSched.barHit = false;
        Serial.printf("[sync] %s launch armed (preloaded): %s -> next bar (from top)\n", t.tag, arg);
        return;
    }
    g_forceTransportZero = true;   // idle: define the downbeat now and start from the top
    trackStartArg(t, arg);
    g_forceTransportZero = false;  // safety: clear if trackStartArg bailed before consuming it
}
// @DRUMF entry: the drum Track's trackLaunch (inside drumStartFile) is already launch-quantize
// aware (preload+arm on the bar, else immediate), so this is just the filename->path hop.
static void drumLaunchFile(const char* fname) { drumStartFile(fname); }
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
#if defined(TDSP_DRUM_VOICE) || defined(TDSP_DRUM_TSF) || defined(TDSP_DRUM_SD)
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
    out.print("drums\x1f");  out.print(g_sdReady ? "file:/midi/drums/catalog.tsv" : "none");
    out.print("|drumkit\x1fbundled:gmkits");
    out.print("|instr\x1f"); out.print(synthIsGM() ? "bundled:gm128" : "engine");
    // loops = melody/bass loops the app loads into a track's looper via @LOOPF (see
    // project_midi_loops / tools/fetch_loops.py). Gated on the recorder build (the
    // loopers @LOOPF targets); "none" hides the loops picker on non-recorder builds.
#if TDSP_RECORDER
    out.print("|loops\x1f"); out.print(g_sdReady ? "file:/midi/loops/catalog.tsv" : "none");
#endif
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

// Keep the master transport alive during a long, blocking SD stream. streamFile()/
// streamDir()/streamClip() run a synchronous read/encode/write loop that monopolizes
// loop() for the WHOLE transfer — so while the app fetches e.g. /tdsp/grooves.ndjson to
// build the drum-loops page, the Conductor clock, metronome click, and groove/song
// players would all freeze (then jerk to catch up). Pumping the SD-free, non-blocking
// realtime work between frames keeps time running smoothly across the transfer. Defined
// after all the objects it touches (just above loop()); prototype here so the streamers
// can call it. Deliberately SD-free: it does NOT fire launch-quantized starts or song-loop
// restarts (those re-parse the SD and would re-enter the very transfer that called us).
static void pumpTransport();

static void streamFile(Print& out, const char* path) {
    const uint8_t id = ++g_xferId;
    File f = SD.open(path);
    if (!f || f.isDirectory()) { if (f) f.close(); out.printf("@FERR=%u\x1f%s\n", id, "not found"); return; }
    out.printf("@FB=%u\x1f%s\x1f%lu\n", id, path, (unsigned long)f.size());
    uint8_t raw[360];   // 360 = mult of 3 (no mid-stream b64 pad) AND @FD line fits one ~512 BLE MTU
    char b64[4 * (sizeof(raw) / 3) + 1];
    uint32_t seq = 0;
    elapsedMicros svcClock = 0;   // pace pumpTransport() so a big/slow read never starves the clock
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
        if (svcClock >= 2000) { svcClock = 0; pumpTransport(); }   // ~2ms: keep clock/click/grooves running
        if (n < (int)sizeof(raw)) break;   // final (short) read
    }
    f.close();
    out.printf("@FE=%u\x1f%lu\n", id, (unsigned long)seq);
}

// --- Generic recursive SD directory list (@LS) -------------------------------
// One small framed line per entry, modeled on streamFile's @FB/@FD/@FE. The CLIENT
// drills folder-by-folder and sorts; the firmware only lists ONE level in filesystem
// order (openNextFile), never buffering or sorting. Subdirs are always listed (D);
// files (F) only when `ext` is empty OR the name ends ".<ext>" (case-insensitive).
// Dotfiles + "System Volume Information" are skipped. Jailed to the card: any path
// containing ".." is rejected. Reusable for future /samples, /sf2, /dexed browsing.
//     @LB=<id>\x1f<path>            begin (id = ++counter)
//     @LD=<id>\x1f<D|F>\x1f<name>   one entry (bare name, not a full path)
//     @LE=<id>\x1f<count>           end (number of @LD emitted)
//     @LERR=<id>\x1f<reason>        error
static uint8_t g_lsId = 0;
static void streamDir(Print& out, const char* path, const char* ext) {
    const uint8_t id = ++g_lsId;
    if (!path || !*path || strstr(path, "..")) { out.printf("@LERR=%u\x1f%s\n", id, "bad path"); return; }
    File d = SD.open(path);
    if (!d || !d.isDirectory()) { if (d) d.close(); out.printf("@LERR=%u\x1f%s\n", id, "not a dir"); return; }
    out.printf("@LB=%u\x1f%s\n", id, path);
    uint32_t count = 0;
    elapsedMicros svcClock = 0;   // keep the transport ticking while a big folder lists
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
        const char* nm = f.name();
        const bool dir = f.isDirectory();
        if (nm && nm[0] != '.' && strcmp(nm, "System Volume Information") != 0) {
            // NB: split the string literals around the D/F — a "\x1fD" would parse as the greedy
            // hex escape \x1FD (0xFD), swallowing the marker. Adjacent literals end the escape at \x1f.
            if (dir) { out.printf("@LD=%u\x1f" "D" "\x1f%s\n", id, nm); ++count; }
            else if (endsWithExt(nm, ext)) { out.printf("@LD=%u\x1f" "F" "\x1f%s\n", id, nm); ++count; }
            // Pace the ESP32/BLE link (as streamFile does); USB CDC is flow-controlled.
            if (&out != &Serial) delay(4);
        }
        f.close();
        if (svcClock >= 2000) { svcClock = 0; pumpTransport(); }
    }
    d.close();
    out.printf("@LE=%u\x1f%lu\n", id, (unsigned long)count);
}

#if TDSP_RECORDER_EDIT
// Stream an in-memory LoopClip using the SAME @FB/@FD/@FE base64 framing as streamFile,
// but sourced from the clip byte-by-byte (LoopClipIo) with no whole-clip scratch buffer —
// the note-editor @RECDUMP path (planning/midi-editor/DESIGN.md §4.1). `name` is a synthetic
// path (e.g. "mem:/loop1") so the app's file reassembler can match the transfer.
static void streamClip(Print& out, const char* name, const tdsp::LoopClip& c) {
    const uint32_t total = tdsp::loopClipBytes(c);
    const uint8_t  id    = ++g_xferId;
    out.printf("@FB=%u\x1f%s\x1f%lu\n", id, name, (unsigned long)total);
    uint8_t raw[360];   // same window as streamFile: mult of 3, @FD line fits one BLE MTU
    char b64[4 * (sizeof(raw) / 3) + 1];
    uint32_t seq = 0, pos = 0;
    elapsedMicros svcClock = 0;   // keep the transport ticking during the clip dump
    while (pos < total) {
        int n = 0;
        while (n < (int)sizeof(raw) && pos < total) raw[n++] = tdsp::loopClipByteAt(c, pos++);
        int o = 0, i = 0;
        for (; i + 3 <= n; i += 3) {
            uint32_t v = ((uint32_t)raw[i] << 16) | ((uint32_t)raw[i + 1] << 8) | raw[i + 2];
            b64[o++] = kB64[(v >> 18) & 63]; b64[o++] = kB64[(v >> 12) & 63];
            b64[o++] = kB64[(v >> 6) & 63];  b64[o++] = kB64[v & 63];
        }
        if (n - i == 1) {
            uint32_t v = (uint32_t)raw[i] << 16;
            b64[o++] = kB64[(v >> 18) & 63]; b64[o++] = kB64[(v >> 12) & 63]; b64[o++] = '='; b64[o++] = '=';
        } else if (n - i == 2) {
            uint32_t v = ((uint32_t)raw[i] << 16) | ((uint32_t)raw[i + 1] << 8);
            b64[o++] = kB64[(v >> 18) & 63]; b64[o++] = kB64[(v >> 12) & 63];
            b64[o++] = kB64[(v >> 6) & 63];  b64[o++] = '=';
        }
        b64[o] = 0;
        out.printf("@FD=%u\x1f%lu\x1f%s\n", id, (unsigned long)seq++, b64);
        if (&out != &Serial) delay(6);   // pace the ESP32/BLE relay (USB is flow-controlled)
        if (svcClock >= 2000) { svcClock = 0; pumpTransport(); }
    }
    out.printf("@FE=%u\x1f%lu\n", id, (unsigned long)seq);
}

// --- Editor clip LOAD receiver (@RECLOAD/@RD/@RECEND) --------------------------
// The upstream mirror of streamClip: @WB can't be relayed over BLE (raw bytes), so the
// editor streams a replacement clip back as base64 @RD frames (DESIGN §4.2). We decode
// straight into the target MidiLooper's clip via its beginClipLoad/pushLoadEvent/endClipLoad
// API — no second 8 KB LoopClip staged. Bytes are consumed as a flat stream: first the
// 12-byte LoopClipIo header, then packed 8-byte events, reassembled across frame boundaries.
static inline int recB64Val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;   // '=' padding or stray char
}

struct RecLoadReceiver {
    bool     active   = false;
    bool     err      = false;
    uint8_t  voice    = 1;
    tdsp::MidiLooper* target = nullptr;
    uint32_t expected = 0;      // total bytes announced by @RECLOAD
    uint32_t got      = 0;      // raw bytes consumed so far
    uint32_t nextSeq  = 0;      // expected @RD seq (gap detection)
    uint8_t  hdr[12]; uint8_t hdrFill = 0; bool hdrDone = false;
    uint8_t  evb[8];  uint8_t evFill  = 0;

    void reset() { active = false; err = false; got = 0; nextSeq = 0;
                   hdrFill = 0; hdrDone = false; evFill = 0; target = nullptr; }

    // One decoded raw byte of the clip stream.
    void feed(uint8_t b) {
        if (err) return;
        got++;
        if (!hdrDone) {
            hdr[hdrFill++] = b;
            if (hdrFill == 12) {
                uint16_t count, loopTicks; uint8_t bpb, bars;
                if (!tdsp::readLoopClipHdr(hdr, count, loopTicks, bpb, bars)) { err = true; return; }
                target->beginClipLoad(loopTicks, bpb, bars);
                hdrDone = true;
            }
            return;
        }
        evb[evFill++] = b;
        if (evFill == 8) {
            if (!target->pushLoadEvent(tdsp::readLoopEvent(evb))) err = true;   // clip full
            evFill = 0;
        }
    }
    // Decode a base64 payload straight into feed().
    void feedB64(const char* s) {
        uint32_t acc = 0; int bits = 0;
        for (; *s; ++s) {
            int v = recB64Val(*s);
            if (v < 0) continue;            // skip '=' / whitespace
            acc = (acc << 6) | (uint32_t)v; bits += 6;
            if (bits >= 8) { bits -= 8; feed((uint8_t)((acc >> bits) & 0xff)); }
        }
    }
};
static RecLoadReceiver g_recLoad;
#endif  // TDSP_RECORDER_EDIT

// --- Live MIDI IN -> MIDI HUB -> per-track routers (Phase 3, Thread C) ----------
// The box can have several MIDI INPUT DEVICES at once (DIN on Serial1, a USB-host controller,
// Bluetooth-MIDI via the ESP32, a serial source) and N synth voices. Every input tags its events
// with a MidiSourceId and calls midihub::*; the hub fans each event ONLY to the Tracks whose
// liveSrcMask subscribes to that source (and whose optional channel filter matches), into that
// Track's router -> [arp] -> sink. So "which device feeds which synth" is a per-Track FIELD, and
// switching it is a field write the hub reads on the next event — the static audio graph never
// changes (no AudioConnection edit, no loadVoice, no clock/player touch). Each router still
// normalizes pitch bend to semitones (2 normal / 48 MPE), CC74 -> timbre, pressure -> Z. This
// replaces the old usbRouter() keyboard-owner switch (gone) with a subscription model. See Track.h.

// SYNCHRO START (PSS-140 style): the first live note kicks off an armed groove on beat 1 — you
// pick the downbeat by when you play. Source-agnostic: any input's first note arms the groove.
static void maybeSynchroStart(byte vel) {
    if (g_drumArmed && vel > 0) {
        g_drumArmed = false;
        g_forceTransportZero = true;    // this note IS the intentional downbeat: define the grid here
        trackFire(g_drumTrack, /*anchorNow=*/false);   // groove was preloaded at arm; fire from the just-zeroed grid
        g_forceTransportZero = false;
        Serial.println("[drum] SYNCHRO start (first note)");
    }
}

// A synth voice can be idled by the runtime @POOL partition (its engines were handed to a group
// lead). An idle voice must take NO live notes — routing to it would allocate on engines the lead
// now owns (voice-stealing). Always-live on non-repartitionable builds.
static inline bool voiceLive(int i) {
#if defined(TDSP_SYNTH_DEXED_POOL) && TDSP_SYNTH_VOICES >= 4 && !TDSP_HETERO
    return (i < 0 || i >= kSynthVoices) ? true : poolVoiceActive(i);
#else
    (void)i; return true;
#endif
}

namespace midihub {
    // Held live notes, per source, so a subscription change can release cleanly (no hung note) on a
    // track that stops listening — the generalization of the old per-keyboard usbHeld flush. Sized
    // for the deepest realistic chord; extra notes past the cap just aren't auto-released on switch.
    struct Held { uint8_t ch, note; };
    static Held    s_held[SrcCount][24];
    static uint8_t s_heldN[SrcCount];
    static void heldAdd(MidiSourceId s, uint8_t ch, uint8_t note) {
        uint8_t &n = s_heldN[s];
        for (uint8_t i = 0; i < n; ++i) if (s_held[s][i].ch == ch && s_held[s][i].note == note) return;
        if (n < 24) { s_held[s][n].ch = ch; s_held[s][n].note = note; ++n; }
    }
    static void heldRemove(MidiSourceId s, uint8_t ch, uint8_t note) {
        uint8_t &n = s_heldN[s];
        for (uint8_t i = 0; i < n; ++i) if (s_held[s][i].ch == ch && s_held[s][i].note == note) { s_held[s][i] = s_held[s][--n]; return; }
    }
    static inline bool subscribed(const Track &t, MidiSourceId s) { return (t.liveSrcMask & srcBit(s)) != 0; }
    static inline bool chOk(const Track &t, uint8_t ch) { return t.srcChMask == 0 || (ch >= 1 && ch <= 16 && (t.srcChMask & (uint16_t)(1u << (ch - 1)))); }

    // Live finger-drumming: when the drum track subscribes to a source, a controller's notes play the
    // GM kit on ch10 REGARDLESS of the controller's own channel (a keyboard sends ch1). The drum track
    // has no router (a groove has no live input) — a drum hit is a momentary one-shot, so we deliver
    // straight to its sink with no held-note tracking and nothing to release on a subscription switch.
    // Gated on drumEngineOk() so we only fire when a ch10-capable engine actually loaded.
    static inline void drumLive(MidiSourceId s, uint8_t ch, uint8_t note, uint8_t vel) {
        if (!g_drumTrack.sink || !subscribed(g_drumTrack, s) || !chOk(g_drumTrack, ch) || !drumEngineOk()) return;
        if (vel) g_drumTrack.sink->onNoteOn(10, note, vel);
        else     g_drumTrack.sink->onNoteOff(10, note, 0);
    }

    // The five fan-out entries. O(tracks) enum/pointer compares + forward — no audio-graph work.
    static void noteOn(MidiSourceId s, uint8_t ch, uint8_t note, uint8_t vel) {
        maybeSynchroStart(vel);
        if (vel) heldAdd(s, ch, note); else heldRemove(s, ch, note);
        for (Track &t : g_tracks) if (t.router && subscribed(t, s) && chOk(t, ch) && voiceLive((int)(&t - g_tracks))) t.router->handleNoteOn(ch, note, vel);
        drumLive(s, ch, note, vel);   // live finger-drumming: a subscribed drum track plays the kit on ch10
    }
    static void noteOff(MidiSourceId s, uint8_t ch, uint8_t note, uint8_t vel) {
        heldRemove(s, ch, note);
        for (Track &t : g_tracks) if (t.router && subscribed(t, s) && chOk(t, ch)) t.router->handleNoteOff(ch, note, vel);
        drumLive(s, ch, note, 0);   // note-off -> ch10 (drum sink chokes open-hat, ignores the rest)
    }
    static void controlChange(MidiSourceId s, uint8_t ch, uint8_t cc, uint8_t val) {
        for (Track &t : g_tracks) if (t.router && subscribed(t, s) && chOk(t, ch)) t.router->handleControlChange(ch, cc, val);
    }
    static void pitchBend(MidiSourceId s, uint8_t ch, int bend) {
        for (Track &t : g_tracks) if (t.router && subscribed(t, s) && chOk(t, ch)) t.router->handlePitchBend(ch, (int16_t)bend);
    }
    static void channelPressure(MidiSourceId s, uint8_t ch, uint8_t pressure) {
        for (Track &t : g_tracks) if (t.router && subscribed(t, s) && chOk(t, ch)) t.router->handleChannelPressure(ch, pressure);
    }

    // Change a track's live subscription. Releases notes still held on the sources it's DROPPING
    // (this track's router only — clean note-offs, never a panic that could cut a song), then swaps
    // the mask. Pure field write + a few note-offs; the audio graph is untouched. This is the switch
    // the app drives via @TRK<i>.SRC and the zero-dropout acceptance test.
    static void setSources(Track &t, uint8_t newMask) {
        if (t.router) {
            uint8_t removed = (uint8_t)(t.liveSrcMask & ~newMask);
            for (uint8_t s = SrcDin; s < SrcCount; ++s) if (removed & (uint8_t)(1u << s))
                for (uint8_t i = 0; i < s_heldN[s]; ++i) t.router->handleNoteOff(s_held[s][i].ch, s_held[s][i].note, 0);
        }
        t.liveSrcMask = newMask;
    }
    // Wire<->mask helpers for @TRK<i>.SRC= and @STATE. The app picks a single device (or none/all).
    static uint8_t parseSrcMask(const char *a) {
        if (!strcmp(a, "none"))   return 0;
        if (!strcmp(a, "all"))    return srcMaskAllLocal();
        if (!strcmp(a, "din"))    return srcBit(SrcDin);
        if (!strcmp(a, "usb"))    return srcBit(SrcUsbHost);
        if (!strcmp(a, "bt"))     return srcBit(SrcBtMidi);
        if (!strcmp(a, "serial")) return srcBit(SrcSerial);
        return 0;
    }
    static const char *srcName(uint8_t mask) {
        switch (mask) {
            case 0:                       return "none";
            case (1u << SrcDin):          return "din";
            case (1u << SrcUsbHost):      return "usb";
            case (1u << SrcBtMidi):       return "bt";
            case (1u << SrcSerial):       return "serial";
            default:                      return "multi";   // >1 source subscribed
        }
    }
    static int chNum(uint16_t mask) {   // report the channel filter as a single number (0 = all)
        if (mask == 0) return 0;
        for (int c = 1; c <= 16; ++c) if (mask == (uint16_t)(1u << (c - 1))) return c;
        return 0;
    }
}

// Physical-input callbacks: each tags its source and hands off to the hub. Signatures match the
// MIDI.h (DIN) and MIDIDevice (USB host) setHandle* families so both register the same shapes.
#if TDSP_HAS_DIN_MIDI
static void dinNoteOn  (byte ch, byte note, byte vel) { midihub::noteOn (SrcDin, ch, note, vel); }
static void dinNoteOff (byte ch, byte note, byte vel) { midihub::noteOff(SrcDin, ch, note, vel); }
static void dinCC      (byte ch, byte cc,   byte val) { midihub::controlChange(SrcDin, ch, cc, val); }
static void dinPitch   (byte ch, int  bend)           { midihub::pitchBend(SrcDin, ch, bend); }
#endif
#if TDSP_HAS_USB_MIDI_HOST
static void usbHostNoteOn  (byte ch, byte note, byte vel) { midihub::noteOn (SrcUsbHost, ch, note, vel); }
static void usbHostNoteOff (byte ch, byte note, byte vel) { midihub::noteOff(SrcUsbHost, ch, note, vel); }
static void usbHostCC      (byte ch, byte cc,   byte val) { midihub::controlChange(SrcUsbHost, ch, cc, val); }
static void usbHostPitch   (byte ch, int  bend)           { midihub::pitchBend(SrcUsbHost, ch, bend); }
static void usbHostPressure(byte ch, byte pressure)       { midihub::channelPressure(SrcUsbHost, ch, pressure); }
#endif

// MPE member-channel pitch-bend range, in ONE-SIDED semitones (the router scales a full
// +-8192 wheel deflection to +-range). The LinnStrument's default per-note bend range is
// +-24 semitones (2 octaves each way) — match it so a full-surface slide reproduces two
// octaves, not the four that the old +-48 default produced. This also makes the Dexed pool
// stop clamping (its kBendRange is 24), so every backend now agrees on the range.
static constexpr float kMpeMemberBendRange = 24.0f;

// Switch the device between normal MIDI and MPE (per-note expression). Sets the router's
// per-channel bend range (+-2 normal vs the LinnStrument's +-24-semi MPE default) and lets
// the backend reconfigure (TSF frees ch10 from drums so it's an MPE member channel).
static void applyMidiMode(bool mpe) {
    g_mpeMode = mpe;
    float range = mpe ? kMpeMemberBendRange : 2.0f;   // +-24 (MPE, 2 octaves) vs +-2
    // Every voice's live-MIDI router gets the same bend range so an MPE controller's per-note
    // slides aren't clamped to +-2 semis on whichever synth it's subscribed to (voices 2/3 too).
    for (tdsp::MidiRouter &r : g_routerV) for (uint8_t ch = 1; ch <= 16; ch++) r.setPitchBendRange(ch, range);
    // MPE is single-timbre: a song's per-channel program changes shouldn't apply, so the
    // whole performance (and the MPE test song) uses the SELECTED instrument, not the file's.
    g_player.setProgramChangeEnabled(!mpe);
    // Reconfigure EVERY track's engine for the new mode via the vtable — the primary Dexed pool AND
    // the hetero OPLL/Plaits tracks (fixes: hetero engines used to stay in poly mode because only the
    // primary backend's synthSetMpeMode() was called). A new engine gets MPE by overriding setMpeMode.
    trackEnginesSetMpe(mpe);
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
#if TDSP_BOARDTEST
static void runBoardTest(Stream &out);        // @BOARDTEST: per-track + drums audibility + full +-24 bend capture (defn in BoardTest.inc.h)
static void runMpeInputTest(Stream &out);     // @MPETEST: same full-bend check driven through the real router/MPE input path
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

// Uniform per-track engine adapters + g_trackEngine[] inventory (additive; nothing reads it yet).
#include "TrackEngineImpl.inc.h"

// Track-indexed command dispatch (Phase 3): "@TRK<i>.<CMD>[=<arg>]" routes a UNIFORM interface
// to any track by index — g_tracks[0..n-1] (synth voices) or g_drumTrack (the drum). This is what
// lets the app drive N cards from the @STATE tracks[] array with one command family, so a future
// 4th/5th track needs no new command. Thin dispatch over the existing per-voice handlers; the legacy
// @SONG*/@SONG2*/@DRUM*/@ARP*/@DXPICK* stay. Covers transport (PLAY/RESTART/STOP/VOL/LOOP), the whole
// arp surface (ARP<any> -> handleArpLine), voice select (DXPICK/DXVOICE), and the live-MIDI
// subscription (SRC/SRCCH, Thread C). `reply` echoes confirmations back over the arriving link.
static void handleTrkCmd(const char* s, Stream& reply) {
    const char* dot = strchr(s, '.');
    if (!dot) return;
    const int i = atoi(s);                       // "<i>" before the dot
    const char* cmd = dot + 1;                   // "<CMD>[=<arg>]"
    const char* eq = strchr(cmd, '=');
    const char* arg = eq ? eq + 1 : "";
    const int nSynth = (int)(sizeof g_tracks / sizeof g_tracks[0]);
    Track* t = nullptr; bool isDrum = false;
    if (i >= 0 && i < nSynth) t = &g_tracks[i];
    else if (i == nSynth)     { t = &g_drumTrack; isDrum = true; }
    if (!t) return;
    // An idle voice (absorbed by the @POOL partition) can't launch a song — its engines belong to a
    // group lead, so playing would steal the lead's voices. Config commands (INSTR/SRC) still work.
    const bool launchCmd = (strncmp(cmd, "PLAY=", 5) == 0 || strncmp(cmd, "SONGF=", 6) == 0 || strncmp(cmd, "RESTART=", 8) == 0);
    if (launchCmd && !isDrum && !voiceLive(i)) { reply.printf("@TRK%d.PLAY=IDLE\n", i); return; }
    if      (strncmp(cmd, "PLAY=", 5) == 0 || strncmp(cmd, "SONGF=", 6) == 0) { const char* a = eq + 1; if (isDrum) drumStartFile(a); else trackLaunch(*t, a); }
    else if (strncmp(cmd, "RESTART=", 8) == 0) { if (isDrum) drumStartFile(arg); else trackRestart(*t, arg); }
    else if (strncmp(cmd, "STOP", 4) == 0)     { if (isDrum) drumStop(); else songStop(*t); }
    else if (strncmp(cmd, "VOL=", 4) == 0)     { if (t->setLevel) t->setLevel(atoi(arg)); }
    // List THIS track's engine instruments (index-ordered, \x1f-separated) so the app renders the
    // engine's OWN patch picker (OPLL ROM voices / Plaits models / any future engine) instead of the
    // Dexed cart library. Engine-agnostic via the ITrackEngine vtable — a new engine needs no new code.
    else if (strncmp(cmd, "INSTRS", 6) == 0) {
        ITrackEngine *e = isDrum ? g_drumTrackEngine : g_trackEngine[i];
        reply.printf("@TRK%d.INSTRS=", i);
        const int n = e->numInstruments();
        for (int k = 0; k < n; k++) { if (k) reply.write((uint8_t)0x1f); reply.print(e->instrumentName(k)); }
        reply.print("\n");
    }
    // Drum KIT select through the uniform track surface: @TRK<nSynth>.INSTR=<kit> -> the drum engine's
    // setInstrument (== setDrumKit), mirroring @DRUMKIT= but via the same @TRK<i> path the app uses for
    // synth voices — so a componentized per-track card drives the kit exactly like an instrument. Not
    // hetero-gated (works on any drum build); guarded on appliesKit() so a fixed-voice OPLL rhythm engine
    // (no GM kits) ignores it and just echoes. @DRUMKIT= stays for back-compat.
    else if (isDrum && strncmp(cmd, "INSTR=", 6) == 0) {
        if (g_drumTrackEngine->appliesKit()) g_drumTrackEngine->setInstrument(atoi(arg));
        reply.printf("@TRK%d.INSTR=%d\n", i, g_drumTrackEngine->instrument());
    }
#if TDSP_FX_SEND
    // Per-track reverb SEND level (0..100). Track index maps 1:1 onto the fxIn cascade slot
    // (synths -> fxIn1, drums -> fxIn2). Dry path is untouched. See planning/.../SEND_MATRIX.md.
    else if (strncmp(cmd, "FXSEND=", 7) == 0)  { fxSendApply(i, atoi(arg)); reply.printf("@TRK%d.FXSEND=%d\n", i, g_fxSend[i]); }
#endif
    else if (strncmp(cmd, "LOOP=", 5) == 0)    { if (!isDrum) { *t->loop = (atoi(arg) != 0); t->player->setLooping(*t->loop); } }   // a groove always loops
    // Whole arp surface for the track (ON/PAT/RATE/OCT/LATCH/GATE/SWING/PRESET/SEQ) — reuse the
    // shared arp parser with a synthetic "ARP" prefix so "@TRK<i>.ARPPAT=" -> handleArpLine "PAT=".
    else if (strncmp(cmd, "ARP", 3) == 0)      { if (t->arp) handleArpLine(cmd, reply, *t->arp, "ARP"); }
    // Live-MIDI input subscription (Thread C): which physical device feeds this synth + channel
    // filter. Pure field writes the hub reads per event — no audio-graph repatch, zero switch latency.
    else if (strncmp(cmd, "SRC=", 4) == 0)     { midihub::setSources(*t, midihub::parseSrcMask(arg)); reply.printf("@TRK%d.SRC=%s\n", i, midihub::srcName(t->liveSrcMask)); }
    else if (strncmp(cmd, "SRCCH=", 6) == 0)   { int n = atoi(arg); t->srcChMask = (n <= 0 || n > 16) ? 0 : (uint16_t)(1u << (n - 1)); reply.printf("@TRK%d.SRCCH=%d\n", i, midihub::chNum(t->srcChMask)); }
#if TDSP_HETERO
    // Engine-agnostic instrument select (Slot abstraction, Thread D): on a HETERO build a track may
    // be a Dexed pool window OR a melodic OPLL, so the app picks by index without knowing the engine
    // — INSTR=<idx> routes to the track's actual engine kind. For an OPLL voice INSTR/DXVOICE/DXPICK
    // are all handled here (the OPLL has no /dexed carts); a Dexed voice's DXVOICE/DXPICK fall through
    // to the unchanged Dexed block below. This whole block is HETERO-only so non-hetero builds are
    // byte-identical.
    // Engine-agnostic instrument select (migrated to the ITrackEngine vtable): @TRK<i>.INSTR=<idx>
    // (and OPLL's DXVOICE= alias, which has no cart library) route to whatever engine the track is —
    // the adapter forwards to the same per-kind functions the two hand-written branches used to call.
    else if (!isDrum && (strncmp(cmd, "INSTR=", 6) == 0 ||
             (voiceIsOpll(i) && strncmp(cmd, "DXVOICE=", 8) == 0))) {
        g_trackEngine[i]->setInstrument(atoi(strchr(cmd, '=') + 1));
        reply.printf("@TRK%d.INSTR=%d\n", i, g_trackEngine[i]->instrument());
    }
#if TDSP_OPLL_ENGINES >= 1
    else if (voiceIsOpll(i) && !isDrum && strncmp(cmd, "DXPICK=", 7) == 0) {
        // OPLL has no cart library — ignore the pick but echo so the app doesn't hang on a reply.
        reply.printf("@TRK%d.DXPICKED=\t0\t%s\n", i, heteroOpllInstrumentName(heteroOpllInstrument(i - kDexedVoices)));
    }
#endif
#endif
#if defined(TDSP_SYNTH_DEXED) || defined(TDSP_SYNTH_DEXED_POOL)
    // Voice select for this track. DXPICK=<relCart>\t<voice> (library pick) / DXVOICE=<idx> (bundled).
    else if (strncmp(cmd, "DXPICK=", 7) == 0 && !isDrum) {
        char buf[160]; strncpy(buf, arg, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
        int voice = 0; char* tab = strrchr(buf, '\t'); if (tab) { *tab = 0; voice = atoi(tab + 1); }
        const char* nm =
#if TDSP_SYNTH_VOICES >= 4 && !TDSP_HETERO
            synthPickCartVoiceV(i, buf, voice);
#elif TDSP_VOICE2
            (i == 1) ? synthPickCartVoice2(buf, voice) : synthPickCartVoice(buf, voice);
#else
            synthPickCartVoice(buf, voice);
#endif
        reply.printf("@TRK%d.DXPICKED=%s\t%d\t%s\n", i, buf, voice, nm ? nm : "?");
    }
    else if (strncmp(cmd, "DXVOICE=", 8) == 0 && !isDrum) {
        g_trackEngine[i]->setInstrument(atoi(arg));   // bundled-index select (vtable; DXPICK cart pick stays Dexed-specific)
    }
#endif
}

// `reply` is the stream the command arrived on (USB Serial or the ESP32 UART,
// Serial7). Typed as Stream (not just Print) so commands that need to READ back
// on the same link — e.g. @FXUP handing the stream to FlasherX — can do so.
FLASHMEM static bool handleControlLine(const char* line, Stream& reply) {
    if      (strncmp(line, "@VOL=", 5) == 0)      setMasterVolumePct(atoi(line + 5));
#ifdef TDSP_FLASHERX
    else if (strncmp(line, "@FXUP", 5) == 0)      fxRunUpdate(reply);    // OTA self-update on the arriving link (USB or ESP32/Serial7); blocks, reboots
#endif
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
    else if (strncmp(line, "@TRK", 4) == 0)       handleTrkCmd(line + 4, reply);   // @TRK<i>.<CMD>[=<arg>] — uniform per-track interface (Phase 3)
    else if (strncmp(line, "@SONGF=", 7) == 0)    trackLaunch(g_tracks[0], line + 7);     // @SONGF=<filename|name> (play by name — the app's path; bar-quantized if @QUANTIZE=1)
    else if (strncmp(line, "@SONGRESTART=", 13) == 0) trackRestart(g_tracks[0], line + 13);   // hard restart on a fresh downbeat — the app's Play / ‹ ›
    else if (strncmp(line, "@SONG=", 6) == 0) {
        if (strcmp(line + 6, "stop") == 0) songStop(g_tracks[0]);
        else trackStartIndex(g_tracks[0], atoi(line + 6));   // @SONG=<catalog index> (legacy; resolved via songs.ndjson)
    }
#if TDSP_VOICE2
    // --- Player 2 (voice-2 song player), so a second song plays at the same time. @SONG2F starts
    // immediately (no launch-quantize slot); it still locks to the running grid via sync. ---
    else if (strncmp(line, "@SONG2RESTART=", 14) == 0) trackRestart(g_tracks[1], line + 14);   // hard restart player 2 on a fresh downbeat
    else if (strncmp(line, "@SONG2F=", 8) == 0)   trackStartArg(g_tracks[1], line + 8);        // @SONG2F=<filename|name>
    else if (strncmp(line, "@SONG2=", 7) == 0)  { if (strcmp(line + 7, "stop") == 0) songStop(g_tracks[1]); }
    else if (strncmp(line, "@LOOP2=", 7) == 0)  { g_song2Loop = (atoi(line + 7) != 0);
                                 g_player2.setLooping(g_song2Loop);   // seamless self-loop (no re-arm) — takes effect mid-play
                                 Serial.printf("[song2] loop %s\n", g_song2Loop ? "ON" : "off"); }
#endif
    else if (strcmp(line, "@GETCAT") == 0)        refreshCatalog(reply);   // re-scan SD + send catalog
    else if (strcmp(line, "@REINDEX") == 0)       { tdsp::catdb::buildCatalog(engineCaps(), catdbWriteBundled, millis()); reply.println("@REINDEXED"); }  // rebuild /tdsp/*.ndjson DB (upsert)
#if defined(TDSP_DRUM_SD)
    // @DRUMRESCAN: re-scan /drums for kit folders WITHOUT a reflash (the SD sampler only scans at
    // boot). Picks up kits pushed live over @WB; if drums went 0 -> N it wires the ch10 route + marks
    // drums available, then rebuilds the catalog so the app's kit list refreshes. Reply carries the count.
    else if (strcmp(line, "@DRUMRESCAN") == 0) {
        g_drumSamplerSink.scanKits();
        const int n = g_drumSamplerSink.numKits();
        if (n > 0) {
            g_drumNoteMapper.setDownstream(&g_drumSamplerSink);      // idempotent (same wiring setup() does)
            g_drumPlayer.setSink(&g_drumNoteMapper); g_drumTrack.sink = &g_drumSamplerSink;
            g_drumSamplerSink.setKit(g_drumSamplerSink.currentKit());
            g_engineHasDrums = true;
        } else {
            g_engineHasDrums = false;
        }
        tdsp::catdb::buildCatalog(engineCaps(), catdbWriteBundled, millis());
        reply.printf("@DRUMRESCANNED\t%d\n", n);
    }
#endif
    else if (strncmp(line, "@READ=", 6) == 0)     streamFile(reply, line + 6);  // generic file fetch (catalog transport)
    else if (strncmp(line, "@LS=", 4) == 0) {                                    // generic recursive dir list: @LS=<path>[\x1f<ext>]
        char buf[160]; strncpy(buf, line + 4, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
        const char* ext = "";
        char* us = strchr(buf, '\x1f');
        if (us) { *us = 0; ext = us + 1; }
        streamDir(reply, buf, ext);
    }
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
#if defined(TDSP_DRUM_TSF)
    else if (strcmp(line, "@FONTS") == 0)          sendFonts(reply);             // list swappable drum fonts for the picker
    else if (strncmp(line, "@DRUMFONT=", 10) == 0) {                             // swap the resident drum SF2 at runtime
        if (drumFontSwap(line + 10)) {
            catdbWriteBundled();                          // refresh /tdsp/drumkits.ndjson for the new font's kit list
            sendFonts(reply);                             // updated fonts list (the current-flag moved)
            reply.printf("@DRUMFONT=%s\n", g_drumFontPath);   // ack -> app reloads the catalog (fresh kits) + @STATE
        } else {
            reply.printf("@DRUMFONTERR=%s\n", line + 10);
        }
    }
#endif
    else if (strncmp(line, "@DRUMMAP=", 9) == 0) {   // ch10 note-map mode: 0=GmReduce (fold Roland 22/26->42/46), 1=Passthrough
        // Passthrough is ONLY correct on a font that has real regions at 22/26 (a V-Drums/TD-11
        // kit); on a plain GM font it re-drops the edge hi-hats silent. Default GmReduce; this
        // is set automatically when an authentic drum font loads (see setup()), and exposed here
        // for diagnostics / an app toggle. See planning/drum-note-map/DESIGN.md.
        g_drumNoteMapper.setMode(atoi(line + 9) ? tdsp::DrumNoteMapper::Passthrough
                                                : tdsp::DrumNoteMapper::GmReduce);
        Serial.printf("@DRUMMAP=%d\n", (int)g_drumNoteMapper.mode());
    }
    else if (strncmp(line, "@DRUMVOL=", 9) == 0)    setDrumVol(atoi(line + 9));    // 0..150 %
#if TDSP_USB_AUDIO
    else if (strncmp(line, "@USBGAIN=", 9) == 0) {   // USB-in return level into the external sub-mix (0..150 %); host owns its own stream volume
        int v = atoi(line + 9); if (v < 0) v = 0; if (v > 150) v = 150;
        g_usbGainPct = v;
        extL.gain(2, v / 100.0f); extR.gain(2, v / 100.0f);   // slot 2 = USB-in on the external sub-mixer
        reply.printf("@USBGAIN=%d\n", v);
    }
#endif
    else if (strncmp(line, "@SONGVOL=", 9) == 0)    setSongVol(atoi(line + 9));    // 0..150 %, MIDI player level (independent of @VOL master)
    else if (strncmp(line, "@BPM=", 5) == 0)        setMasterBpm(atoi(line + 5));  // master tempo (song+drum)
    else if (strncmp(line, "@METROLOCK=", 11) == 0) {   // tempo lock: when ON, content stops auto-loading its BPM (the metronome lock-icon)
        g_tempoLock = (atoi(line + 11) != 0);
        reply.printf("@METROLOCK=%d\n", g_tempoLock ? 1 : 0);
        Serial.printf("[tempo] lock %s\n", g_tempoLock ? "ON (tempo held)" : "off (sole content sets BPM)");
    }
    else if (strncmp(line, "@DRUMSYNCHRO=", 13) == 0) { g_drumSynchro = (atoi(line + 13) != 0);   // start-on-first-note
                                 Serial.printf("[drum] synchro start %s\n", g_drumSynchro ? "ON (play a note to start)" : "off (start on Play)"); }
    else if (strncmp(line, "@QUANTIZE=", 10) == 0) {   // launch quantize: defer song/groove start to the next bar edge
        g_launchQuantize = (atoi(line + 10) != 0);
        if (!g_launchQuantize) { g_songLaunchPending = g_drumLaunchPending = false;
#if TDSP_VOICE2
                                 g_song2LaunchPending = false;
#endif
                               }   // dropping the mode cancels any armed launch
        reply.printf("@QUANTIZE=%d\n", g_launchQuantize ? 1 : 0);
        Serial.printf("[sync] launch quantize %s\n", g_launchQuantize ? "ON (starts land on the next bar)" : "off (start now)");
    }
    else if (strncmp(line, "@HPF=", 5) == 0)      setDacHpfMode(atoi(line + 5));
    else if (strncmp(line, "@LOOP=", 6) == 0)   { g_loop = (atoi(line + 6) != 0);
                                 g_player.setLooping(g_loop);   // seamless self-loop (no re-arm) — takes effect mid-play
                                 Serial.printf("[song] loop %s\n", g_loop ? "ON" : "off"); }
    else if (strncmp(line, "@SYNCPROBE=", 11) == 0) {   // 1 Hz drift probe: master beat vs each synced player's cursor
        g_syncProbe = (atoi(line + 11) != 0); g_syncProbeClock = 0;
        reply.printf("@SYNCPROBE=%d\n", g_syncProbe ? 1 : 0);
        Serial.printf("[sync] probe %s\n", g_syncProbe ? "ON (1 Hz)" : "off");
    }
    else if (strncmp(line, "@DRUMJIT=", 9) == 0) {   // drum-timing jitter probe: dispatch lateness + loop() gap
        g_jitProbe = (atoi(line + 9) != 0);
        g_drumPlayer.setJitterProbe(g_jitProbe);
        g_drumPlayer.resetJitter();
        g_maxLoopGapUs = 0; g_lastLoopUs = 0; g_jitClock = 0;
        reply.printf("@DRUMJIT=%d\n", g_jitProbe ? 1 : 0);
        Serial.printf("[jit] drum jitter probe %s\n", g_jitProbe ? "ON (1 Hz @JIT)" : "off");
    }
#ifdef TDSP_METRONOME
    else if (strncmp(line, "@METRO=", 7) == 0) {   // MASTER TRANSPORT play/stop (the metronome IS the clock)
        if (atoi(line + 7) != 0) transportPlay(); else transportStop();
        reply.printf("@METRO=%d\n", g_conductor.running() ? 1 : 0);
    }
    else if (strncmp(line, "@METROMUTE=", 11) == 0) {   // is the click AUDIBLE? (transport runs either way)
        metroSetMuted(atoi(line + 11) != 0);
        reply.printf("@METROMUTE=%d\n", g_metroMuted ? 1 : 0);
        Serial.printf("[metro] click %s\n", g_metroMuted ? "muted" : "on");
    }
    else if (strncmp(line, "@METROSIG=", 10) == 0) {   // metronome time signature = N beats/bar (accent on beat 1)
        int n = atoi(line + 10); if (n < 1) n = 1; if (n > 16) n = 16;
        g_metroBpb = (uint8_t)n;
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
    else if (strncmp(line, "@LOOPF=", 7) == 0) {              // load+play a loop .mid into a voice
        // @LOOPF=<v>\x1f<path>. A leading '/' path is used verbatim; otherwise the arg is a
        // /midi/loops catalog relpath (e.g. "cc0/Foo [bass].mid"). The loop plays at once,
        // bar-locked to the master clock, through voice v's own synth via its MidiLooper.
        const char *p   = line + 7;
        int         v   = atoi(p); if (v != 2) v = 1;
        const char *us  = strchr(p, '\x1f');
        const char *arg = us ? us + 1 : nullptr;
        tdsp::MidiLooper *L =
#if TDSP_VOICE2
            (v == 2) ? &g_loop2 :
#endif
            &g_loop1;
        char full[160] = {0};
        if (arg && arg[0] == '/')  snprintf(full, sizeof full, "%s", arg);
        else if (arg)              snprintf(full, sizeof full, "/midi/loops/%s", arg);
        bool ok = arg && loadLoopFileIntoLooper(full, L, /*play=*/true);
        reply.printf("@LOOPF=%d\x1f%s\n", v, ok ? "ok" : "err");
    }
#if TDSP_RECORDER_EDIT
    // Note editor round trip (planning/midi-editor/DESIGN.md §4). @RECDUMP streams voice v's
    // clip out (same @FB/@FD/@FE framing as @READ); @RECLOAD/@RD/@RECEND stream an edited
    // clip back in. The load decodes straight into the clip (no staging buffer) and, on
    // commit, re-anchors playback to the retained grid phase.
    else if (strncmp(line, "@RECDUMP=", 9) == 0) {             // stream voice v's clip to the app
        int v = atoi(line + 9); if (v != 2) v = 1;
        tdsp::MidiLooper *L =
#if TDSP_VOICE2
            (v == 2) ? &g_loop2 :
#endif
            &g_loop1;
        char name[16]; snprintf(name, sizeof(name), "mem:/loop%d", v);
        streamClip(reply, name, L->clip());
    }
    else if (strncmp(line, "@RECLOAD=", 9) == 0) {             // begin load: <v>\x1f<bytes>[\x1f<crc>]
        const char *p = line + 9;
        int  v  = atoi(p); if (v != 2) v = 1;
        const char *us = strchr(p, '\x1f');
        long bytes = us ? atol(us + 1) : 0;
        tdsp::MidiLooper *L =
#if TDSP_VOICE2
            (v == 2) ? &g_loop2 :
#endif
            &g_loop1;
        const tdsp::MidiLooper::State st = L->state();
        const long maxBytes = (long)(tdsp::kLoopClipHdrBytes + (uint32_t)L->maxEvents() * tdsp::kLoopEventBytes);
        if (st == tdsp::MidiLooper::Armed || st == tdsp::MidiLooper::Recording || st == tdsp::MidiLooper::Overdub) {
            reply.printf("@RECERR=%d\x1fbusy\n", v);          // never mutate a clip mid-capture
        } else if (bytes < (long)tdsp::kLoopClipHdrBytes || bytes > maxBytes) {
            reply.printf("@RECERR=%d\x1fsize\n", v);
        } else {
            g_recLoad.reset();
            g_recLoad.active   = true;
            g_recLoad.voice    = (uint8_t)v;
            g_recLoad.target   = L;
            g_recLoad.expected = (uint32_t)bytes;
            reply.printf("@RECOK=%d\x1f%ld\n", v, bytes);
        }
    }
    else if (strncmp(line, "@RD=", 4) == 0) {                  // one data frame: <v>\x1f<seq>\x1f<b64>
        const char *p  = line + 4;
        int         v  = atoi(p);
        const char *s1 = strchr(p, '\x1f');
        const char *s2 = s1 ? strchr(s1 + 1, '\x1f') : nullptr;
        if (g_recLoad.active && s2 && v == (int)g_recLoad.voice) {
            uint32_t seq = (uint32_t)atol(s1 + 1);
            if (seq != g_recLoad.nextSeq) g_recLoad.err = true;   // gap -> whole-load retry at @RECEND
            else { g_recLoad.feedB64(s2 + 1); g_recLoad.nextSeq++; }
        }
        // else: stray/duplicate frame from an aborted transfer — ignore silently.
    }
    else if (strncmp(line, "@RECEND=", 8) == 0) {              // commit the load
        int v = atoi(line + 8); if (v != 2) v = 1;
        if (!g_recLoad.active || v != (int)g_recLoad.voice) {
            reply.printf("@RECERR=%d\x1fnoload\n", v);
        } else if (g_recLoad.err || !g_recLoad.hdrDone || g_recLoad.evFill != 0 ||
                   g_recLoad.got != g_recLoad.expected) {
            g_recLoad.target->endClipLoad(false);             // finalize the partial clip, stay stopped
            g_recLoad.reset();
            reply.printf("@RECERR=%d\x1fbad\n", v);           // app re-sends (DESIGN §4.2)
        } else {
            g_recLoad.target->endClipLoad(true);              // re-anchor + resume in phase
            uint16_t n = g_recLoad.target->eventCount();
            g_recLoad.reset();
            reply.printf("@RECE=%d\x1f%u\n", v, n);
        }
    }
#endif  // TDSP_RECORDER_EDIT
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
#if TDSP_SYNTH_VOICES >= 4 && !TDSP_HETERO
    // Runtime pool partition (no reflash): redistribute the 8 engines among the 4 voices. 0=4 voices
    // (2 eng each), 1=2 voices (4 eng), 2=1 voice (8 eng), 3=4+2+2. Absorbed voices go silent: stop
    // their players + panic their arps + release live notes (voiceLive() then gates them off live MIDI).
    else if (strncmp(line, "@POOL=", 6) == 0) {
        poolSetPreset(atoi(line + 6));
        for (int v = 0; v < kSynthVoices; ++v) if (!poolVoiceActive(v)) {
            songStop(g_tracks[v]);                  // absorbed voice: stop its player
            if (g_tracks[v].arp) g_tracks[v].arp->panic();   // and clear its arp (poolApplyPartition already panicked the sink)
        }
        applyMidiMode(g_mpeMode);                   // re-apply per-channel bend range to the resized windows
        reply.printf("@POOL=%d\n", poolPreset());
    }
    else if (strcmp(line, "@POOL") == 0) reply.printf("@POOL=%d\n", poolPreset());   // query
#endif
#endif
#if defined(TDSP_SYNTH_SF2_TSF) && TDSP_DIAGNOSTICS
    else if (strncmp(line, "@PROOF=", 7) == 0)     runAxisProof(atoi(line + 7));   // capture 1 note w/ axis at full (0=press 1=timbre 2=bend 3=neutral)
#endif
    else if (strncmp(line, "@MIDIMODE=", 10) == 0) applyMidiMode(atoi(line + 10) != 0);
#if TDSP_FX
    // FX master insert (reverb): @FX / @FX.<PARAM>=<v>. Distinct from @FXUP (FlasherX).
    else if (handleFxCmd(line, reply)) { /* handled + echoed inside */ }
#endif
#ifdef TDSP_HAS_REPLAYGAIN
    else if (strncmp(line, "@RG=", 4) == 0) {          // ReplayGain master switch (Tier-1 + Tier-2)
        tdsp::g_replayGainOn = (atoi(line + 4) != 0);
        // Re-apply the Tier-1 audition trim under the new state — but only when NOT mid-song:
        // a playing song already neutralizes Tier-1, and its Tier-2 per-channel trims re-gate
        // on the next Program Change (gmProgramTrim() honors the switch), so re-selecting the
        // instrument here would needlessly stomp the song's per-channel programs.
        if (!g_player.isPlaying()) synthSetInstrument(g_synthInstrument);
#if defined(TDSP_SYNTH_DEXED_POOL) && TDSP_VOICE2
        // Every other voice carries its OWN Tier-1 trim, so re-gate them too — otherwise the toggle
        // would only re-gain synth A. Gain-only (no reload), so it's safe mid-play.
#if TDSP_SYNTH_VOICES >= 4 && !TDSP_HETERO
        for (int v = 1; v < kSynthVoices; ++v) synthReapplyVoiceTrimV(v);   // voices 1..3
#else
        synthReapplyVoice2Trim();
#endif
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
#if TDSP_BOARDTEST
    else if (strcmp(line, "@BOARDTEST") == 0) runBoardTest(reply);  // per-track + drums audibility + full +-24 bend (tools/board_bend_test.py)
    else if (strcmp(line, "@MPETEST") == 0)   runMpeInputTest(reply);  // same full-bend check via the real router/MPE input path
#endif
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
#if TDSP_SYNTH_VOICES < 4 && !TDSP_HETERO
    // The runtime Synth-B enable is a DYNAMIC 4/4 split (repatches engines). A fixed 4-way pool
    // (N=4) has no toggle — all four voices are always live — so this command is retired there.
    // A HETERO build ALSO retires it: its 2 Dexed voices are a PERMANENT split (forced on at boot),
    // because disabling it unifies the pool while Synth B's sink still targets engines 4..7 -> the
    // two voices then FIGHT over the same engines (audible as per-measure voice-stealing alternation).
    // So @VOICE2= is ignored on a hetero build; the split can never be turned off.
    else if (strncmp(line, "@VOICE2=", 8) == 0) {          // Synth B master enable (dynamic 4/4 pool split)
        bool on = (atoi(line + 8) != 0);
        if (on != g_voice2On) {
            if (!on) {
                // Disabling Synth B: engines 4..7 rejoin Synth A, so nothing may keep driving the
                // B sink or its notes collide with voice 1. Stop the B song player and silence
                // arp 2 BEFORE the pool reunifies (while engines 4..7 are still B's).
                songStop(g_tracks[1]);
#if TDSP_ARP2
                g_arpFilter2.panic();
#endif
            }
            g_voice2On = on;
            synthSetVoice2Enabled(on);   // reshape the pool: 4/4 split (on) or unified 8 engines (off)
#if TDSP_HAS_USB_MIDI_HOST
            // Keyboard follows the split via the SUBSCRIPTION model (a field write, not a router
            // repatch): move the USB-host source onto whichever voice is now active. setSources()
            // releases any held keyboard notes on the voice being left — no hang, no song cut. The
            // app's explicit @TRK<i>.SRC commands take over from here if it drives them.
            midihub::setSources(g_tracks[1], on ? (uint8_t)(g_tracks[1].liveSrcMask |  srcBit(SrcUsbHost))
                                                : (uint8_t)(g_tracks[1].liveSrcMask & ~srcBit(SrcUsbHost)));
            midihub::setSources(g_tracks[0], on ? (uint8_t)(g_tracks[0].liveSrcMask & ~srcBit(SrcUsbHost))
                                                : (uint8_t)(g_tracks[0].liveSrcMask |  srcBit(SrcUsbHost)));
#endif
        }
        reply.printf("@VOICE2=%d\n", g_voice2On ? 1 : 0);
    }
#endif  // TDSP_SYNTH_VOICES < 4 && !TDSP_HETERO (runtime @VOICE2 split toggle; hetero = permanent split)
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
        reply.printf("@STATE={\"vol\":%d,\"hpf\":%d,\"bpm\":%d,\"metrolock\":%d,\"loop\":%d,\"quant\":%d,", volPct, g_hpf, (int)(g_masterBpm + 0.5f), g_tempoLock ? 1 : 0, *g_tracks[0].loop ? 1 : 0, g_launchQuantize ? 1 : 0);
        reply.printf("\"arp\":{\"on\":%d,\"pat\":%d,\"rate\":%d,\"oct\":%d,\"latch\":%d},",
                     g_arpFilter.enabled() ? 1 : 0, (int)g_arpFilter.pattern(), (int)g_arpFilter.rate(),
                     g_arpFilter.octaveRange(), g_arpFilter.latch() ? 1 : 0);
        { Track &t = g_tracks[0];
          reply.printf("\"song\":{\"playing\":%d,\"p\":%d,\"sync\":%d,\"vol\":%d,\"name\":", t.player->isPlaying() ? 1 : 0, t.player->positionPermille(), t.player->isSynced() ? 1 : 0, g_songVolPct);
          tdsp::catdb::jsonStr(reply, t.name); reply.print("},"); }
        reply.printf("\"drums\":{\"kit\":%d,\"playing\":%d,\"sync\":%d,\"vol\":%d,\"map\":%d},", g_drumKit, g_drumPlayer.isPlaying() ? 1 : 0, g_drumPlayer.isSynced() ? 1 : 0, g_drumVolPct, (int)g_drumNoteMapper.mode());
#if defined(TDSP_DRUM_TSF)
        // Runtime drum-font swap: which SF2 is resident now (path + short display label), so the app's
        // Drum Font picker highlights the current one. caps.drumfontsel (below) gates the picker's visibility.
        reply.print("\"drumfont\":{\"path\":"); tdsp::catdb::jsonStr(reply, g_drumFontPath);
        reply.print(",\"display\":"); tdsp::catdb::jsonStr(reply, g_drumFontDisplay); reply.print("},");
#endif
#ifdef TDSP_METRONOME
        reply.printf("\"metro\":%d,\"metromuted\":%d,\"metrosig\":%d,\"metrovol\":%d,", g_conductor.running() ? 1 : 0, g_metroMuted ? 1 : 0, g_metroBpb, g_metroVolPct);   // transport running + click mute + time sig + click level
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
        // n1/n2 = event count per clip, max = the per-clip cap, so the editor can show
        // headroom (n/max) and disable Add near the limit (DESIGN §2.5).
        reply.printf("\"rec\":{\"v\":%d,\"bars1\":%d,\"st1\":%d,\"p1\":%d,\"n1\":%d,\"max\":%d",
                     g_recVoice, g_loop1.bars(), (int)g_loop1.state(), g_loop1.positionPermille(),
                     g_loop1.eventCount(), g_loop1.maxEvents());
#if TDSP_VOICE2
        reply.printf(",\"bars2\":%d,\"st2\":%d,\"p2\":%d,\"n2\":%d",
                     g_loop2.bars(), (int)g_loop2.state(), g_loop2.positionPermille(), g_loop2.eventCount());
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
#if TDSP_FX
        // FX (reverb) bus state — the app hydrates the Reverb card from this.
        reply.print("\"fx\":"); fxEmitJson(reply); reply.print(",");
#endif
#if TDSP_USB_AUDIO
        // USB Audio interface status — the app's USB Audio card reads this. "active" = the host
        // is streaming (pop_ok advanced since the last @STATE); gain = the USB-in return level;
        // rxover/txunder = ring over/underrun counters (host<->codec clock-drift diagnostics).
        { AudioInputUSB_F32::Status st = AudioInputUSB_F32::getStatus();
          uint32_t nowPop = AudioInputUSB_F32::pop_ok;
          bool act = (nowPop != g_usbLastPop); g_usbLastPop = nowPop;
          reply.printf("\"usb\":{\"active\":%d,\"gain\":%d,\"rxover\":%u,\"txunder\":%u},",
                       act ? 1 : 0, g_usbGainPct, (unsigned)st.rx_overruns, (unsigned)st.tx_underruns); }
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
        { Track &t = g_tracks[1];
          reply.printf(",\"song2\":{\"playing\":%d,\"p\":%d,\"sync\":%d,\"loop\":%d,\"name\":",
                       t.player->isPlaying() ? 1 : 0, t.player->positionPermille(), t.player->isSynced() ? 1 : 0, *t.loop ? 1 : 0);
          tdsp::catdb::jsonStr(reply, t.name); reply.print("}"); }
#endif
        // tracks[] — the compiled Track INVENTORY as data (Phase 3): one entry per Track so the app can
        // render a card per track (kind/name/playing/on/arp) instead of hardcoding Synth A/B/Drums.
        // Additive alongside the per-voice keys above; the app migrates to this, the old keys retire
        // later. "on" = the track is currently usable (voice 2 enabled / engine renders ch10 drums).
        // Each synth entry also carries its live-MIDI subscription (Thread C): "src" = which input
        // device feeds it ("din"/"usb"/"bt"/"serial"/"none"/"multi"), "srcch" = channel filter (0=all).
        reply.print(",\"tracks\":[");
        // ONE loop over the track inventory (the ITrackEngine vtable): every synth track emits
        // identically — its engine kind (engTag()), current voice display name (currentName()), and
        // player/live-MIDI state — so a build with ANY mix of engines (incl. no Dexed) needs no
        // per-kind block here. This is what removes the "Dexed is the primary" assumption from @STATE.
        // Wire-compat: Dexed's engTag() is "" so it stays absent exactly as before; OPLL/Plaits/… carry
        // their tag. "on"/"arp" are Track-level: voice 1 gates on @VOICE2 / TDSP_ARP2, others always on.
        for (int v = 0; v < kSynthVoices; ++v) {
            Track &t = g_tracks[v];
            int on = 1, arp = 1;
#if TDSP_VOICE2
            if (v == 1) { on = ((TDSP_SYNTH_VOICES >= 4) || g_voice2On) ? 1 : 0; arp = TDSP_ARP2 ? 1 : 0; }
#endif
            reply.printf("%s{\"i\":%d,\"kind\":\"synth\",\"playing\":%d,\"on\":%d,\"arp\":%d,\"src\":\"%s\",\"srcch\":%d",
                         v ? "," : "", v, t.player->isPlaying() ? 1 : 0, on, arp,
                         midihub::srcName(t.liveSrcMask), midihub::chNum(t.srcChMask));
            const char *eng = g_trackEngine[v]->engTag();
            if (eng[0]) { reply.print(",\"eng\":"); tdsp::catdb::jsonStr(reply, eng); }
            // ninstr = this engine's instrument count, so the app can step ‹/› with wrap and knows to
            // show the engine's OWN patch list (via @TRK<i>.INSTRS) instead of the Dexed cart library.
            reply.printf(",\"ninstr\":%d", g_trackEngine[v]->numInstruments());
            reply.print(",\"name\":"); tdsp::catdb::jsonStr(reply, g_trackEngine[v]->currentName());
            reply.print(",\"song\":"); tdsp::catdb::jsonStr(reply, t.name);
#if TDSP_FX_SEND
            reply.printf(",\"fxsend\":%d", g_fxSend[v]);
#endif
            reply.print("}");
        }
        // The drum track emitted through the SAME ITrackEngine vtable (its DrumTrackEngineAdapter) as the
        // synth tracks: kind:"drum" plus engTag()/ninstr/currentName() exactly like a synth entry, so the
        // app's per-track hydrate treats it uniformly — ninstr (the kit count) lets a componentized track
        // card step kits the same way it steps voices. "playing"/"on"/src stay direct reads (Track-level,
        // no vtable). Wire-compat: the drum's engTag() is "" so no "eng" appears, as before.
        reply.printf(",{\"i\":%d,\"kind\":\"drum\",\"playing\":%d,\"on\":%d,\"arp\":0,\"src\":\"%s\",\"srcch\":%d",
                     kSynthVoices, g_drumTrack.player->isPlaying() ? 1 : 0, drumEngineOk() ? 1 : 0,
                     midihub::srcName(g_drumTrack.liveSrcMask), midihub::chNum(g_drumTrack.srcChMask));
        {
            const char *deng = g_drumTrackEngine->engTag();
            if (deng[0]) { reply.print(",\"eng\":"); tdsp::catdb::jsonStr(reply, deng); }
            reply.printf(",\"ninstr\":%d", g_drumTrackEngine->numInstruments());
            reply.print(",\"name\":"); tdsp::catdb::jsonStr(reply, g_drumTrackEngine->currentName());
        }
#if TDSP_FX_SEND
        reply.printf(",\"fxsend\":%d", g_fxSend[kSynthVoices]);
#endif
        reply.print("}]");
        // Build-time capabilities so the app SHOWS the Voices-2 / arp-2 cards only on builds
        // that have them compiled in (both are pool-only, build-flag gated). caps.tracks = the
        // compiled track count (2 synth voices + 1 drum, or 1+1 on a non-voice2 build).
        // caps.audioloop = the number of audio loops that ACTUALLY allocated (0 = the board
        // couldn't spare the RAM -> the app hides the card), not just the build flag.
        // caps.drumfontsel: the app shows the Drum Font picker only when this build has the sampled drum
        // TSF AND the card carries more than one swappable font (else there's nothing to pick between).
        int drumFontSel = 0;
#if defined(TDSP_DRUM_TSF)
        drumFontSel = (g_drumFontCount > 1) ? 1 : 0;
#endif
        reply.printf(",\"caps\":{\"voice2\":%d,\"arp2\":%d,\"rec\":%d,\"recedit\":%d,\"tracks\":%d,\"audioloop\":%d,\"fx\":%d,\"usbaudio\":%d,\"drumkitsel\":%d,\"drumfontsel\":%d}",
                     TDSP_VOICE2 ? 1 : 0, (TDSP_VOICE2 && TDSP_ARP2) ? 1 : 0, TDSP_RECORDER ? 1 : 0,
                     TDSP_RECORDER_EDIT ? 1 : 0, kSynthVoices + 1,   // N synth voices + the drum track
#if TDSP_AUDIOLOOP
                     (int)g_aloopN
#else
                     0
#endif
                     , TDSP_FX ? 1 : 0   // caps.fx: the app shows the Reverb card only on FX builds
                     , TDSP_USB_AUDIO ? 1 : 0   // caps.usbaudio: the app shows the USB Audio card only on USB-audio builds
                     , kDrumKitSelectable ? 1 : 0   // caps.drumkitsel: 0 = fixed drum voice (OPLL) -> app hides the GM kit picker
                     , drumFontSel   // caps.drumfontsel: >1 swappable drum font on the card
                     );
        // PSRAM presence (top-level, MB): the app greys PSRAM-dependent cards with a reason when 0.
        reply.printf(",\"psram\":%u", (unsigned)external_psram_size);
        // Reasons for features that were BUILT (compiled in) but are currently UNAVAILABLE, so the app
        // can GREY the card (not hide it) and show WHY. A feature appears here only when its code is
        // present yet it failed to come up — almost always a missing resource (PSRAM, an SD font). The
        // app keys each card's grey-out on its own name in this map; absent name = normal/available.
        { bool any = false;
          reply.print(",\"unavail\":{");
#if TDSP_AUDIOLOOP
          if (g_aloopN == 0) {   // built but no loop buffer allocated (LEAN_RAM / no PSRAM)
              reply.printf("%s\"audioloop\":", any ? "," : ""); any = true;
              tdsp::catdb::jsonStr(reply, external_psram_size ? "Out of RAM" : "PSRAM required");
          }
#endif
#ifdef TDSP_DRUM_TSF
          if (!drumEngineOk()) {   // built but the sampled drum font didn't load (no PSRAM / missing /sf2)
              reply.printf("%s\"drums\":", any ? "," : ""); any = true;
              tdsp::catdb::jsonStr(reply, external_psram_size ? "Missing /sf2 drum font" : "PSRAM required");
          }
#endif
          (void)any;
          reply.print("}");
        }
#if TDSP_HETERO
        // The compiled engine INVENTORY (Thread D), a top-level @STATE field (kept OUT of "caps" so the
        // shared caps printf stays byte-identical on non-hetero builds): how many voices of each engine
        // kind, so the app knows this board is heterogeneous and which track index maps to which engine
        // (tracks [0,dexed) = Dexed pool windows, the next opll = melodic OPLL). Slot binding is a build fact.
        reply.printf(",\"engines\":{\"dexed\":%d,\"opll\":%d}", kDexedVoices, kSynthVoices - kDexedVoices);
#endif
#if defined(TDSP_SYNTH_DEXED_POOL) && TDSP_SYNTH_VOICES >= 4 && !TDSP_HETERO
        // Runtime pool partition (@POOL, no reflash): the current preset + per-voice engine count +
        // which voices are active (a lead absorbs its group; absorbed voices are idle). The app uses
        // "active" to grey absorbed cards, "preset" for the picker, "engines" to show polyphony
        // (engines*2 = normal poly, engines = MPE notes). Top-level (out of caps) so it's additive.
        reply.printf(",\"pool\":{\"preset\":%d,\"engines\":[%d,%d,%d,%d],\"active\":[%d,%d,%d,%d]}",
                     poolPreset(), poolVoiceEngines(0), poolVoiceEngines(1), poolVoiceEngines(2), poolVoiceEngines(3),
                     poolVoiceActive(0) ? 1 : 0, poolVoiceActive(1) ? 1 : 0, poolVoiceActive(2) ? 1 : 0, poolVoiceActive(3) ? 1 : 0);
#endif
        reply.print("}\n");   // close root object
        reply.printf("@APP=%s\n", g_appState);   // opaque app-owned state, emitted with @STATE so one connect rehydrates both
    }
    else return false;
    return true;
}

#if TDSP_HETERO
// Bind a track's per-index SONG STATE (follow/buf/name/loop/tempo/…) from the i-th backing slot, so a
// hetero voice can sit at ANY track index instead of being forced onto the hardcoded index-2/3 (*3/*4)
// slots — the per-track-state coupling that blocked a 0-Dexed inventory (planning/multi-engine-tracks).
// The per-slot scalars are UNCHANGED (aliases into them here); index i == the track's own index resolves
// to the SAME slot the old hardcoded assignment used, so current builds are byte-identical. Only the
// index-uniform SONG-STATE fields are set here; engine/router/sink/level/caps stay per-block (per-role).
// Table guards mirror the scalar #ifs (slot1 = VOICE2, slot2 = VOICES>=3, slot3 = VOICES>=4), so the
// table has exactly kSynthVoices entries.
FLASHMEM static void bindTrackSongState(Track &t, int i) {
    tdsp::PlayerFollower* const followTbl[kSynthVoices] = { &g_songFollow,
#if TDSP_VOICE2
        &g_songFollow2,
#endif
#if TDSP_SYNTH_VOICES >= 3
        &g_songFollow3,
#endif
#if TDSP_SYNTH_VOICES >= 4
        &g_songFollow4,
#endif
    };
    tdsp::MidiFileEvent* const bufTbl[kSynthVoices] = { g_buf,
#if TDSP_VOICE2
        g_buf2,
#endif
#if TDSP_SYNTH_VOICES >= 3
        g_buf3,
#endif
#if TDSP_SYNTH_VOICES >= 4
        g_buf4,
#endif
    };
    const int bufCapTbl[kSynthVoices] = { MAX_EVENTS,
#if TDSP_VOICE2
        MAX_EVENTS2,
#endif
#if TDSP_SYNTH_VOICES >= 3
        MAX_EVENTS3,
#endif
#if TDSP_SYNTH_VOICES >= 4
        MAX_EVENTS3,
#endif
    };
    char* const nameTbl[kSynthVoices] = { g_curSongName,
#if TDSP_VOICE2
        g_curSong2Name,
#endif
#if TDSP_SYNTH_VOICES >= 3
        g_curSong3Name,
#endif
#if TDSP_SYNTH_VOICES >= 4
        g_curSong4Name,
#endif
    };
    char* const argTbl[kSynthVoices] = { g_curSongArg,
#if TDSP_VOICE2
        g_curSong2Arg,
#endif
#if TDSP_SYNTH_VOICES >= 3
        g_curSong3Arg,
#endif
#if TDSP_SYNTH_VOICES >= 4
        g_curSong4Arg,
#endif
    };
    bool* const loopTbl[kSynthVoices] = { &g_loop,
#if TDSP_VOICE2
        &g_song2Loop,
#endif
#if TDSP_SYNTH_VOICES >= 3
        &g_song3Loop,
#endif
#if TDSP_SYNTH_VOICES >= 4
        &g_song4Loop,
#endif
    };
    bool* const wasTbl[kSynthVoices] = { &g_songWasPlaying,
#if TDSP_VOICE2
        &g_song2WasPlaying,
#endif
#if TDSP_SYNTH_VOICES >= 3
        &g_song3WasPlaying,
#endif
#if TDSP_SYNTH_VOICES >= 4
        &g_song4WasPlaying,
#endif
    };
    float* const bpmTbl[kSynthVoices] = { &g_songBpm,
#if TDSP_VOICE2
        &g_song2Bpm,
#endif
#if TDSP_SYNTH_VOICES >= 3
        &g_song3Bpm,
#endif
#if TDSP_SYNTH_VOICES >= 4
        &g_song4Bpm,
#endif
    };
    uint8_t* const bpbTbl[kSynthVoices] = { &g_songBpb,
#if TDSP_VOICE2
        &g_song2Bpb,
#endif
#if TDSP_SYNTH_VOICES >= 3
        &g_song3Bpb,
#endif
#if TDSP_SYNTH_VOICES >= 4
        &g_song4Bpb,
#endif
    };
    double* const loopBeatsTbl[kSynthVoices] = { &g_songLoopBeats,
#if TDSP_VOICE2
        &g_song2LoopBeats,
#endif
#if TDSP_SYNTH_VOICES >= 3
        &g_song3LoopBeats,
#endif
#if TDSP_SYNTH_VOICES >= 4
        &g_song4LoopBeats,
#endif
    };
    bool* const launchTbl[kSynthVoices] = { &g_songLaunchPending,
#if TDSP_VOICE2
        &g_song2LaunchPending,
#endif
#if TDSP_SYNTH_VOICES >= 3
        &g_song3LaunchPending,
#endif
#if TDSP_SYNTH_VOICES >= 4
        &g_song4LaunchPending,
#endif
    };
    t.follow = followTbl[i]; t.buf = bufTbl[i]; t.bufCap = bufCapTbl[i];
    t.name = nameTbl[i]; t.arg = argTbl[i]; t.loop = loopTbl[i]; t.wasPlaying = wasTbl[i];
    t.bpm = bpmTbl[i]; t.bpb = bpbTbl[i]; t.loopBeats = loopBeatsTbl[i]; t.launchPending = launchTbl[i];
}
#endif  // TDSP_HETERO

// Populate g_tracks[] — pointers to the per-voice objects declared at file scope + caps. Called
// once from setup(). The three caps flags carry the deliberately voice-1-ONLY behaviors (global
// MPE mode, meter ownership, the special prep); voice 2 leaves them false (its current behavior).
FLASHMEM static void tracksInit() {
    Track &t0 = g_tracks[0];
    t0.player = &g_player; t0.arp = &g_arpFilter; t0.router = &g_router; t0.follow = &g_songFollow;
#if TDSP_RECORDER
    t0.looper = &g_loop1;
#else
    t0.looper = nullptr;
#endif
    t0.sink = g_synthSink; t0.buf = g_buf; t0.bufCap = MAX_EVENTS; t0.chMask = tdsp::MidiFilePlayer::kMaskNoDrums;
    t0.setLevel = setSongVol; t0.tag = "song";
    t0.caps = { /*ownsGlobalMode*/true, /*ownsMeter*/true, /*prepSpecial*/true, /*splitGuarded*/false,
                /*loopsSeamless*/false, /*ownsPatch*/false, /*drumGated*/false, /*appliesKit*/false,
                /*mutesSongDrums*/false, /*tempoSourceWhenIdle*/true };   // a song is content: it sets the master BPM when it's the sole thing playing
    t0.name = g_curSongName; t0.arg = g_curSongArg; t0.loop = &g_loop; t0.wasPlaying = &g_songWasPlaying;
    t0.bpm = &g_songBpm; t0.bpb = &g_songBpb; t0.loopBeats = &g_songLoopBeats; t0.launchPending = &g_songLaunchPending;
    t0.liveSrcMask = srcMaskAllLocal(); t0.srcChMask = 0;   // voice 0 hears BOTH local inputs (DIN + USB) by default
#if TDSP_VOICE2
    Track &t1 = g_tracks[1];
    t1.player = &g_player2;
#if TDSP_ARP2
    t1.arp = &g_arpFilter2;
#else
    t1.arp = nullptr;
#endif
    t1.router = &g_kbdRouter; t1.follow = &g_songFollow2;
#if TDSP_RECORDER
    t1.looper = &g_loop2;
#else
    t1.looper = nullptr;
#endif
    t1.sink = g_synthSinkB; t1.buf = g_buf2; t1.bufCap = MAX_EVENTS2; t1.chMask = tdsp::MidiFilePlayer::kMaskNoDrums;
    t1.setLevel = synthSetVoice2Vol; t1.tag = "song2";
    t1.caps = { false, false, false, /*splitGuarded*/true,
                /*loopsSeamless*/false, /*ownsPatch*/false, /*drumGated*/false, /*appliesKit*/false,
                /*mutesSongDrums*/false, /*tempoSourceWhenIdle*/true };   // voice-2 song also sets the master BPM when it's the sole content
    t1.name = g_curSong2Name; t1.arg = g_curSong2Arg; t1.loop = &g_song2Loop; t1.wasPlaying = &g_song2WasPlaying;
    t1.bpm = &g_song2Bpm; t1.bpb = &g_song2Bpb; t1.loopBeats = &g_song2LoopBeats; t1.launchPending = &g_song2LaunchPending;
    t1.liveSrcMask = 0; t1.srcChMask = 0;   // idle until @VOICE2 moves the keyboard here, or @TRK1.SRC assigns a device
#endif
#if TDSP_SYNTH_VOICES >= 4 && !TDSP_HETERO
    // Voices 3/4 (indices 2/3) of the fixed 4-way Dexed pool: full Track peers of voices 0/1, each
    // its own player/arp/router/sink/bus/level. No split-guard (the 4-way split is always active).
    // Loopers are null initially (the MIDI recorder targets voices 0/1). liveSrcMask=0: idle until
    // the app assigns a device via @TRK2/3.SRC. Both are melodic (no drum/mode/meter ownership).
    { Track &t2 = g_tracks[2];
      t2.player = &g_playerV[2]; t2.arp = &g_arpFilterV[2]; t2.router = &g_routerV[2]; t2.follow = &g_songFollow3; t2.looper = nullptr;
      t2.sink = g_synthSink2; t2.buf = g_buf3; t2.bufCap = MAX_EVENTS3; t2.chMask = tdsp::MidiFilePlayer::kMaskNoDrums;
      t2.setLevel = synthSetVoice3Vol; t2.tag = "song3";
      t2.caps = { false, false, false, /*splitGuarded*/false,
                  false, false, false, false, false, /*tempoSourceWhenIdle*/true };
      t2.name = g_curSong3Name; t2.arg = g_curSong3Arg; t2.loop = &g_song3Loop; t2.wasPlaying = &g_song3WasPlaying;
      t2.bpm = &g_song3Bpm; t2.bpb = &g_song3Bpb; t2.loopBeats = &g_song3LoopBeats; t2.launchPending = &g_song3LaunchPending;
      t2.liveSrcMask = 0; t2.srcChMask = 0; }
    { Track &t3 = g_tracks[3];
      t3.player = &g_playerV[3]; t3.arp = &g_arpFilterV[3]; t3.router = &g_routerV[3]; t3.follow = &g_songFollow4; t3.looper = nullptr;
      t3.sink = g_synthSink3; t3.buf = g_buf4; t3.bufCap = MAX_EVENTS3; t3.chMask = tdsp::MidiFilePlayer::kMaskNoDrums;
      t3.setLevel = synthSetVoice4Vol; t3.tag = "song4";
      t3.caps = { false, false, false, /*splitGuarded*/false,
                  false, false, false, false, false, /*tempoSourceWhenIdle*/true };
      t3.name = g_curSong4Name; t3.arg = g_curSong4Arg; t3.loop = &g_song4Loop; t3.wasPlaying = &g_song4WasPlaying;
      t3.bpm = &g_song4Bpm; t3.bpb = &g_song4Bpb; t3.loopBeats = &g_song4LoopBeats; t3.launchPending = &g_song4LaunchPending;
      t3.liveSrcMask = 0; t3.srcChMask = 0; }
#elif TDSP_HETERO
    // HETEROGENEOUS inventory (Thread D): the voices after the Dexed ones (indices kDexedVoices..)
    // are MELODIC OPLL engines, NOT Dexed pool windows — each a full Track peer of the Dexed voices
    // 0/1 with its own player/arp/router, but its sink is an OPLL (HeteroOpll.h). This is the
    // per-track engine binding the thread exists to prove: switching @TRK<i>'s input feeds the SAME
    // uniform stack into a different engine kind. OPLL #0 reuses the index-2 extra-voice state
    // (g_songFollow3/g_buf3/g_curSong3*); OPLL #1 (if built, TDSP_OPLL_ENGINES>=2) reuses the
    // index-3 state (g_songFollow4/g_buf4/g_curSong4*).
#if TDSP_OPLL_ENGINES >= 1
    { const int idx = kDexedVoices; Track &t2 = g_tracks[idx];
      t2.player = &g_playerV[idx]; t2.arp = &g_arpFilterV[idx]; t2.router = &g_routerV[idx]; t2.looper = nullptr;
      t2.sink = g_hoOpllVoiceSink[0]; t2.chMask = tdsp::MidiFilePlayer::kMaskNoDrums;
      t2.setLevel = heteroOpllSetVol0; t2.tag = "song3";
      t2.caps = { false, false, false, /*splitGuarded*/false,
                  false, false, false, false, false, /*tempoSourceWhenIdle*/true };
      bindTrackSongState(t2, idx);   // follow/buf/name/loop/tempo BY INDEX (== old *3 slot at idx 2; index-flexible for 0-Dexed)
      t2.liveSrcMask = 0; t2.srcChMask = 0; }
#endif
#if TDSP_OPLL_ENGINES >= 2
    { const int idx = kDexedVoices + 1; Track &t3 = g_tracks[idx];
      t3.player = &g_playerV[idx]; t3.arp = &g_arpFilterV[idx]; t3.router = &g_routerV[idx]; t3.looper = nullptr;
      t3.sink = g_hoOpllVoiceSink[1]; t3.chMask = tdsp::MidiFilePlayer::kMaskNoDrums;
      t3.setLevel = heteroOpllSetVol1; t3.tag = "song4";
      t3.caps = { false, false, false, /*splitGuarded*/false,
                  false, false, false, false, false, /*tempoSourceWhenIdle*/true };
      bindTrackSongState(t3, idx);   // BY INDEX (== old *4 slot at idx 3)
      t3.liveSrcMask = 0; t3.srcChMask = 0; }
#endif
#if TDSP_HETERO_PLAITS
    // Melodic PLAITS track (generalized inventory): after the Dexed + OPLL voices, at index
    // pv = kDexedVoices + kOpllVoices. Its per-track song state is now bound BY INDEX via
    // bindTrackSongState() (the *3/*4 slots are no longer hardcoded) — so the Plaits track can sit at
    // ANY index, including index 0/1 in a 0-Dexed inventory. Guard: pv must be a real synth-voice index.
    static_assert((kDexedVoices + kOpllVoices) < kSynthVoices,
                  "HeteroPlaits track index (DEXED+OPLL) must be < kSynthVoices");
    { const int pv = kDexedVoices + kOpllVoices;
      Track &tp = g_tracks[pv];
      tp.player = &g_playerV[pv]; tp.arp = &g_arpFilterV[pv]; tp.router = &g_routerV[pv];
      tp.looper = nullptr; tp.sink = g_hpVoiceSink;
      tp.chMask = tdsp::MidiFilePlayer::kMaskNoDrums; tp.setLevel = heteroPlaitsSetVol;
      tp.caps = { false, false, false, false, false, false, false, false, false, /*tempoSourceWhenIdle*/true };
      tp.tag = (pv == 2) ? "song3" : "song4";   // cosmetic log label (unchanged for current builds)
      bindTrackSongState(tp, pv);   // follow/buf/name/loop/tempo BY INDEX (was the pv==2/3 *3/*4 special-case)
      tp.liveSrcMask = 0; tp.srcChMask = 0; }
#endif
#endif

    // Drum track (Phase 2): a Track peer of the voices, bound to the looping ch10 groove player.
    // sink is set to the real drum sink (g_drumTsfSink/g_drumVoiceSink/g_synthSink) in setup() where
    // the drum engine is brought up. arp/router/looper are null (a groove has no live input or arp).
    Track &td = g_drumTrack;
    td.player = &g_drumPlayer; td.arp = nullptr; td.router = nullptr; td.follow = &g_drumFollow; td.looper = nullptr;
    td.sink = g_synthSink; td.buf = g_drumBuf; td.bufCap = MAX_DRUM_EVENTS; td.chMask = (uint16_t)(1u << 9);
    td.setLevel = setDrumVol; td.tag = "drum";
    td.caps = { /*ownsGlobalMode*/false, /*ownsMeter*/true, /*prepSpecial*/false, /*splitGuarded*/false,
                /*loopsSeamless*/true, /*ownsPatch*/true, /*drumGated*/true, /*appliesKit*/true,
                /*mutesSongDrums*/true, /*tempoSourceWhenIdle*/true };
    td.name = g_curDrumName; td.arg = g_curDrumArg; td.loop = &g_drumLoop; td.wasPlaying = &g_drumWasPlaying;
    td.bpm = &g_drumFileBpm; td.bpb = &g_drumBpb; td.loopBeats = &g_drumLoopBeats; td.launchPending = &g_drumLaunchPending;
    td.liveSrcMask = 0; td.srcChMask = 0;   // a groove has no live input (router is null)
}

// Wire one track's MIDI graph (unified — replaces the parallel voice-1/voice-2 hookup blocks in
// setup()). Uniform routing for every track:  live-MIDI router ─┐
//                                                   song player ─┴→ [arp] ─→ sink  (+ looper tap)
// The arp forwards verbatim in bypass, so with it the router+player feed the arp and the arp fans to
// the sink; without one (no TDSP_ARP2 on voice 2) they feed the sink directly. The looper taps the
// baked post-arp stream (arp downstream when present, else the router). Call BEFORE synthBegin (which
// then overrides voice 1's channel mask for a drum-capable engine) — so both players start at the
// no-drums default here, matching today's per-voice code. Followers are registered separately, after
// g_conductor.begin(). Drums are NOT a track yet (P2 folds g_drumPlayer in here).
FLASHMEM static void trackWireSetup(Track &t) {
    if (t.arp) {
        t.arp->setClock(&g_conductor.clock());
        t.arp->addDownstream(t.sink);
        t.router->addSink(t.arp);
    } else {
        t.router->addSink(t.sink);
    }
#if TDSP_RECORDER
    if (t.looper) {
        t.looper->begin(&g_conductor.clock(), t.sink);   // play the loop back into the sink directly
        if (t.arp) t.arp->addDownstream(t.looper);        // capture the BAKED (post-arp) stream
        else       t.router->addSink(t.looper);
    }
#endif
    // The song player goes THROUGH the arp too (parity; bypassed = normal playback), which also lands
    // its notes on the arp downstream where the looper taps — else straight to the sink.
    t.player->setSink(t.arp ? static_cast<tdsp::MidiSink*>(t.arp) : t.sink);
    // Melodic voice: skip ch10 and never panic it (so a song stop/restart never cuts a ch10 groove).
    // synthBegin() re-opens voice 1's mask to kMaskAll on a drum-capable engine (voice 2 stays melodic).
    t.player->setChannelMask(tdsp::MidiFilePlayer::kMaskNoDrums);
    t.player->setPanicMask(tdsp::MidiFilePlayer::kMaskNoDrums);
}

FLASHMEM void setup() {
    hardResetCodecPower();
    tracksInit();
    trackEnginesInit();   // bind each track to its ITrackEngine (additive; call sites migrate onto it next)

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

    // SD card (Teensy 4.1 built-in slot): scan /midi/songs/**.mid so songs can be added
    // by copying files to the card. Falls back to the built-in songs if no card.
#if TDSP_HAS_SDCARD
    // Retry SD.begin a few times: a card can need a moment after power-up, so a single
    // attempt at boot often false-reports "no card" for a perfectly good card (it then
    // mounts on a later @GETCAT/Refresh). Looping here mounts it at boot instead.
    for (int i = 0; i < 10 && !g_sdReady; ++i) { g_sdReady = SD.begin(BUILTIN_SDCARD); if (!g_sdReady) delay(40); }
    Serial.printf("[sd] card %s\n", g_sdReady ? "ready" : "not present");
    // Nested MIDI layout: /midi/{songs,drums,loops,tests}. Create the tree at boot so a
    // fresh card + the @WB push / MTP drop / @LS browser all have a home. (/midi first,
    // then children — some SD.mkdir impls don't auto-create parents.)
    if (g_sdReady) {
        if (!SD.exists("/midi"))       SD.mkdir("/midi");
        if (!SD.exists("/midi/songs")) SD.mkdir("/midi/songs");
        if (!SD.exists("/midi/drums")) SD.mkdir("/midi/drums");
        if (!SD.exists("/midi/loops")) SD.mkdir("/midi/loops");
        if (!SD.exists("/midi/tests")) SD.mkdir("/midi/tests");
    }
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
    AudioMemory_F32(60 + 6 + 4 * TDSP_AUDIOLOOP_N + TDSP_FX * 8 + TDSP_USB_AUDIO * 8);
#else
    AudioMemory_F32(60 + TDSP_FX * 8 + TDSP_USB_AUDIO * 8);   // + reverb insert + USB/ext sub-mix blocks
#endif
    setMix(1.0f, 0.0f, 1.0f);
#if TDSP_METRONOME
    postL.gain(0, 1.0f); postR.gain(0, 1.0f);   // main mix -> DAC at unity through the post mixer
    metroApplyMute();                            // metronome slot 1 starts at its (muted) level
#endif
#if TDSP_FX
    fxInit();   // push default reverb params + start bypassed (BYPASS_MODE_PASS = clean dry)
#endif
    outL.gain(3, TDSP_DEFAULT_SYNTH_MAKEUP);  outR.gain(3, TDSP_DEFAULT_SYNTH_MAKEUP);  // synth (slot 3) mix make-up in the
                                                 // F32 domain, where there's real headroom.
    testTone.frequency(440.0f);  testTone.amplitude(0.0f);
    spdifTone.frequency(1000.0f); spdifTone.amplitude(0.25f);
    if (g_codecOk) applyVol();   // codec at its fixed board level (g_dvol)
    applyAppMaster();            // digital app master start (TDSP_DEFAULT_APP_VOL_PCT)

#if TDSP_HAS_DIN_MIDI
    // Physical MIDI IN on Serial1 (pin 0), omni, soft-thru off -> the hub (SrcDin).
    MIDI.begin(MIDI_CHANNEL_OMNI);
    MIDI.turnThruOff();
    MIDI.setHandleNoteOn(dinNoteOn);
    MIDI.setHandleNoteOff(dinNoteOff);
    MIDI.setHandlePitchBend(dinPitch);
    MIDI.setHandleControlChange(dinCC);
#endif
#if TDSP_HAS_USB_MIDI_HOST
    // USB host: a controller (LinnStrument) plugged into the Teensy 4.1 host port -> the hub
    // (SrcUsbHost). Which synth voice(s) it feeds is the Track subscription, not a callback choice.
    g_usbHost.begin();
    g_usbMidi.setHandleNoteOn(usbHostNoteOn);
    g_usbMidi.setHandleNoteOff(usbHostNoteOff);
    g_usbMidi.setHandleControlChange(usbHostCC);
    g_usbMidi.setHandlePitchChange(usbHostPitch);
    g_usbMidi.setHandleAfterTouchChannel(usbHostPressure);   // channel pressure = MPE Z-axis
#endif

    // Uniform per-track MIDI graph (trackWireSetup): each track's router + song player feed [arp] ->
    // sink, with the looper tapping the baked post-arp stream. The arp is bypassed by default (verbatim
    // forward) so behaviour is identical until @ARP*ON=1; it steps on its router's onClock (fed by the
    // Conductor's 24-PPQN tick hook) so rate divisions lock to the master BPM. Voice 1 first; voice 2
    // (keyboard router -> engines 4..7) is independent so the song/arp/drums keep running on voice 1.
    trackWireSetup(g_tracks[0]);
#if TDSP_AUDIOLOOP
    audioLoopSetup();   // allocate the audio-loop buffers (PSRAM else OCRAM) + final-mix gains
#endif
    // Dedicated drum-groove player (still hand-wired — P2 folds it into a Track): channel 10 only,
    // loops, ignores the file's program changes (we own the kit via @DRUMKIT). Feeds the GM sink
    // DIRECTLY, bypassing the arp, so a groove backs the melodic voice but is never arpeggiated.
    // Route ch10 through the note-map shim (rescues GMD Roland hi-hats 22/26 on GM engines;
    // default GmReduce). g_drumTrack.sink keeps the REAL sink -- the P2 track path is still inert.
    g_drumNoteMapper.setDownstream(g_synthSink);
    g_drumPlayer.setSink(&g_drumNoteMapper); g_drumTrack.sink = g_synthSink;   // default; a dedicated drum engine overrides below
    g_drumPlayer.setChannelMask((uint16_t)(1u << 9));   // MIDI channel 10 (index 9)
    g_drumPlayer.setProgramChangeEnabled(false);
    g_drumPlayer.setLooping(true);
    g_drumPlayer.setOneShotTail(true);   // drums are one-shot: don't fire the loop's seam note-offs
                                         // (end-of-track cleanup) — let the last fill/crash ring through
                                         // instead of getting chopped every bar. Mid-loop offs (hat chokes)
                                         // still fire, so articulation is preserved.
#if TDSP_VOICE2
    // Voice 2's per-channel bend range is owned by applyMidiMode() (2 normal / 48 MPE); the startup
    // applyMidiMode() call below sets it, so an MPE controller's per-note slides aren't clamped.
    trackWireSetup(g_tracks[1]);
#endif
#if TDSP_SYNTH_VOICES >= 4
    trackWireSetup(g_tracks[2]);   // voices 3/4 of the 4-way pool (own router/arp/sink)
    trackWireSetup(g_tracks[3]);
#elif TDSP_HETERO
    trackWireSetup(g_tracks[kDexedVoices]);   // the melodic OPLL voice (own router/arp -> OPLL sink)
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
    for (Track &t : g_tracks) if (t.follow) g_conductor.addFollower(t.follow);   // each track's player retimes to master BPM
    g_conductor.addFollower(&g_drumFollow);
    g_conductor.addFollower(&g_launchSched);   // flags bar edges so loop() can fire quantized launches
    g_router.addSink(&g_clockSink);
    g_conductor.setTickHook(+[](void*){
        for (tdsp::MidiRouter &r : g_routerV) r.handleClock();   // step every voice's arp on the master grid
    }, nullptr);
    synthBegin();
#if TDSP_HETERO
    // Heterogeneous inventory (Thread D): bring up the melodic OPLL voice(s) (summed into mix slot
    // TDSP_HO_SLOT), then force the Dexed pool's 4/4 split ON so both Dexed voices (0/1) are always
    // live alongside them — the hetero build has no runtime @VOICE2 toggle; every voice is a
    // permanent Track. The split is enabled whenever the build has a second Dexed voice (TDSP_VOICE2),
    // independent of the TOTAL voice count (which now also counts the melodic OPLL tracks).
#if TDSP_OPLL_ENGINES >= 1
    heteroOpllBegin();   // skipped when the hetero inventory has no hetero-OPLL voices (e.g. 0-Dexed OPLL-primary + Plaits)
#endif
#if TDSP_HETERO_PLAITS
    heteroPlaitsBegin();   // bring up the melodic Plaits track + open its mix slot
#endif
#if TDSP_VOICE2
    g_voice2On = true;
    synthSetVoice2Enabled(true);   // split engines 4..7 off as Dexed voice 1 (g_synthSinkB)
#endif
#endif
    // Capture the engine's drum capability NOW (synthBegin set the song mask to
    // kMaskAll on drum-capable engines). drumEngineOk() reads this, so we're free to
    // toggle g_player's live channel mask later to mute a song's drums under a groove.
    g_engineHasDrums = (g_player.channelMask() == tdsp::MidiFilePlayer::kMaskAll);
#ifdef TDSP_DRUM_TSF
    // Melodic engine + dedicated GM-drum TSF: bring up the drum engine, route the groove
    // player's channel 10 to it (instead of the melodic sink), and mark drums available
    // regardless of the melodic engine's own no-drum song mask.
    if (drumTsfBegin()) {
        g_drumNoteMapper.setDownstream(&g_drumTsfSink);
        g_drumPlayer.setSink(&g_drumNoteMapper); g_drumTrack.sink = &g_drumTsfSink;
        g_engineHasDrums = true;
        // If the acoustic multi-kit font loaded, swap the Drums menu to ITS kit list
        // (its sibling <font>.tsv) so @DRUMKIT / the app select real kits by program.
        if (g_drumFontIsKits) loadDrumKitsTsv(g_drumFontPath);
#if defined(TDSP_DRUM_TSF)
        g_drumFontCount = drumFontCount();   // cache the swappable-font count for caps.drumfontsel
#endif
    }
#endif
#ifdef TDSP_DRUM_VOICE
    // Same idea with the OPLL rhythm voice (no PSRAM): route ch10 to it + mark drums OK.
    if (drumVoiceBegin()) {
        g_drumNoteMapper.setDownstream(&g_drumVoiceSink);
        g_drumPlayer.setSink(&g_drumNoteMapper); g_drumTrack.sink = &g_drumVoiceSink;
        g_engineHasDrums = true;
    }
#endif
#ifdef TDSP_DRUM_PLAITS
    // Same idea with the authentic Plaits drum pool (no PSRAM): route ch10 to it + mark drums OK.
    if (drumPlaitsBegin()) {
        g_drumNoteMapper.setDownstream(&g_drumPlaitsSink);
        g_drumPlayer.setSink(&g_drumNoteMapper); g_drumTrack.sink = &g_drumPlaitsSink;
        g_engineHasDrums = true;
    }
#endif
#ifdef TDSP_DRUM_SD
    // Same idea with the SD-streaming one-shot kit (no PSRAM): route ch10 to it + mark drums OK.
    // Idles gracefully (returns false -> drumEngineOk() stays false) when /drums has no kit folders.
    if (drumSamplerBegin()) {
        g_drumNoteMapper.setDownstream(&g_drumSamplerSink);
        g_drumPlayer.setSink(&g_drumNoteMapper); g_drumTrack.sink = &g_drumSamplerSink;
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
        // Also catch a DRUM-engine swap that kept the SAME melodic engine (e.g. OPLL/Plaits drums ->
        // SD Sampler): readStoredEngine sees no change, so the app would keep the stale drumEngine
        // label. Compare the stored parallel-drum label to this build's (engineCaps().drumEngine).
        char storedDrum[32];
        tdsp::catdb::readStoredDrumEngine(storedDrum, sizeof storedDrum);   // "" if absent
        const char *curDrum = engineCaps().drumEngine ? engineCaps().drumEngine : "";
        bool drumChanged = strcmp(storedDrum, curDrum) != 0;
        if (engineChanged || versionChanged || drumChanged) {
            Serial.printf("[catdb] catalog stale (engine %s->%s, drum %s->%s, v %d->%d) -> auto-reindex\n",
                          have ? stored : "(none)", synthName(), storedDrum[0] ? storedDrum : "(none)",
                          curDrum[0] ? curDrum : "(none)", storedVer, tdsp::catdb::kCatalogVersion);
            // forceAll: a builder-version bump can change the WRITER output without changing the
            // per-source signature (grooves went recursive), so rebuild every source, not just
            // the ones whose file count/bytes moved.
            tdsp::catdb::buildCatalog(engineCaps(), catdbWriteBundled, millis(), /*forceAll=*/true);
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
#if TDSP_BOARDTEST
#include "BoardTest.inc.h"   // @BOARDTEST self-test (own gate; independent of TDSP_DIAGNOSTICS)
#endif

// The SD-free, non-blocking realtime subset of loop(), factored out so a long blocking
// SD stream (streamFile/streamDir/streamClip) can keep the master transport running mid-
// transfer. This is EXACTLY the time-critical work loop() does every iteration — advance
// the clock, strike the click, dispatch the groove/song players and arps — MINUS anything
// that could touch the SD and re-enter the transfer in flight: the launch-quantize fire
// (trackFire parses a groove/song from the card) and trackLoopTick (re-arms a finished
// song, another SD parse). Those stay loop()-only; a launch armed during a stream simply
// fires on the next real loop() the moment the transfer completes. Prototyped up by the
// streamers (they call it); defined here where every symbol it needs is already in scope.
static void pumpTransport() {
    g_conductor.update(micros());
#ifdef TDSP_METRONOME
    metroPoll();
#endif
    beatEmitPoll();
#if TDSP_HAS_DIN_MIDI
    while (MIDI.read()) { /* live DIN MIDI still played through the load */ }
#endif
#if TDSP_HAS_USB_MIDI_HOST
    g_usbHost.Task();
    while (g_usbMidi.read()) { /* live USB-host controller stays responsive */ }
#endif
    for (Track &t : g_tracks) t.player->tick();
    g_drumPlayer.tick();
    for (tdsp::ArpFilter &a : g_arpFilterV) a.tick(micros());
#if TDSP_RECORDER
    g_loop1.poll(); g_loop1.tick();
#if TDSP_VOICE2
    g_loop2.poll(); g_loop2.tick();
#endif
    recPollClick();
#endif
#if TDSP_AUDIOLOOP
    for (uint8_t i = 0; i < g_aloopN; i++) g_aloop[i].poll();
#endif
}

void loop() {
    // Flash-mode passthrough owns the loop (also handles @BOOTAPP@); in run mode this
    // ticks the slow LED heartbeat and returns false.
    if (kit.service(Serial)) return;

#ifdef TDSP_DRUM_PLAITS
    g_drumPlaitsSink.service();   // release one-shot Plaits drum gates whose ~6 ms hold has elapsed
#endif
#ifdef TDSP_DRUM_SD
    g_drumSamplerSink.pollVoices();   // reclaim SD drum voices whose one-shot stream has ended
#endif
    // Anti-zipper param glides (cheap, self-gated ~1 kHz): ramp the master DAC gain + FX mix toward
    // their targets so the app's throttled slider drags fade smoothly instead of stepping.
    masterVolTick();
#if TDSP_FX
    fxTick();
#endif

    // Jitter probe: worst loop()-to-loop() gap this window. A drum hit can be delayed by
    // at most one loop() period, so this bounds the dispatch jitter (see @DRUMJIT / @JIT).
    if (g_jitProbe) {
        uint32_t nowUs = micros();
        if (g_lastLoopUs) { uint32_t gap = nowUs - g_lastLoopUs; if (gap > g_maxLoopGapUs) g_maxLoopGapUs = gap; }
        g_lastLoopUs = nowUs;
    }

#if TDSP_HAS_SDCARD && defined(USB_MTPDISK_SERIAL)
    MTP.loop();   // service USB file transfers to/from the SD (host drag-and-drop)
#endif

    // Advance the master clock BEFORE draining MIDI. In Internal mode it emits
    // catch-up 24-PPQN ticks (fanned to onClock() consumers via the tick hook)
    // and fires bar-edge callbacks to followers; in External mode this is the
    // stall watchdog. Cheap when nothing's due.
    g_conductor.update(micros());

#ifdef TDSP_METRONOME
    metroPoll();   // strike/decay the clock-driven click (silent unless unmuted)
#endif

    beatEmitPoll();   // @BEAT=<beatInBar>/<beatsPerBar> once per beat, for the app's beat lights

    // Live MIDI: drain DIN + USB-host controllers, then advance the (non-blocking) song.
#if TDSP_HAS_DIN_MIDI
    while (MIDI.read()) { /* handlers fire per message */ }
#endif
#if TDSP_HAS_USB_MIDI_HOST
    g_usbHost.Task();
    while (g_usbMidi.read()) { /* USB-host MIDI handlers fire per message */ }
#endif
    for (Track &t : g_tracks) t.player->tick();   // advance every track's song player
    g_drumPlayer.tick();   // loops internally (setLooping), so no external re-arm needed

    // Launch quantize: fire armed launches (song / groove / player 2) on the bar edge — but ONLY
    // AFTER the already-running players ticked this beat's notes above, so a launch (and its
    // blocking SD load) can NEVER preempt or drop another player's downbeat. That was the bug:
    // starting the drums / player 2 on a downbeat stole player 1 & 2's first note. Then re-tick the
    // players so a JUST-launched one still lands its own downbeat in this same iteration.
    if (g_launchSched.barHit) {
        g_launchSched.barHit = false;
        // Every voice fires from its PRELOADED stash (trackFire = play()+sync, no SD parse) so a launch
        // can never stall loop() on the beat. anchorNow=true starts it from the top at this downbeat.
        if (*g_tracks[0].launchPending) { *g_tracks[0].launchPending = false; trackFire(g_tracks[0], /*anchorNow=*/true); }
        if (*g_drumTrack.launchPending) { *g_drumTrack.launchPending = false; trackFire(g_drumTrack, /*anchorNow=*/true); }
#if TDSP_VOICE2
        if (*g_tracks[1].launchPending) { *g_tracks[1].launchPending = false; trackFire(g_tracks[1], /*anchorNow=*/true); }
#endif
#if TDSP_SYNTH_VOICES >= 4
        if (*g_tracks[2].launchPending) { *g_tracks[2].launchPending = false; trackFire(g_tracks[2], /*anchorNow=*/true); }
        if (*g_tracks[3].launchPending) { *g_tracks[3].launchPending = false; trackFire(g_tracks[3], /*anchorNow=*/true); }
#endif
        g_syncAnchorNow = false;   // defensive: a not-found launch never leaves it armed
        for (Track &t : g_tracks) t.player->tick();   // a just-launched player hits its downbeat now (already-running ones no-op)
        g_drumPlayer.tick();
    }
    for (tdsp::ArpFilter &a : g_arpFilterV) a.tick(micros());   // drain each arp's gate-off queue (steps fire on onClock)
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
    trackLoopTick(g_tracks[0]);   // auto-restart the song if loop mode is on and it just ended
#if TDSP_VOICE2
    trackLoopTick(g_tracks[1]);   // same for player 2
#endif
#if TDSP_SYNTH_VOICES >= 4
    trackLoopTick(g_tracks[2]); trackLoopTick(g_tracks[3]);   // voices 3/4 loop re-arm
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

    // @DRUMJIT: once/second, emit drum dispatch-lateness stats + worst loop() gap. Lateness
    // is reported in ms (converted from beats via the live master BPM) so the host tool can
    // read on-beat accuracy directly. Cumulative since @DRUMJIT=1 for count/mean/max; the
    // loop gap is a per-second worst-case (reset each window to surface sporadic stalls).
    if (g_jitProbe && g_jitClock >= 1000) {
        g_jitClock = 0;
        const float bpm = g_masterBpm > 1.0f ? g_masterBpm : 120.0f;
        const double msPerBeat = 60000.0 / (double)bpm;
        Serial.printf("@JIT n=%lu maxLate=%.2f meanLate=%.2f std=%.2f maxLoopGap=%.0f bpm=%.1f\n",
                      (unsigned long)g_drumPlayer.jitterCount(),
                      g_drumPlayer.jitterMaxBeats()  * msPerBeat,
                      g_drumPlayer.jitterMeanBeats() * msPerBeat,
                      g_drumPlayer.jitterStdBeats()  * msPerBeat,
                      (double)g_maxLoopGapUs / 1000.0, (double)bpm);
        g_maxLoopGapUs = 0;   // per-second worst-case window
    }

    // Push each track's playback position to the app (drives the MIDI Player progress bars): @SONGP
    // for voice 1, @SONG2P for voice 2. Block-static throttle/edge state per track.
    { static elapsedMillis clk; static bool prev = false; emitTrackPos(g_tracks[0], "@SONGP", clk, prev); }
#if TDSP_VOICE2
    { static elapsedMillis clk; static bool prev = false; emitTrackPos(g_tracks[1], "@SONG2P", clk, prev); }
#endif
#if TDSP_SYNTH_VOICES >= 4
    { static elapsedMillis clk; static bool prev = false; emitTrackPos(g_tracks[2], "@TRK2.P", clk, prev); }
    { static elapsedMillis clk; static bool prev = false; emitTrackPos(g_tracks[3], "@TRK3.P", clk, prev); }
#elif TDSP_HETERO
    { static elapsedMillis clk; static bool prev = false; emitTrackPos(g_tracks[kDexedVoices], "@TRK2.P", clk, prev); }
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
            else if (c == 'W') { if (g_player.isPlaying()) songStop(g_tracks[0]);             // play/stop
                                 else if (g_curSongArg[0]) trackStartArg(g_tracks[0], g_curSongArg);
                                 else trackStartIndex(g_tracks[0], 0); }
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
            else if (c == 'O') { g_loop = !g_loop; g_player.setLooping(g_loop); Serial.printf("[song] loop %s\n", g_loop ? "ON" : "off"); }  // lOop toggle
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
                      "btPeak=%.3f spdifPeak=%.3f outPeak=%.3f  cpuMax=%.1f%% memMax=%u f32Max=%u\n",
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
                      AudioProcessorUsageMax(), AudioMemoryUsageMax(),
                      (unsigned)AudioStream_F32::f32_memory_used_max);   // DIAG: F32 block pool peak vs AudioMemory_F32(N)
        AudioProcessorUsageMaxReset();   // make cpuMax a per-second rolling peak
        AudioMemoryUsageMaxReset();
        AudioStream_F32::f32_memory_used_max = 0;
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

