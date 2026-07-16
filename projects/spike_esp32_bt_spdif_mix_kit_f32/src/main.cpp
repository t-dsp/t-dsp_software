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
#include <MIDI.h>
#include <USBHost_t36.h>   // USB host: receive MIDI from a controller (e.g. LinnStrument) via USB
#include <MidiRouter.h>    // MPE-aware fan-out: bend->semitones, CC74->timbre, pressure->onPressure
#include <SD.h>
#include <MTP_Teensy.h>   // expose the SD card to the host over USB (Serial+MTP)
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

// Developer bench diagnostics (self-tests, MPE/axis proofs, capture probes, the
// ReplayGain sweep) are opt-in and live in Diagnostics.inc.h. Default ON so every
// existing build behaves EXACTLY as before; a lean product build sets
// -D TDSP_DIAGNOSTICS=0 to compile them out and reclaim flash.
#ifndef TDSP_DIAGNOSTICS
#define TDSP_DIAGNOSTICS 1
#endif

extern "C" uint8_t external_psram_size;   // MB of soldered PSRAM (Teensy core startup)

constexpr int     TAC5212_EN_PIN      = 35;     // shared SHDNZ, active-low
constexpr uint8_t TAC5212_I2C_ADDRESS = 0x51;

// Physical MIDI IN: schematic MIDI_RX = Teensy pin 0 (Serial1 RX) via the H11L1
// opto. Drives the Dexed source below. (See projects/spike_midi_dexed.)
MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI);

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
AsyncAudioInput<AsyncAudioInputI2S2_16bitslave> btIn(false, false, 100, 20, 80);
AudioConvert_I16toF32  btToF32L, btToF32R;

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
USBHost                g_usbHost;
MIDIDevice             g_usbMidi(g_usbHost);
tdsp::MidiRouter       g_router;

// Master clock: THE tempo authority. The song + drum players follow it via
// PlayerFollower adapters (applyTempos() below is the single tempo write path).
// Its internal 24-PPQN tick fans through the router so an arp / onClock() sink
// is a drop-in. g_clockSink lets external MIDI clock (0xF8) slave the kit once
// real-time handlers + Clock::External are enabled. See lib/TDspTempo/README.md.
tdsp::Conductor        g_conductor;
tdsp::PlayerFollower   g_songFollow{g_player};      // g_player / g_drumPlayer are
tdsp::PlayerFollower   g_drumFollow{g_drumPlayer};  // declared above (lines ~92-93)
tdsp::ClockSink        g_clockSink{&g_conductor.clock()};
tdsp::ArpFilter        g_arpFilter;                 // live MIDI -> arp -> synth (bypass by default)
static bool            g_mpeMode = false;    // false = normal MIDI (bend +-2, ch10 drums), true = MPE

AudioMixer4_F32        outL, outR;           // F32 mix: 0=BT, 1=local tone, 2=S/PDIF-in, 3=synth
AudioAnalyzePeak_F32   peakBt, peakSpdif, peakOut;

// int16 leg: optical-out tone -> SPDIF TX (self-test loopback source)
AudioConnection     c_txL    (spdifTone,  0, spdifOut, 0);
AudioConnection     c_txR    (spdifTone,  0, spdifOut, 1);
// int16 -> F32 bridges (BT L/R) — the int16 side of the convert blocks
AudioConnection     c_btcL   (btIn,       0, btToF32L, 0);
AudioConnection     c_btcR   (btIn,       1, btToF32R, 0);
// F32 mix bus and 24-bit TDM output (synth engine feeds slot 3 from its backend)
AudioConnection_F32 c_btL    (btToF32L,   0, outL, 0);
AudioConnection_F32 c_btR    (btToF32R,   0, outR, 0);
AudioConnection_F32 c_toneL  (testTone,   0, outL, 1);
AudioConnection_F32 c_toneR  (testTone,   0, outR, 1);
#if TDSP_SPDIF_IN
AudioConnection_F32 c_spL    (spdifIn,    0, outL, 2);
AudioConnection_F32 c_spR    (spdifIn,    1, outR, 2);
#endif
AudioConnection_F32 c_outL   (outL,       0, tdmOut, 0);
AudioConnection_F32 c_outR   (outR,       0, tdmOut, 1);
AudioConnection_F32 c_pkBt   (btToF32L,   0, peakBt,    0);
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

static float g_dvol = -20.0f;
static void applyVol() {
    g_codec.out(1).setDvol(g_dvol);
    g_codec.out(2).setDvol(g_dvol);
}

// Master headphone volume from the phone app: it arrives as an "@VOL=<pct>" line
// on the ESP32 UART (App -> BLE -> ESP32 -> here). 0..100% -> DAC dB: 0 = mute,
// 1..100 maps linearly across -60..0 dB. Controls the TAC5212 OUT1/OUT2 (HP jack).
static void setMasterVolumePct(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    g_dvol = (pct == 0) ? -128.0f : (-60.0f + 0.60f * (float)pct);
    if (g_codecOk) applyVol();
    Serial.printf("[vol] app set %d%% -> %.1f dB\n", pct, g_dvol);
}

