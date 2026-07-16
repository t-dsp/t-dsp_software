// boards/jaymint.h — the "jay-mint" board (Teensy SN 7681380): PSRAM + LINE OUT.
//
// Same digital-audio board PCB as digital_audio_board.h (Teensy 4.1 + ESP32 +
// TAC5212 behind the TCA9544A mux), but populated with PSRAM and wired for LINE
// OUT instead of headphone. So it differs only in the output stage + startup
// levels; every other capability inherits the firmware defaults.
//
// Select with:  -D TDSP_BOARD_HEADER="boards/jaymint.h"
#pragma once

// #ifndef throughout so precedence is: -D override > this header > defaults.

// --- Output: single-ended LINE OUT (not headphone) --------------------------
#ifndef TDSP_OUT_TYPE
#define TDSP_OUT_TYPE TDSP_OUT_LINE          // TAC5212 SeLine driver
#endif

// --- Startup volumes (two-stage master; tune for your line stage) -----------
// (1) Codec analog output level, FIXED. Full-scale by default so the downstream
//     mixer/interface sets the working level; drop it (e.g. -6..-12 dB) if the
//     line output is too hot for the receiving gear.
#ifndef TDSP_DEFAULT_OUT_DVOL_DB
#define TDSP_DEFAULT_OUT_DVOL_DB (0.0f)
#endif
// (2) Digital app master start. Line out usually runs near unity so it feeds a
//     consistent level; the app fader can still pull it down.
#ifndef TDSP_DEFAULT_APP_VOL_PCT
#define TDSP_DEFAULT_APP_VOL_PCT 100
#endif

// Capabilities/roles otherwise identical to the digital-audio board -> left at
// firmware defaults (ESP32 BT, DIN + USB-host MIDI, synth/song/mixer roles). The
// I2C mux + RAM-tuning flags come from the env (same as digital_audio_board.h).
// PSRAM is auto-detected at boot (external_psram_size) — no macro needed.
