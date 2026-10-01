// OpllBank.h — OPLL (YM2413) user-voice patch banks loaded from the SD card.
//
// The YM2413 has 15 melodic voices in ROM and ONE programmable "user voice": 8 register bytes
// ($00..$07). Every other OPLL timbre in the world (the Yamaha PortaSound keyboards' 100-voice
// sets, VRC7 / YMF281B ROM variants, tracker presets, game rips) is just such an 8-byte set.
// This loader reads any number of them from text files under one SD folder and exposes them as a
// flat, bank-tagged catalog, so an instrument picker can show each file as its own folder.
//
// FILE FORMAT  (/opll/<Bank Name>.txt — produced by tools/fetch_opll_patches.py, trivially hand-written)
//   # comment lines start with '#'
//   #name: Yamaha PSS-140          <- optional: display name for the bank (default = file name sans .txt)
//   13 01 18 0F 9E 60 00 9F<TAB>Piano 1
//   ^ exactly 8 hex bytes (no prefix, space-separated) = OPLL regs $00..$07, then a TAB (or 2+ spaces)
//     and the patch name (up to 31 chars; longer names are cut).
//
// MEMORY: patches live in PSRAM when the board has it (extmem_malloc), else in the heap, capped so a
// no-PSRAM build can't be starved by a big library. 40 bytes/patch -> 2,000 patches = 80 KB.
#pragma once
#include <Arduino.h>
#include <FS.h>

namespace tdsp {

struct OpllPatch {
    uint8_t regs[8];
    char    name[31];
    uint8_t bank;        // index into bankName()
};

class OpllBankLib {
public:
    static constexpr int kMaxBanks = 32;

    // Scan `dir` for *.txt banks. maxPatches caps total storage (heap fallback is further capped
    // to heapCap when there is no PSRAM). Safe to call with no card / no folder: count() stays 0.
    bool begin(FS& fs, const char* dir = "/opll", int maxPatches = 4096, int heapCap = 512);

    int   count() const { return count_; }
    const OpllPatch& patch(int i) const { return patches_[i]; }
    int   bankCount() const { return bankCount_; }
    const char* bankName(int b) const { return (b >= 0 && b < bankCount_) ? bankNames_[b] : ""; }
    // First patch index of bank b and how many it has (patches are stored bank-contiguous).
    int   bankStart(int b) const { return (b >= 0 && b < bankCount_) ? bankStart_[b] : 0; }
    int   bankSize(int b) const  { return (b >= 0 && b < bankCount_) ? bankSize_[b] : 0; }
    // Case-insensitive bank lookup by display name OR file name (sans .txt), -1 if absent.
    int   findBank(const char* name) const;
    const char* bankFile(int b) const { return (b >= 0 && b < bankCount_) ? bankFiles_[b] : ""; }
    bool  inPsram() const { return psram_; }

private:
    bool loadFile(FS& fs, const char* path, const char* defaultName);
    static bool parseLine(const char* line, uint8_t regs[8], char* name, size_t nameCap);

    OpllPatch* patches_ = nullptr;
    int        cap_ = 0, count_ = 0;
    bool       psram_ = false;
    int        bankCount_ = 0;
    char       bankNames_[kMaxBanks][32];   // display names ("#name:" line, else the file name)
    char       bankFiles_[kMaxBanks][32];   // file names sans .txt (stable id for lookups)
    int        bankStart_[kMaxBanks];
    int        bankSize_[kMaxBanks];
};

} // namespace tdsp