// TAC5212 DAC highpass filter from the phone app: arrives as "@HPF=<mode>" on the
// ESP32 UART. mode 0 = off (all-pass), 1/2/3 = 1/12/96 Hz cutoff. Chip-global,
// applied to the DAC output (the ADC path is disabled in this firmware).
static void setDacHpfMode(int mode) {
    tac5212::DacHpf hpf;
    switch (mode) {
        case 1:  hpf = tac5212::DacHpf::Cut1Hz;  break;
        case 2:  hpf = tac5212::DacHpf::Cut12Hz; break;
        case 3:  hpf = tac5212::DacHpf::Cut96Hz; break;
        default: hpf = tac5212::DacHpf::Programmable; break;  // 0 / unknown = off
    }
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
    g_codec.out(1).setMode(tac5212::OutMode::HpDriver);
    g_codec.out(2).setMode(tac5212::OutMode::HpDriver);
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
}

// mixer helper: 0=BT, 1=local test tone, 2=S/PDIF-in (slot 3 = Dexed, set once,
// stays on independently of source-mode switches)
static void setMix(float bt, float tone, float spdif) {
    outL.gain(0, bt);    outR.gain(0, bt);
    outL.gain(1, tone);  outR.gain(1, tone);
#if !defined(TDSP_DRUM_TSF) && !defined(TDSP_DRUM_VOICE)
    outL.gain(2, spdif); outR.gain(2, spdif);
#else
    (void)spdif;   // slot 2 is the drum voice's bus in this build (drum*Begin owns its gain)
#endif
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

// Unified runtime song list: built-ins first, then /songs/*.mid off the SD card.
// SD songs are parsed on play into g_buf. Adding a song = drop a .mid on the
// card; it appears in the catalog (and the app) with no firmware rebuild.
// A song source is one of: baked SongEv[] (ev), a baked rich MidiFileEvent[] test
// sequence (mev, plays directly + flips MPE mode via `mpe`), or an SD .mid file (sd/path).
struct SongRef { char name[48]; const SongEv *ev; uint32_t count; char path[96]; bool sd;
                 const tdsp::MidiFileEvent *mev; uint32_t mcount; bool mpe; };
static SongRef g_songs[48];
static int     g_numSongs = 0;
// g_sdReady is declared earlier (before the synth backend include).

static const int MAX_EVENTS = 24000;                 // longest playable song (baked or SD)
DMAMEM static tdsp::MidiFileEvent g_buf[MAX_EVENTS];  // ~144KB in OCRAM (off the DTCM budget)

static bool endsWithMid(const char *s) {
    size_t n = strlen(s);
    return n > 4 && strcasecmp(s + n - 4, ".mid") == 0;
}
static bool songNameExists(const char *name) {   // case-insensitive, for de-dup
    for (int i = 0; i < g_numSongs; ++i)
        if (strcasecmp(g_songs[i].name, name) == 0) return true;
    return false;
}
// Scan one directory for *.mid and append each (deduped by display name). `dir`
// is "/songs" or "/" (the card root, so files dropped at the top level also work).
FLASHMEM static void scanSongDir(const char *dir) {
    File d = SD.open(dir);
    if (!d || !d.isDirectory()) { if (d) d.close(); return; }
    const int cap = (int)(sizeof(g_songs) / sizeof(g_songs[0]));
    for (File f = d.openNextFile(); f && g_numSongs < cap; f = d.openNextFile()) {
        const char *nm = f.name();
        if (!f.isDirectory() && nm && endsWithMid(nm)) {
            char disp[48];                                  // display name = filename minus ".mid"
            size_t copy = strlen(nm) - 4;                   // (endsWithMid guarantees len > 4)
            if (copy > sizeof(disp) - 1) copy = sizeof(disp) - 1;
            memcpy(disp, nm, copy); disp[copy] = 0;
            if (!songNameExists(disp)) {                    // keep the list unique (built-ins win)
                SongRef &r = g_songs[g_numSongs++];
                if (strcmp(dir, "/") == 0) snprintf(r.path, sizeof(r.path), "/%s", nm);
                else                       snprintf(r.path, sizeof(r.path), "%s/%s", dir, nm);
                strncpy(r.name, disp, sizeof(r.name) - 1); r.name[sizeof(r.name) - 1] = 0;
                r.ev = nullptr; r.count = 0; r.sd = true; r.mev = nullptr; r.mcount = 0; r.mpe = false;
            }
        }
        f.close();
    }
    d.close();
}
FLASHMEM static void buildSongList() {
    const int cap = (int)(sizeof(g_songs)/sizeof(g_songs[0]));
    g_numSongs = 0;
    // Built-in MIDI/MPE test sequences FIRST, so they head the picker as "01 .. 08".
    for (int i = 0; i < testsong::kNumTestSongs && g_numSongs < cap; ++i) {
        SongRef &r = g_songs[g_numSongs++];
        strncpy(r.name, testsong::kTestSongs[i].name, sizeof(r.name) - 1); r.name[sizeof(r.name) - 1] = 0;
        r.ev = nullptr; r.count = 0; r.sd = false; r.path[0] = 0;
        r.mev = testsong::kTestSongs[i].ev; r.mcount = testsong::kTestSongs[i].count; r.mpe = testsong::kTestSongs[i].mpe;
    }
    // SD songs BEFORE the baked built-ins: a real .mid carries its real tempo, so an
    // SD copy of a built-in (same display name) must WIN — the baked SongEv versions
    // were transcoded to raw ms and have NO tempo (they can't lock to a drum groove).
    if (g_sdReady) {
        if (!SD.exists("/songs")) SD.mkdir("/songs");   // a home to drop songs into
        scanSongDir("/songs");
        scanSongDir("/");                               // also accept .mid at the card root
    }
    // Baked built-ins LAST — a no-SD fallback only. Skipped when an SD song already
    // provides that name, so the tempo-bearing SD copy is the one that plays.
    int nBuiltin = 0;
    for (int i = 0; i < kNumBuiltin && g_numSongs < cap; ++i) {
        if (songNameExists(kBuiltinSongs[i].name)) continue;   // SD copy wins
        SongRef &r = g_songs[g_numSongs++]; nBuiltin++;
        strncpy(r.name, kBuiltinSongs[i].name, sizeof(r.name) - 1); r.name[sizeof(r.name) - 1] = 0;
        r.ev = kBuiltinSongs[i].ev; r.count = kBuiltinSongs[i].count; r.sd = false; r.path[0] = 0;
        r.mev = nullptr; r.mcount = 0; r.mpe = false;
    }
    Serial.printf("[sd] songs: %d total (%d test + %d SD + %d baked fallback)\n",
                  g_numSongs, testsong::kNumTestSongs,
                  g_numSongs - testsong::kNumTestSongs - nBuiltin, nBuiltin);
}

static int  g_songSel = 0;          // selected / currently-playing song index
static bool g_loop = false;         // when set, a song restarts itself when it ends
static bool g_songWasPlaying = false;  // edge-detect natural song end (for loop) in loop()

// Load the selected song into g_buf and hand it to the player. Baked built-ins
// expand from their legacy SongEv[] (channel 0); SD songs parse straight to
// MidiFileEvent[] (full channel/program/velocity). The player is non-blocking
// (g_player.tick() in loop) and drives the synth via g_synthSink.
static void applyMidiMode(bool mpe);   // defined below; test songs flip mode on start

// Drum controls. g_drumSel / g_drumKit are used by the drum section further below.
static int  g_drumSel      = 0;     // selected / currently-playing groove index
static int  g_drumKit      = 0;     // index into kDrumKits ("instrument")
static int  g_drumVolPct   = 100;   // drum level 0..150 (% of file velocity)
static bool g_drumSynchro  = false; // SYNCHRO START (PSS-140 style): groove starts on your first note
static bool g_engineHasDrums = false;// engine renders ch10 (captured once at setup; not the live mask)

// --- Master tempo (BPM) — one knob drives the song AND the drum groove -------
// The song and the groove each have a NATIVE tempo; the master BPM retimes both
// to a single tempo so they stay locked, and moving it speeds/slows both together:
//   song scale = masterBpm / songNativeBpm     (songNativeBpm: SD = real, built-in = estimate)
//   drum scale = masterBpm / grooveNativeBpm
// Downbeat align: whichever starts SECOND begins on the other's bar/loop downbeat
// (both bars are 4/4 = 4*60000/masterBpm long once retimed, so they line up).
// NOTE accurate lock needs the song's REAL tempo -> use an SD .mid; the baked
// built-ins only carry an estimate.
static float         g_masterBpm     = 120.0f;  // the one tempo knob (40..240)
static float         g_songBpm       = 120.0f;  // playing/last song NATIVE tempo
static float         g_drumFileBpm   = 120.0f;  // selected groove's NATIVE tempo
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
    g_conductor.setBpm(g_masterBpm);
}

