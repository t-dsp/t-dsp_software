# Board profiles (`include/boards/`)

One header per physical board. A build selects one with:

```ini
build_flags = -D TDSP_BOARD_HEADER=\"boards/<name>.h\"
```

`tdsp_hw_config.h` includes that header **first**, then fills every macro the
header left unset from its `#ifndef` defaults. So a board header lists **only what
differs** from the reference firmware; everything else falls through unchanged.

## What a board header sets

Three macro families (all defined with defaults in `tdsp_hw_config.h`):

- **Capabilities — what the board HAS.**
  `TDSP_HAS_ESP32_BT`, `TDSP_HAS_DIN_MIDI`, `TDSP_HAS_USB_MIDI_HOST`,
  `TDSP_HAS_MIC_PREAMP`, `TDSP_HAS_SDCARD`, `TDSP_HAS_SPDIF[_IN]`,
  `TDSP_HAS_I2C_MUX` (+ `TDSP_MUX_ADDR`/`_CHANNEL`), `TDSP_IN_TYPE`,
  `TDSP_OUT_TYPE`.
- **Roles — which subsystems are active** (composable/additive).
  `TDSP_ROLE_SYNTH`, `TDSP_ROLE_SONG_PLAYER`, `TDSP_ROLE_BT_RECEIVER`,
  `TDSP_ROLE_MIXER`. *(Defaults exist; graph-gating on them is incremental — see
  the follow-up note below.)*
- **Power-on defaults — how it boots.**
  `TDSP_DEFAULT_MASTER_DB`, `TDSP_DEFAULT_BPM`, `TDSP_DEFAULT_HPF_MODE`,
  `TDSP_DEFAULT_MPE`, `TDSP_DEFAULT_ARP`, `TDSP_DEFAULT_SYNTH_MAKEUP`.

## Build identity = board header × synth env

The **synth engine** stays a PlatformIO env (it drives `lib_deps` /
`build_src_filter` / linker and is memory-exclusive — one engine per binary). The
board header carries everything else. So a unit is fully described by
`(synth env) × (board header)`; `tools/boards.tsv` maps a Teensy serial number to
that pair and `tools/flash.py` uploads it.

## Adding a board

Copy `digital_audio_board.h`, change the lines that differ, point an env's
`TDSP_BOARD_HEADER` at it. Example — a balanced-in, line-out unit with no
Bluetooth and a hotter start level:

```c
#pragma once
#define TDSP_HAS_ESP32_BT   0
#define TDSP_IN_TYPE         TDSP_IN_BALANCED
#define TDSP_OUT_TYPE        TDSP_OUT_LINE
#define TDSP_DEFAULT_MASTER_DB (-6.0f)
```

## Current status / follow-ups

- Capability + default macros are **wired and behaviour-preserving** today
  (`digital_audio_board.h` reproduces the reference build exactly).
- `TDSP_HAS_I2C_MUX` and the RAM-tuning flags (`TDSP_NO_SPDIF_IN`,
  `TDSP_LEAN_RAM`) are still set by the env, not the header — they migrate in once
  envs are trimmed to `{board header, synth, RAM}`.
- `TDSP_ROLE_*` graph-gating in `main.cpp` (dropping the BT/SPDIF subsystems for a
  synth-only board) is a bench-verified follow-up, deliberately not done blind.
