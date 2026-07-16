// boards/digital_audio_board_nobt.h — digital-audio board WITHOUT the ESP32.
//
// The same teensy41_digital_audio_board PCB, but built with the ESP32 DevKitC not
// populated (a cheaper / Bluetooth-free SKU). Everything else is identical, so this
// profile differs from digital_audio_board.h in exactly one line: no ESP32.
//
// TDSP_HAS_ESP32_BT 0 makes TDSP_ROLE_BT_RECEIVER default to 0, which graph-gates
// the A2DP input branch out of the build (see main.cpp #if TDSP_ROLE_BT_RECEIVER) —
// reclaiming ~103 KB of DTCM (the async I2S resampler's filter[]). That headroom is
// enough to re-enable an engine or optical S/PDIF-in that wouldn't otherwise fit.
//
// Select it with:  -D TDSP_BOARD_HEADER="boards/digital_audio_board_nobt.h"
#pragma once

// --- Capabilities this board HAS --------------------------------------------
#define TDSP_HAS_ESP32_BT      0            // ESP32 not populated -> no Bluetooth, no A2DP receiver
#define TDSP_HAS_DIN_MIDI      1
#define TDSP_HAS_USB_MIDI_HOST 1
#define TDSP_HAS_MIC_PREAMP    0
#define TDSP_IN_TYPE           TDSP_IN_LINE
#define TDSP_OUT_TYPE          TDSP_OUT_HEADPHONE

// --- Roles this board runs ---------------------------------------------------
#define TDSP_ROLE_SYNTH        1
#define TDSP_ROLE_SONG_PLAYER  1
#define TDSP_ROLE_MIXER        1
// TDSP_ROLE_BT_RECEIVER defaults to TDSP_HAS_ESP32_BT (0 here) -> A2DP path dropped.

// Power-on defaults left at firmware defaults (see tdsp_hw_config.h).
// NOTE (migration): TDSP_HAS_I2C_MUX + RAM-tuning flags are still set by the env,
// same as boards/digital_audio_board.h.
