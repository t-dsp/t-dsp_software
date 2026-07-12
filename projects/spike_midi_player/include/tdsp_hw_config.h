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
