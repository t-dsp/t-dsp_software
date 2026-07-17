// tdsp_hw_config.h — modular hardware configuration for the T-DSP firmware.
//
// The T-DSP runs on more than one physical board. The differences that the
// firmware actually cares about are selected here via PlatformIO build flags,
// so a single source tree targets every hardware variant:
//
//   * Board:  Teensy 4.0 vs 4.1  (the 4.1 has BUILTIN_SDCARD; the 4.0 doesn't)
//   * I2C:    codec direct on the Wire bus, vs behind a TCA9544A I2C mux
//             (the teensy41_digital_audio_board routes the TAC5212 through one)
//
// Each platformio.ini env passes the matching -D flags; see that file for the
// teensy40/41 x mux/nomux matrix. Nothing here is auto-detected at runtime —
// the build is pinned to one hardware configuration, which keeps the codec
// bring-up path deterministic.

#pragma once

#include <Arduino.h>
#include <Wire.h>

// --- Board profile header (optional) ----------------------------------------
// A build may select a physical-board profile with
//   -D TDSP_BOARD_HEADER="boards/<name>.h"
// The header sets only the TDSP_HAS_* / TDSP_ROLE_* / TDSP_DEFAULT_* macros that
// differ for that board; everything it leaves unset falls through to the #ifndef
// defaults below. Included FIRST so a board's values win over the defaults.
#ifdef TDSP_BOARD_HEADER
  #include TDSP_BOARD_HEADER
#endif

// --- Board ------------------------------------------------------------------
// An env may set TDSP_BOARD_TEENSY40 / _TEENSY41 explicitly. If it doesn't,
// fall back to the Teensy core's own ARDUINO_TEENSY4x macro so the firmware
// still builds with a bare `pio run` against either board.
#if !defined(TDSP_BOARD_TEENSY40) && !defined(TDSP_BOARD_TEENSY41)
  #if defined(ARDUINO_TEENSY40)
    #define TDSP_BOARD_TEENSY40 1
  #else
    #define TDSP_BOARD_TEENSY41 1  // historical default for this project
  #endif
#endif

// --- SD card ----------------------------------------------------------------
// Only the Teensy 4.1 exposes BUILTIN_SDCARD (4-bit SDIO). On the 4.0 the
// sampler slot has no card to stream from, so its SD init is compiled out and
// the slot simply stays silent — the rest of the graph is unaffected.
#if defined(TDSP_BOARD_TEENSY41)
  #define TDSP_HAS_SDCARD 1
#else
  #define TDSP_HAS_SDCARD 0
#endif

// --- Optical S/PDIF (TOSLINK) -----------------------------------------------
// The digital-audio carrier board has optical IN (FCR6842032R -> Teensy
// SPDIF_IN pin 15) and optical OUT (FCR6842032T <- Teensy SPDIF_OUT pin 14).
//
// Enabled on BOTH Teensy 4.0 and 4.1: the S/PDIF peripheral and the pins
// (14/15) are identical on both (same IMXRT1062), and internal RAM is the same
// size, so optical works on either. The optical hardware is on the carrier
// board, not the Teensy. A bare board with no optical source just reads "no
// signal" (in) / transmits silence (out) — harmless. Override -D ...=0 to drop.
#ifndef TDSP_HAS_SPDIF
  #define TDSP_HAS_SPDIF 1
#endif

// Optical INPUT is gated separately because it's the heavy part: the async
// S/PDIF receiver's resampler carries ~190 KB of coefficient/window tables.
// Those are relocated to DMAMEM (see lib/Audio/Resampler.cpp) and cold code is
// moved to FLASHMEM (see main.cpp) so the input fits in RAM1 alongside the full
// synth firmware on either board. Set -D TDSP_HAS_SPDIF_IN=0 to drop just the
// input (keeps output) if you need the RAM/flash back.
#ifndef TDSP_HAS_SPDIF_IN
  #define TDSP_HAS_SPDIF_IN 1
#endif
#if TDSP_HAS_SPDIF_IN && !TDSP_HAS_SPDIF
  #undef TDSP_HAS_SPDIF
  #define TDSP_HAS_SPDIF 1   // input implies the S/PDIF subsystem is present
#endif

