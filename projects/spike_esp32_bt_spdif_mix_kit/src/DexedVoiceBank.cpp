#include "DexedVoiceBank.h"
#include "dexed_banks_data.h"

#include <string.h>
#include <avr/pgmspace.h>

namespace tdsp {
namespace dexed {

namespace {

// The decoded VCED format Dexed::loadVoiceParameters() expects. 155 is
// the documented size for a single voice; the extra +1 slot mirrors
// the library examples that reserve a trailing byte for safety.
constexpr int kDecodedVoiceBytes = 156;

constexpr int kVmemBytes        = 128;
constexpr int kVmemNameOffset   = 117;

// Order must match the bank layout in dexed_banks_data.h. Names chosen
// to be short enough to fit in a dropdown without wrapping.
const char *const kBankNames[kNumBanks] = {
    "RitChie 1",
    "RitChie 2",
    "ROM 1A",
    "ROM 1B",
    "ROM 2A",
    "ROM 2B",
    "ROM 3A",
    "ROM 3B",
    "ROM 4A",
    "ROM 4B",
};

inline bool indicesValid(int bank, int voice) {
    return bank >= 0 && bank < kNumBanks &&
           voice >= 0 && voice < kVoicesPerBank;
}

} // namespace

const char *bankName(int bank) {
    if (bank < 0 || bank >= kNumBanks) return "Bank ?";
    return kBankNames[bank];
}

bool copyVoiceName(int bank, int voice, char *out, int outLen) {
    if (!indicesValid(bank, voice) || out == nullptr || outLen < kVoiceNameBufBytes) {
        return false;
    }

    // PROGMEM access — must use pgm_read_byte, not direct indexing.
    for (int i = 0; i < kVoiceNameLen; ++i) {
        out[i] = (char)pgm_read_byte(&progmem_bank[bank][voice][kVmemNameOffset + i]);
    }
    out[kVoiceNameLen] = '\0';

    // Trim trailing spaces — DX7 voice names are space-padded to 10
    // chars. A display like "FM-Rhodes " looks messy next to a
    // trimmed "FM-Rhodes".
    for (int i = kVoiceNameLen - 1; i >= 0; --i) {
        if (out[i] == ' ') out[i] = '\0';
        else break;
    }
    return true;
}

bool loadVoice(AudioSynthDexed &engine, int bank, int voice) {
    if (!indicesValid(bank, voice)) return false;

    // Copy the packed VMEM out of PROGMEM into a local RAM buffer so
    // Dexed::decodeVoice can read it with normal pointer arithmetic —
    // it does not use pgm_read_byte internally.
    uint8_t packed[kVmemBytes];
    memcpy_P(packed, &progmem_bank[bank][voice][0], kVmemBytes);

    uint8_t decoded[kDecodedVoiceBytes];
    // Dexed::decodeVoice(output, input) — the library's own Banks.ino
    // example has these reversed. Don't copy-paste from there.
    if (!engine.decodeVoice(decoded, packed)) return false;

    engine.loadVoiceParameters(decoded);
    return true;
}

} // namespace dexed
} // namespace tdsp
