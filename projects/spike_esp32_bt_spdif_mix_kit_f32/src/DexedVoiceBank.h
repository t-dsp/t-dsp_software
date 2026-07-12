// DexedVoiceBank — thin read-only accessor over the bundled PROGMEM
// voice banks in dexed_banks_data.h.
//
// The raw array is a [10][32][128] uint8_t block of VMEM (packed DX7
// cartridge) bytes. This wrapper:
//   * exposes bank / voice counts and user-facing names;
//   * unpacks a selected voice into VCED and loads it into an
//     AudioSynthDexed engine.
//
// The decode + load path ends up copying ~155 bytes per voice change,
// which is cheap enough to run from the main loop (caller's
// responsibility — synth_dexed is NOT threadsafe with its own audio
// interrupt, so load voices from setup() or from OSC/CLI handlers that
// run in the main task).

#pragma once

#include <stdint.h>
#include <synth_dexed.h>

namespace tdsp {
namespace dexed {

// The bundled data has exactly 10 banks of 32 voices each. These are
// compile-time constants to let callers size UI arrays without having
// to #include the 240 KB data header.
constexpr int kNumBanks          = 10;
constexpr int kVoicesPerBank     = 32;
constexpr int kVoiceNameLen      = 10;   // chars inside VMEM
constexpr int kVoiceNameBufBytes = kVoiceNameLen + 1; // incl. terminator

// Human-facing name for a bank (0..kNumBanks-1). Returns "Bank N" as a
// fallback for out-of-range. Returned pointer is valid for the lifetime
// of the program (points at a static string literal).
const char *bankName(int bank);

// Copies the 10-char voice name from PROGMEM into `out` and appends a
// NUL. `out` must be at least kVoiceNameBufBytes long. Trailing spaces
// are trimmed so UI labels are compact. Returns true on success.
bool copyVoiceName(int bank, int voice, char *out, int outLen);

// Unpacks the VMEM voice at (bank, voice) into VCED and loads it into
// `engine`. Returns false on out-of-range indices. Does NOT stop
// currently-sounding notes — callers that want a clean switch should
// call `engine.panic()` first.
bool loadVoice(AudioSynthDexed &engine, int bank, int voice);

} // namespace dexed
} // namespace tdsp