// --- I2C mux (TCA9544A) ------------------------------------------------------
// Set TDSP_HAS_I2C_MUX=1 for boards where the codec sits behind a TCA9544A
// 4-channel mux. Address + channel default to the teensy41_digital_audio_board
// wiring (mux at 0x70, TAC5212 on channel 0); override per-env if a future
// board straps them differently.
#ifndef TDSP_HAS_I2C_MUX
  #define TDSP_HAS_I2C_MUX 0
#endif
#ifndef TDSP_MUX_ADDR
  #define TDSP_MUX_ADDR 0x70
#endif
#ifndef TDSP_MUX_CHANNEL
  #define TDSP_MUX_CHANNEL 0
#endif

// TCA9544A control byte: bit 2 (0x04) enables a channel, bits 1..0 select it.
#define TDSP_TCA9544A_CTRL(ch) ((uint8_t)(0x04 | ((ch) & 0x03)))

// Point the TCA9544A at the codec's downstream channel so the TAC5212 (and any
// other device on that channel) becomes visible on the main Wire bus. A no-op
// on direct-wired boards. Call ONCE after Wire.begin() and before any codec
// I2C transaction. The mux holds its selection until changed, and nothing else
// on this board lives on the other channels, so a one-shot select is enough —
// every later TAC5212 driver transaction then "just works" at 0x51.
static inline void tdspMuxSelectCodec() {
#if TDSP_HAS_I2C_MUX
  Wire.beginTransmission(TDSP_MUX_ADDR);
  Wire.write(TDSP_TCA9544A_CTRL(TDSP_MUX_CHANNEL));
  uint8_t err = Wire.endTransmission();
  Serial.printf("[mux] TCA9544A 0x%02X -> channel %d (err=%u)\n",
                TDSP_MUX_ADDR, TDSP_MUX_CHANNEL, err);
#endif
}

// Auto-detect the mux channel carrying the codec. Probes codecAddr on each of
// the 4 TCA9544A channels and leaves the mux pointed at the first that ACKs,
// returning that channel (0-3). This makes the firmware port-agnostic: the
// board's 4 TDM headers each map to a different mux channel, so the same
// daughtercard works in any header without a rebuild. Returns -1 if the codec
// isn't found on any channel, leaving TDSP_MUX_CHANNEL selected as a defined
// fallback. On direct-wired boards this is a no-op returning -1.
static inline int tdspMuxAutoSelectCodec(uint8_t codecAddr) {
#if TDSP_HAS_I2C_MUX
  for (uint8_t ch = 0; ch < 4; ch++) {
    Wire.beginTransmission(TDSP_MUX_ADDR);
    Wire.write(TDSP_TCA9544A_CTRL(ch));
    if (Wire.endTransmission() != 0) continue;       // mux control write failed
    Wire.beginTransmission(codecAddr);
    if (Wire.endTransmission() == 0) {               // codec ACKed on this channel
      Serial.printf("[mux] codec 0x%02X found on TCA9544A 0x%02X channel %u\n",
                    codecAddr, TDSP_MUX_ADDR, ch);
      return ch;
    }
  }
  // Not found anywhere — select the configured default so state is defined.
  Wire.beginTransmission(TDSP_MUX_ADDR);
  Wire.write(TDSP_TCA9544A_CTRL(TDSP_MUX_CHANNEL));
  Wire.endTransmission();
  Serial.printf("[mux] codec 0x%02X NOT found on any channel; default ch%u\n",
                codecAddr, TDSP_MUX_CHANNEL);
  return -1;
#else
  (void)codecAddr;
  return -1;
#endif
}

// ===========================================================================
// Capability / role / default macro families (board-configurable)
// ===========================================================================
// A board profile header (TDSP_BOARD_HEADER, included at the top of this file)
// may set any of these; whatever it leaves unset falls through to the defaults
// here. Every default reproduces the CURRENT firmware behaviour, so a build that
// selects no board header is byte-for-byte unchanged.
//
// (TDSP_HAS_SDCARD / _SPDIF / _SPDIF_IN / _I2C_MUX and the mux address/channel
// are defined above — the original hardware axis. The families below extend that
// same #ifndef pattern to the rest of the board's identity.)

