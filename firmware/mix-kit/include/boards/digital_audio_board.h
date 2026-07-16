// boards/digital_audio_board.h — profile for the teensy41_digital_audio_board.
//
// This is the reference board the mix-kit was developed on: Teensy 4.1 + ESP32
// DevKitC (A2DP Bluetooth) + TAC5212 codec behind a TCA9544A I2C mux, optical
// S/PDIF in/out, DIN MIDI in, USB-host MIDI in, headphone out.
//
// Select it with:  -D TDSP_BOARD_HEADER="boards/digital_audio_board.h"
//
// A board header sets only what differs from the firmware defaults (see the
// #ifndef families at the end of tdsp_hw_config.h). Everything here matches the
// current firmware, so selecting this header is behaviour-identical — it just
// makes the board's identity explicit and reviewable in one place.
#pragma once

// Capabilities/roles use #ifndef so the precedence is: command-line -D (env
// override / testing) > this board header > tdsp_hw_config.h defaults. A build
// normally sets none of these, so the header's values apply as written.

// --- Capabilities this board HAS --------------------------------------------
#ifndef TDSP_HAS_ESP32_BT
#define TDSP_HAS_ESP32_BT      1
#endif
#ifndef TDSP_HAS_DIN_MIDI
#define TDSP_HAS_DIN_MIDI      1
#endif
#ifndef TDSP_HAS_USB_MIDI_HOST
#define TDSP_HAS_USB_MIDI_HOST 1
#endif
#ifndef TDSP_HAS_MIC_PREAMP
#define TDSP_HAS_MIC_PREAMP    0            // line-level input; no mic preamp populated
#endif
#ifndef TDSP_IN_TYPE
#define TDSP_IN_TYPE           TDSP_IN_LINE
#endif
#ifndef TDSP_OUT_TYPE
#define TDSP_OUT_TYPE          TDSP_OUT_HEADPHONE
#endif

// --- Roles this board runs ---------------------------------------------------
#ifndef TDSP_ROLE_SYNTH
#define TDSP_ROLE_SYNTH        1
#endif
#ifndef TDSP_ROLE_SONG_PLAYER
#define TDSP_ROLE_SONG_PLAYER  1
#endif
#ifndef TDSP_ROLE_MIXER
#define TDSP_ROLE_MIXER        1
#endif
// TDSP_ROLE_BT_RECEIVER defaults to TDSP_HAS_ESP32_BT (1 here).

// --- Power-on defaults (leave firmware defaults) -----------------------------
// Master -20 dB, 120 BPM, DAC HPF off, normal-MIDI start, 0.62 synth make-up.
// Override here per board, e.g. a line-out unit might want a hotter -6 dB start.

// NOTE (migration): TDSP_HAS_I2C_MUX and the RAM-tuning flags (TDSP_NO_SPDIF_IN,
// TDSP_LEAN_RAM) are still set by the platformio.ini env, not here — those move
// into board headers once envs are trimmed to just {board header, synth, RAM}.
