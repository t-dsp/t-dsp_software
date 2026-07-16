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

// --- Lazy directory browser (for the full /dexed subfolder library) ----------
// Unlike the capped/name-cached flat scan above, these hold NOTHING in RAM: each
// call does one on-demand SD directory (or file) read, so the browser scales to
// the whole ~3,700-cart library organized in subfolders. All paths are relative
// to /dexed (rel == "" or nullptr means the /dexed root). Runs from loop/handlers.
struct SdDirEntry {
    char name[64];
    bool isDir;
};

// List the entries of /dexed/<rel>: subfolders and 32-voice carts (4104/4096-byte
// .syx only; other files/sizes are skipped). Fills out[0..pageSize) for page
// `page` (0-based) in directory order; sets *total to the full accepted-entry
// count. Returns the number filled. NB: counts the whole directory each call
// (fine for typical folders; a folder with thousands of files is slower).
int sdListDir(const char *rel, int page, int pageSize, SdDirEntry *out, int *total);

// Read the 32 voice names from the cart at /dexed/<relCart>. Returns 32, or 0 on
// a missing/invalid cart.
int sdCartVoiceNames(const char *relCart, char names[kVoicesPerBank][kVoiceNameBufBytes]);

// Load voice `voice` (0..31) from the cart at /dexed/<relCart> into `engine`.
// Runs from loop/handlers (never the ISR); caller should engine.panic() first.
bool sdLoadCartVoice(AudioSynthDexed &engine, const char *relCart, int voice);

} // namespace dexed
} // namespace tdsp