FLASHMEM static void songStart(int idx) {
    if (g_numSongs == 0) return;
    if (idx < 0) idx = 0;
    if (idx >= g_numSongs) idx = g_numSongs - 1;
    g_songSel = idx;
    SongRef &r = g_songs[idx];
    // Clean slate for EVERY song, in EVERY mode: silence sounding notes and clear latched
    // per-engine expression (bend / mod / aftertouch). Without this, a bend left mid-glide
    // by the previous song carries into this one — and the mode-switch path below only runs
    // when the mode actually changes, so a MIDI->MIDI start would otherwise never reset.
    // BUT don't panic channel 10 while a drum groove is looping — an all-channels reset
    // would cut the drums for a beat when you press Play on a song. Spare ch10 then.
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
    // Baked rich-event test sequence: set the device mode (MPE tests need per-note
    // expression) then hand the events straight to the player — no expansion needed.
    if (r.mev) {
        applyMidiMode(r.mpe);
        g_songBpm = 120.0f; applyTempos();                             // baked test seq: no tempo meta
        Serial.printf("[song] %s -> %s (%s, %lu events, start)\n", r.name, synthName(),
                      r.mpe ? "MPE" : "MIDI", (unsigned long)r.mcount);
        g_player.play(r.mev, r.mcount);
        g_songBarClock = 0;
        return;
    }
    // A non-test song is normal MIDI; if a prior MPE test left the device in MPE mode,
    // return to normal so multi-timbral songs play with their own programs.
    if (g_mpeMode) applyMidiMode(false);
    uint32_t n = 0;
    if (r.sd) {
        g_songBpm = 120.0f;
        int got = tdsp::smf::loadSmfFile(r.path, g_buf, MAX_EVENTS, &g_songBpm);   // parse + tempo
        if (got <= 0) { Serial.printf("[song] SD load FAILED: %s\n", r.path); return; }
        n = (uint32_t)got;
        Serial.printf("[song] %s (SD, %lu events, %.1f bpm) -> %s (start)\n", r.name, (unsigned long)n, (double)g_songBpm, synthName());
    } else {
        g_songBpm = 120.0f;                                            // baked SongEv: tempo estimate
        for (int i = 0; i < kNumBuiltin; ++i)
            if (kBuiltinSongs[i].ev == r.ev) { g_songBpm = kBuiltinSongs[i].bpm; break; }
        n = tdsp::expandLegacyNotes(r.ev, r.count, g_buf, MAX_EVENTS);  // baked SongEv -> events
        Serial.printf("[song] %s (%.1f bpm est) -> %s (start)\n", r.name, (double)g_songBpm, synthName());
    }
    if (n == 0) return;
    applyTempos();   // retime the song (and groove) to the master BPM
    g_player.play(g_buf, n);                    // immediate: starts right when you press Play
    g_songBarClock = 0;
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
}