// --- Hardware capabilities (what the board HAS) -----------------------------
#ifndef TDSP_HAS_ESP32_BT
#define TDSP_HAS_ESP32_BT 1          // ESP32 A2DP Bluetooth receiver (Serial7 ctrl + SAI2 audio)
#endif
#ifndef TDSP_HAS_DIN_MIDI
#define TDSP_HAS_DIN_MIDI 1          // 5-pin DIN MIDI IN on Serial1 (pin 0) via the H11L1 opto
#endif
#ifndef TDSP_HAS_USB_MIDI_HOST
#define TDSP_HAS_USB_MIDI_HOST 1     // USB host port: controller (e.g. LinnStrument) MIDI in
#endif
#ifndef TDSP_HAS_MIC_PREAMP
#define TDSP_HAS_MIC_PREAMP 0        // TAC5212 mic preamp path (0 = line-level input)
#endif

// Physical input / output type (informational now; routing hook later). Enumerated.
#define TDSP_IN_LINE       0
#define TDSP_IN_BALANCED   1
#define TDSP_IN_MIC        2
#ifndef TDSP_IN_TYPE
#define TDSP_IN_TYPE TDSP_IN_LINE
#endif
#define TDSP_OUT_HEADPHONE 0    // TAC5212 HpDriver  (mono-SE at OUTxP, 16 Ohm min load)
#define TDSP_OUT_LINE      1    // TAC5212 SeLine    (mono-SE line driver)
#define TDSP_OUT_BALANCED  2    // TAC5212 DiffLine  (differential / balanced line)
#ifndef TDSP_OUT_TYPE
#define TDSP_OUT_TYPE TDSP_OUT_HEADPHONE
#endif

// --- Roles (which SUBSYSTEMS are active; composable / additive) --------------
// Defaults defined now; main.cpp graph-gating on these is an incremental step
// (kept off tonight so the working audio graph is untouched).
#ifndef TDSP_ROLE_SYNTH
#define TDSP_ROLE_SYNTH 1            // synth engine + live MIDI + arp
#endif
#ifndef TDSP_ROLE_SONG_PLAYER
#define TDSP_ROLE_SONG_PLAYER 1      // baked/SD song player + drum-groove player
#endif
#ifndef TDSP_ROLE_BT_RECEIVER
#define TDSP_ROLE_BT_RECEIVER TDSP_HAS_ESP32_BT   // mix the A2DP stream in
#endif
#ifndef TDSP_ROLE_MIXER
#define TDSP_ROLE_MIXER 1            // F32 mix bus + master vol/HPF (always present today)
#endif

// --- Power-on defaults (baked; a board header overrides) ---------------------
// Two-stage master volume (see main.cpp):
//   (1) TDSP_DEFAULT_OUT_DVOL_DB -- the TAC5212 DAC's ANALOG output level, fixed
//       per board (headphone vs line calibration). Set once at codec init.
//   (2) TDSP_DEFAULT_APP_VOL_PCT -- the DIGITAL app master (tdmOut.setGain) start,
//       0..100 % (what the app fader / +/- keys drive). 0 = mute, 100 = unity.
// Defaults (codec 0 dB x ~67% digital ~= -20 dB) reproduce the legacy start level
// while keeping full range (100% = 0 dB analog, the old max).
#ifndef TDSP_DEFAULT_OUT_DVOL_DB
#define TDSP_DEFAULT_OUT_DVOL_DB (0.0f)
#endif
#ifndef TDSP_DEFAULT_APP_VOL_PCT
#define TDSP_DEFAULT_APP_VOL_PCT 67
#endif
#ifndef TDSP_DEFAULT_BPM
#define TDSP_DEFAULT_BPM 120.0f           // master clock start tempo (40..240)
#endif
#ifndef TDSP_DEFAULT_HPF_MODE
#define TDSP_DEFAULT_HPF_MODE 0           // 0=off,1=1Hz,2=12Hz,3=96Hz DAC highpass (not yet applied at boot)
#endif
#ifndef TDSP_DEFAULT_MPE
#define TDSP_DEFAULT_MPE 0                // 0 = start in normal MIDI, 1 = start in MPE
#endif
#ifndef TDSP_DEFAULT_ARP
#define TDSP_DEFAULT_ARP 0               // arp bypassed at boot (not yet applied)
#endif
#ifndef TDSP_DEFAULT_SYNTH_MAKEUP
#define TDSP_DEFAULT_SYNTH_MAKEUP 0.62f  // mix slot-3 synth make-up gain (F32 domain)
#endif
