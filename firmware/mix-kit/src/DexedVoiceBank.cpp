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
constexpr int kVmemNameOffset   = 118;   // name is bytes 118..127; byte 117 is transpose
                                          // (reading 117 prepended a garbage transpose char)

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

uint8_t voiceLfoTags(int bank, int voice) {
    if (!indicesValid(bank, voice)) return 0;
    // Packed-VMEM LFO block: 112 = speed, 114 = pitch-mod-depth (PMD), 115 = amp-mod-depth
    // (AMD). Tag NATIVE movement — a running LFO (speed>0) with real depth — so the tag is
    // selective (patches that actually vibrato/tremolo), not the ~all-patches "could
    // respond to a controller" set (which is what FORCE mode gives you anyway).
    const uint8_t spd = pgm_read_byte(&progmem_bank[bank][voice][112]);
    const uint8_t pmd = pgm_read_byte(&progmem_bank[bank][voice][114]);
    const uint8_t amd = pgm_read_byte(&progmem_bank[bank][voice][115]);
    uint8_t tags = 0;
    if (spd > 0 && pmd > 0) tags |= 1;   // native vibrato
    if (spd > 0 && amd > 0) tags |= 2;   // native tremolo
    return tags;
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