// Called every loop(): if a looping song just ended on its own, restart it. Manual
// stops clear g_songWasPlaying above, so they don't re-trigger.
static void songLoopTick() {
    bool now = g_player.isPlaying();
    if (g_songWasPlaying && !now && g_loop) {
        songStart(g_songSel);       // re-arm the same song (also re-applies its MIDI/MPE mode)
        now = g_player.isPlaying();
    }
    g_songWasPlaying = now;
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
static void catdbWriteBundled() {
    SD.remove("/tdsp/instruments.ndjson");
    File o = SD.open("/tdsp/instruments.ndjson", FILE_WRITE);
    if (o) {
        if (!synthIsGM())   // GM engines stream no names (client renders GM 0..127 itself)
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
    // songs.ndjson — from the g_songs PLAY registry so row i == the @SONG=<i> index.
    SD.remove("/tdsp/songs.ndjson");
    File so = SD.open("/tdsp/songs.ndjson", FILE_WRITE);
    if (so) {
        for (int i = 0; i < g_numSongs; ++i) {
            so.print("{\"i\":"); so.print(i); so.print(",\"name\":");
            tdsp::catdb::jsonStr(so, g_songs[i].name); so.print("}\n");
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
static void drumStartPath(const char* path, const char* disp) {
    if (!drumEngineOk()) { Serial.printf("[drum] %s has no channel-10 drum map — use TSF/SF2/OPL3/OPLL\n", synthName()); return; }
    g_drumFileBpm = 120.0f;
    int got = tdsp::smf::loadSmfFile(path, g_drumBuf, MAX_DRUM_EVENTS, &g_drumFileBpm);
    if (got <= 0) { Serial.printf("[drum] load FAILED: %s\n", path); return; }
    drumApplyKit();
    g_drumPlayer.setVelocityScale(g_drumVolPct / 100.0f);
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
    g_drumPlayer.play(g_drumBuf, (uint32_t)got);               // immediate: beat 1 = now
    g_conductor.start();                                        // zero the master clock to this downbeat
    Serial.printf("[drum] %s (%d ev, %.1f bpm) kit=%s @ master %.0f bpm vol=%d%%\n",
                  disp, got, (double)g_drumFileBpm, kDrumKits[g_drumKit].name, (double)g_masterBpm, g_drumVolPct);
}
static void drumStart(int idx) {   // legacy numeric index (flat menu / serial C/D keys)
    if (g_numDrums == 0) { Serial.println("[drum] no grooves on SD (/drums) — run tools/fetch_drums.py"); return; }
    if (idx < 0) idx = 0;
    if (idx >= g_numDrums) idx = g_numDrums - 1;
    g_drumSel = idx;
    drumStartPath(g_drums[idx].path, g_drums[idx].name);
}
static void drumStartFile(const char* fname) {   // by filename — the browser's play path
    char path[128]; snprintf(path, sizeof(path), "/drums/%s", fname);
    char disp[64]; size_t c = strlen(fname);
    if (c > 4 && strcasecmp(fname + c - 4, ".mid") == 0) c -= 4;   // strip .mid for the log
    if (c > sizeof(disp) - 1) c = sizeof(disp) - 1;
    memcpy(disp, fname, c); disp[c] = 0;
    drumStartPath(path, disp);
}
static void drumStop() {
    g_drumArmed = false;                                       // cancel a synchro-armed groove
    muteSongDrums(false);                                      // give the song back its own drums
    if (!g_drumPlayer.isPlaying()) return;
    g_drumPlayer.stop();                                       // releases the groove's ch10 notes
    Serial.println("[drum] stopped");
}
static void setDrumKit(int i) {
    if (i < 0) i = 0;
    if (i >= kNumDrumKits) i = kNumDrumKits - 1;
    g_drumKit = i;
    if (drumEngineOk()) drumApplyKit();
    Serial.printf("[drum] kit -> %s (prog %u)\n", kDrumKits[i].name, kDrumKits[i].prog);
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
    g_drumPlayer.setVelocityScale(pct / 100.0f);
    Serial.printf("[drum] vol -> %d%%\n", pct);
}

// Stream the device catalog (song + instrument names, '|'-delimited) to the ESP32
// over the UART link. The ESP32 serves it on BLE so the app renders its pickers
// from whatever the device reports — adding a song/instrument is then a firmware
// change only, no app update. Sent when the ESP32 asks (@GETCAT, on BLE connect).
FLASHMEM static void sendCatalog(Print& out) {
    out.print("@SONGS=");
    for (int i = 0; i < g_numSongs; ++i) { if (i) out.print('|'); out.print(g_songs[i].name); }
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
    buildSongList();
    buildDrumList();
    sendCatalog(out);
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
// sources — their setHandle* signatures match.
static void midiNoteOn  (byte ch, byte note, byte vel) {
    // SYNCHRO START (PSS-140 style): the first live note kicks off an armed groove on
    // beat 1 — you pick the downbeat by when you play. (vel 0 = note-off, ignore.)
    if (g_drumArmed && vel > 0) {
        muteSongDrums(true);
        g_drumPlayer.play(g_drumBuf, g_drumArmedN);
        g_conductor.start();                        // downbeat is this note
        g_drumArmed = false;
        Serial.println("[drum] SYNCHRO start (first note)");
    }
    g_router.handleNoteOn(ch, note, vel);
}
static void midiNoteOff (byte ch, byte note, byte vel) { g_router.handleNoteOff(ch, note, vel); }
static void midiCC      (byte ch, byte cc,   byte val) { g_router.handleControlChange(ch, cc, val); }
static void midiPitch   (byte ch, int bend)            { g_router.handlePitchBend(ch, (int16_t)bend); }
static void midiPressure(byte ch, byte pressure)       { g_router.handleChannelPressure(ch, pressure); }

// Switch the device between normal MIDI and MPE (per-note expression). Sets the
// router's per-channel bend range (2 vs the LinnStrument's 48-semi default) and lets
// the backend reconfigure (TSF frees ch10 from drums so it's an MPE member channel).
static void applyMidiMode(bool mpe) {
    g_mpeMode = mpe;
    float range = mpe ? tdsp::MidiRouter::kDefaultPitchBendRange : 2.0f;   // 48 (MPE) vs 2
    for (uint8_t ch = 1; ch <= 16; ch++) g_router.setPitchBendRange(ch, range);
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
#ifdef TDSP_HAS_REPLAYGAIN
static void runGainSweep(int startIdx = 0);   // ReplayGain sweep (any backend); resumable from a voice index
#endif
#ifdef TDSP_SYNTH_DEXED_POOL
static void runMpeSweep(int startIdx);        // MPE demo on each instrument; resumable
static void runAxisProof(int axis);           // capture 1 note with an MPE axis at full
static void runMpeCheck(void);                // measure every instrument under MPE; flag silent/clip
#endif
#ifdef TDSP_SYNTH_SF2_TSF
static void runAxisProof(int axis);           // MPE axis proof ported to TSF (validates CC#74->cutoff)
#endif

FLASHMEM static bool handleControlLine(const char* line, Print& reply) {
    if      (strncmp(line, "@VOL=", 5) == 0)      setMasterVolumePct(atoi(line + 5));
#ifdef TDSP_HAS_REPLAYGAIN
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
    else if (strncmp(line, "@SONG=", 6) == 0) {
        if (strcmp(line + 6, "stop") == 0) songStop();
        else songStart(atoi(line + 6));   // @SONG=<song index>
    }
    else if (strcmp(line, "@GETCAT") == 0)        refreshCatalog(reply);   // re-scan SD + send catalog
    else if (strcmp(line, "@REINDEX") == 0)       { tdsp::catdb::buildCatalog(synthName(), drumEngineOk(), kDrumEngineName, catdbWriteBundled, millis()); reply.println("@REINDEXED"); }  // rebuild /tdsp/*.ndjson DB (upsert)
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
    else if (strncmp(line, "@DRUMF=", 7) == 0)    drumStartFile(line + 7);  // @DRUMF=<filename> (browser, via catalog.tsv)
    else if (strncmp(line, "@DRUMKIT=", 9) == 0)   setDrumKit(atoi(line + 9));    // GM kit ("instrument")
    else if (strncmp(line, "@DRUMVOL=", 9) == 0)    setDrumVol(atoi(line + 9));    // 0..150 %
    else if (strncmp(line, "@BPM=", 5) == 0)        setMasterBpm(atoi(line + 5));  // master tempo (song+drum)
    else if (strncmp(line, "@DRUMSYNCHRO=", 13) == 0) { g_drumSynchro = (atoi(line + 13) != 0);   // start-on-first-note
                                 Serial.printf("[drum] synchro start %s\n", g_drumSynchro ? "ON (play a note to start)" : "off (start on Play)"); }
    else if (strncmp(line, "@HPF=", 5) == 0)      setDacHpfMode(atoi(line + 5));
    else if (strncmp(line, "@LOOP=", 6) == 0)   { g_loop = (atoi(line + 6) != 0);
                                 Serial.printf("[song] loop %s\n", g_loop ? "ON" : "off"); }
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
    else if (strncmp(line, "@MPESWEEP=", 10) == 0) runMpeSweep(atoi(line + 10));   // MPE demo on each instrument from <start>
    else if (strncmp(line, "@PROOF=", 7) == 0)     runAxisProof(atoi(line + 7));   // capture 1 note w/ axis at full (0=press 1=timbre 2=bend 3=neutral)
    else if (strcmp(line, "@MPECHECK") == 0)       runMpeCheck();                  // QA every instrument under MPE (silent/clip)
    else if (strncmp(line, "@LFOMODE=", 9) == 0) {     // 0 = respect patch LFO, 1 = force LFO
        bool force = atoi(line + 9) != 0;
        g_poolSink.setLfoForce(force);
        synthSetInstrument(g_synthInstrument);         // reload so RESPECT restores the patch's own LFO
        Serial.printf("[lfo] mode = %s\n", force ? "FORCE (vib/trem on any patch)" : "RESPECT patch LFO");
    }
#endif
#ifdef TDSP_SYNTH_SF2_TSF
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
    // --- Arpeggiator (lib/TDspArp) — steps at the master BPM via the Conductor ---
    else if (strncmp(line, "@ARPON=", 7) == 0) {           // on (arpeggiate live notes) / off (bypass)
        g_arpFilter.setEnabled(atoi(line + 7) != 0);
        reply.printf("@ARPON=%d\n", g_arpFilter.enabled() ? 1 : 0);
        Serial.printf("[arp] %s\n", g_arpFilter.enabled() ? "ON" : "bypass");
    }
    else if (strncmp(line, "@ARPPAT=", 8) == 0) {          // pattern 0..23 (Up/Down/UpDown/.../Euclidean)
        g_arpFilter.setPattern((tdsp::ArpFilter::Pattern)atoi(line + 8));
        reply.printf("@ARPPAT=%d\n", (int)g_arpFilter.pattern());
    }
    else if (strncmp(line, "@ARPRATE=", 9) == 0) {         // rate 0..14 (1/1 .. 1/32t) — relative to master BPM
        g_arpFilter.setRate((tdsp::ArpFilter::Rate)atoi(line + 9));
        reply.printf("@ARPRATE=%d\n", (int)g_arpFilter.rate());
    }
    else if (strncmp(line, "@ARPGATE=", 9) == 0) {         // gate length %, 5..150
        g_arpFilter.setGate(atoi(line + 9) / 100.0f);
        reply.printf("@ARPGATE=%d\n", (int)(g_arpFilter.gate() * 100.0f + 0.5f));
    }
    else if (strncmp(line, "@ARPSWING=", 10) == 0) {       // swing 50..85 (%), 50 = straight
        g_arpFilter.setSwing(atoi(line + 10) / 100.0f);
        reply.printf("@ARPSWING=%d\n", (int)(g_arpFilter.swing() * 100.0f + 0.5f));
    }
    else if (strncmp(line, "@ARPOCT=", 8) == 0) {          // octave range 1..4
        g_arpFilter.setOctaveRange((uint8_t)atoi(line + 8));
        reply.printf("@ARPOCT=%d\n", g_arpFilter.octaveRange());
    }
    else if (strncmp(line, "@ARPLATCH=", 10) == 0) {       // latch: keep arpeggiating after keys release
        g_arpFilter.setLatch(atoi(line + 10) != 0);
        reply.printf("@ARPLATCH=%d\n", g_arpFilter.latch() ? 1 : 0);
    }
    // Full current-state snapshot (one JSON line) so the app can hydrate every card on
    // connect instead of assuming defaults. Reports what the device actually knows —
    // i.e. what's ACTIVE, not a UI "selection" the firmware never sees.
    else if (strcmp(line, "@STATE") == 0) {
        int volPct = (g_dvol <= -127.0f) ? 0 : (int)((g_dvol + 60.0f) / 0.60f + 0.5f);
        if (volPct < 0) volPct = 0; if (volPct > 100) volPct = 100;
        reply.printf("@STATE={\"vol\":%d,\"bpm\":%d,\"loop\":%d,", volPct, (int)(g_masterBpm + 0.5f), g_loop ? 1 : 0);
        reply.printf("\"arp\":{\"on\":%d,\"pat\":%d,\"rate\":%d,\"oct\":%d,\"latch\":%d},",
                     g_arpFilter.enabled() ? 1 : 0, (int)g_arpFilter.pattern(), (int)g_arpFilter.rate(),
                     g_arpFilter.octaveRange(), g_arpFilter.latch() ? 1 : 0);
        reply.printf("\"song\":{\"playing\":%d,\"i\":%d,\"p\":%d},", g_player.isPlaying() ? 1 : 0, g_songSel, g_player.positionPermille());
        reply.printf("\"drums\":{\"kit\":%d,\"playing\":%d},", g_drumKit, g_drumPlayer.isPlaying() ? 1 : 0);
        reply.print("\"voice\":{");
#if defined(TDSP_SYNTH_DEXED) || defined(TDSP_SYNTH_DEXED_POOL)
        if (g_curCartRel[0]) {   // last pick was a /dexed cart voice (@DXPICK)
            reply.print("\"cart\":"); tdsp::catdb::jsonStr(reply, g_curCartRel);
            reply.printf(",\"cv\":%d,\"name\":", g_curCartVoice); tdsp::catdb::jsonStr(reply, g_curCartName);
        } else
#endif
        { reply.printf("\"i\":%d,\"name\":", synthInstrument()); tdsp::catdb::jsonStr(reply, synthInstrumentName(synthInstrument())); }
        reply.print("}}\n");
    }
    else return false;
    return true;
}

void setup() {
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
    MTP.begin();
    if (g_sdReady) MTP.addFilesystem(SD, "T-DSP Songs");
#endif
    buildSongList();
    buildDrumList();   // scan /drums for loopable channel-10 grooves

    // Two pools now: the int16 pool feeds Dexed, the BT resampler, the optical-out
    // tone, and the input side of the convert blocks; the F32 pool feeds the mix
    // bus, converts, S/PDIF-in and the TDM output.
    AudioMemory(80);   // headroom for up to 4 OPM banks (ymfm multitimbral); Dexed uses far less
    AudioMemory_F32(60);
    setMix(1.0f, 0.0f, 1.0f);
    outL.gain(3, 0.62f);  outR.gain(3, 0.62f);  // synth (slot 3) mix make-up in the
                                                 // F32 domain, where there's real headroom.
    testTone.frequency(440.0f);  testTone.amplitude(0.0f);
    spdifTone.frequency(1000.0f); spdifTone.amplitude(0.25f);
    if (g_codecOk) applyVol();

    // Physical MIDI IN on Serial1 (pin 0), omni, soft-thru off -> the router.
    MIDI.begin(MIDI_CHANNEL_OMNI);
    MIDI.turnThruOff();
    MIDI.setHandleNoteOn(midiNoteOn);
    MIDI.setHandleNoteOff(midiNoteOff);
    MIDI.setHandlePitchBend(midiPitch);
    MIDI.setHandleControlChange(midiCC);
    // USB host: a controller (LinnStrument) plugged into the Teensy 4.1 host port.
    g_usbHost.begin();
    g_usbMidi.setHandleNoteOn(midiNoteOn);
    g_usbMidi.setHandleNoteOff(midiNoteOff);
    g_usbMidi.setHandleControlChange(midiCC);
    g_usbMidi.setHandlePitchChange(midiPitch);
    g_usbMidi.setHandleAfterTouchChannel(midiPressure);   // channel pressure = MPE Z-axis

    // Live MIDI -> arp -> synth. The arp is a router sink; in bypass (default) it
    // forwards every event verbatim to its downstream synth sink, so behaviour is
    // identical until @ARPON=1. It steps on the router's onClock() (fed by the
    // Conductor's 24-PPQN tick hook), so its rate divisions lock to the master BPM.
    // The song + drum players call g_synthSink DIRECTLY (below), bypassing the arp,
    // so only LIVE keyboard/app notes are arpeggiated — never the backing groove.
    g_arpFilter.setClock(&g_conductor.clock());
    g_arpFilter.addDownstream(g_synthSink);
    g_router.addSink(&g_arpFilter);

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
    g_router.addSink(&g_clockSink);
    g_conductor.setTickHook(+[](void*){ g_router.handleClock(); }, nullptr);
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
    applyMidiMode(false);   // start in normal MIDI (after synthBegin, so the engine exists)

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

#if TDSP_HAS_SDCARD
    MTP.loop();   // service USB file transfers to/from the SD (host drag-and-drop)
#endif

    // Advance the master clock BEFORE draining MIDI. In Internal mode it emits
    // catch-up 24-PPQN ticks (fanned to onClock() consumers via the tick hook)
    // and fires bar-edge callbacks to followers; in External mode this is the
    // stall watchdog. Cheap when nothing's due.
    g_conductor.update(micros());

    // Live MIDI: drain DIN + USB-host controllers, then advance the (non-blocking) song.
    while (MIDI.read()) { /* handlers fire per message */ }
    g_usbHost.Task();
    while (g_usbMidi.read()) { /* USB-host MIDI handlers fire per message */ }
    g_player.tick();
    g_drumPlayer.tick();   // loops internally (setLooping), so no external re-arm needed
    g_arpFilter.tick(micros());   // drain the arp's gate-off queue (note steps fire on onClock)
    songLoopTick();   // auto-restart the song if loop mode is on and it just ended

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

    g_sdWrite.tick(Serial, millis());   // abort a stalled @WB transfer (watchdog)

    // USB CDC input serves two roles: '@'-prefixed control LINES (the same protocol
    // the ESP32 relays from BLE — lets a Web Serial browser page drive the device with
    // NO ESP32 attached) and single debug KEYS (t/a/s/W/...). A byte of '@' starts a
    // command line; anything else is a key. They can't collide (keys are never '@').
    static char usbLine[160];
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
            else if (c == '+') { g_dvol += 3.0f; if (g_dvol > 0) g_dvol = 0;
                                 applyVol(); Serial.printf("[cmd] vol %.0f dB\n", g_dvol); }
            else if (c == '-') { g_dvol -= 3.0f; if (g_dvol < -60) g_dvol = -60;
                                 applyVol(); Serial.printf("[cmd] vol %.0f dB\n", g_dvol); }
            else if (c == 'd') { Serial.printf("[reg] RX_OFF(26)=%02X RX_CH1(28)=%02X RX_CH2(29)=%02X "
                                 "CH_EN(76)=%02X PWR(78)=%02X\n",
                                 g_codec.readRegister(0, 0x26), g_codec.readRegister(0, 0x28),
                                 g_codec.readRegister(0, 0x29), g_codec.readRegister(0, 0x76),
                                 g_codec.readRegister(0, 0x78)); }
            else if (c == 'i') { Serial.println("[cmd] re-init codec"); setupCodec(); applyVol();
                                 Serial.printf("[cmd] codec=%s (%s), vol %.0f dB\n",
                                               g_codecOk ? "OK" : "FAIL", g_codecMsg, g_dvol); }
            else if (c == 'W') { if (g_player.isPlaying()) songStop(); else songStart(g_songSel); }  // play/stop
            else if (c == 'S') { if (g_numSongs) g_songSel = (g_songSel + 1) % g_numSongs;  // pick song
                                 Serial.printf("[song] selected: %s\n", g_songs[g_songSel].name); }
            else if (c == 'D') { if (g_drumPlayer.isPlaying()) drumStop(); else drumStart(g_drumSel); }  // drums play/stop
            else if (c == 'C') { if (g_numDrums) g_drumSel = (g_drumSel + 1) % g_numDrums;   // Cycle groove
                                 Serial.printf("[drum] selected: %s\n", g_drums[g_drumSel].name);
                                 if (g_drumPlayer.isPlaying()) drumStart(g_drumSel); }
            else if (c == 'V') { synthSetInstrument((synthInstrument() + 1) % synthNumInstruments());
                                 if (g_mpeMode) synthSetMpeMode(true); }   // re-sync ch10 (MPE member)
            else if (c == 'M') { Serial.printf("[mem] external PSRAM: %u MB\n", external_psram_size); }
            else if (c == 'T') { runInstrumentSelfTest(); }   // exercise all 128 GM + drums, log peaks
            else if (c == 'B') { runPitchBendTest(); }         // audible pitch-bend sweep on ch1
            else if (c == 'E') { applyMidiMode(!g_mpeMode); }  // toggle MIDI <-> MPE mode locally
            else if (c == 'O') { g_loop = !g_loop; Serial.printf("[song] loop %s\n", g_loop ? "ON" : "off"); }  // lOop toggle
            else if (c == 'A') { runMpeTest(); }               // simulate an MPE note (bend + pressure)
#ifdef TDSP_SYNTH_DEXED_POOL
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
#ifdef TDSP_HAS_REPLAYGAIN
            else if (c == 'N') { runGainSweep(); }              // ReplayGain: sweep every voice, print trim table
#endif
        }
    }

    // Mirror the ESP32's UART log to USB, line-buffered with an [esp] prefix.
    static char line[160];
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
        float pbt = peakBt.available()    ? peakBt.read()    : 0.0f;
        float psp = peakSpdif.available() ? peakSpdif.read() : 0.0f;
        float po  = peakOut.available()   ? peakOut.read()   : 0.0f;
        Serial.printf("alive up=%lus  codec=%s(%s)  spdif=%s inFreq=%.0f  "
                      "btPeak=%.3f spdifPeak=%.3f outPeak=%.3f  cpuMax=%.1f%% memMax=%u\n",
                      (unsigned long)(millis() / 1000),
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

