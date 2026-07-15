// DexedSdBank — loads DX7 32-voice cartridges (*.syx) off the SD card so the
// Dexed backend can browse thousands of downloaded voices, not just the 320
// bundled in PROGMEM.
//
// The open DX7 patch ecosystem is huge (BlackWinny's Dexed_cart, MiniDexed's
// ~31k deduped voices, Bobby Blues' "All The Web"). Those all ship as standard
// 32-voice bulk SysEx files. Drop them into /dexed on the card (it mounts over
// USB via MTP) and they appear in the app catalog after the bundled banks.
//
// Scaling: a full library is far too big for RAM, so this caches only the voice
// NAMES at boot (cheap) and reads each voice's 128-byte packed VMEM from the file
// ON DEMAND when it's selected. Capped at kMaxSdBanks banks to bound the name
// cache (raise TDSP_DEXED_MAX_SD_BANKS if you want more resident at once).
//
// DX7 32-voice bulk format: F0 43 0n 09 20 00 <4096 data> <cksum> F7 (4104 bytes;
// payload = 32 voices x 128-byte packed VMEM at offset 6). A headerless 4096-byte
// raw dump is also accepted.
#pragma once

#include <stdint.h>
#include <synth_dexed.h>

#include "DexedVoiceBank.h"   // shared kVoicesPerBank / kVoiceNameLen / kVoiceNameBufBytes

namespace tdsp {
namespace dexed {

#ifndef TDSP_DEXED_MAX_SD_BANKS
#define TDSP_DEXED_MAX_SD_BANKS 64          // 64 banks x 32 = 2048 SD voices
#endif
constexpr int kMaxSdBanks = TDSP_DEXED_MAX_SD_BANKS;

// Scan /dexed for *.syx 32-voice carts. Caches bank filenames + the 32 voice
// names per bank (up to kMaxSdBanks). Safe to call when no card / no dir — just
// returns 0. Returns the number of banks found.
int scanSdBanks();

int  numSdBanks();
int  numSdVoices();                          // numSdBanks() * kVoicesPerBank
const char *sdBankName(int bank);            // filename without path/.syx
bool copySdVoiceName(int bank, int voice, char *out, int outLen);

// Load SD (bank, voice): re-opens the file, reads the voice's 128-byte VMEM,
// decodes + loads it into `engine`. Runs from loop/handlers (never the ISR).
// Does NOT panic held notes — caller should engine.panic() first.
bool loadSdVoice(AudioSynthDexed &engine, int bank, int voice);

} // namespace dexed
} // namespace tdsp
