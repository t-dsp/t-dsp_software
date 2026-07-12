// YmfmOpmMulti — a multitimbral manager over several AudioSynthYmfmOPM "banks".
//
//   MIDI (channel-addressed)  ─▶  YmfmOpmMulti  ─▶  bank[c] (one emulated OPM)
//                                   channel→bank        │  each: own patch, 8 voices
//                                   routing             ▼
//                                              mixer ─▶ TDM ─▶ DAC
//
// Each bank is a full independent YM2151 emulation (8 voices, its own patch), so
// N banks give N distinct simultaneous timbres and N×8 total polyphony. The banks
// themselves are Teensy Audio objects and must be declared at file scope (their
// constructors register with the audio graph); this manager only *borrows*
// pointers to them and adds the channel routing — mirroring how MpeVaSink borrows
// a voice pool. Combined with AudioSynthYmfmOPM's idle gate, banks with no notes
// cost ~nothing, so instantiating several is cheap until they actually play.
//
// Routing model (multitimbral): MIDI channel 1..16 maps to a bank index. Default
// map is (channel-1) % count, so ch1→bank0, ch2→bank1, … wrapping around. Assign
// a different instrument to each bank with setBankVoice(); optionally wire MIDI
// Program Change to a voice table with setVoiceTable().

#pragma once

#include <stdint.h>
#include "AudioSynthYmfmOPM.h"
#include "OpmVoice.h"

namespace tdsp {
namespace ymfmopm {

class YmfmOpmMulti {
public:
    static constexpr int kMaxBanks = 16;

    // banks: array of `count` AudioSynthYmfmOPM* (file-scope statics that outlive
    // this manager). count is clamped to [1, kMaxBanks].
    YmfmOpmMulti(AudioSynthYmfmOPM **banks, int count) {
        m_count = (count < 1) ? 1 : (count > kMaxBanks ? kMaxBanks : count);
        for (int i = 0; i < m_count; i++) m_banks[i] = banks[i];
        for (int ch = 1; ch <= 16; ch++) m_chanBank[ch] = (ch - 1) % m_count;  // default map
    }

    int count() const { return m_count; }
    AudioSynthYmfmOPM *bank(int i) { return (i >= 0 && i < m_count) ? m_banks[i] : nullptr; }

    // begin() every bank (call once in setup, after AudioMemory).
    void begin() { for (int i = 0; i < m_count; i++) m_banks[i]->begin(); }

    // --- configuration ---------------------------------------------------------
    // Route a MIDI channel (1..16) to a bank index (0..count-1).
    void setChannelBank(uint8_t channel, int bank) {
        if (channel >= 1 && channel <= 16 && bank >= 0 && bank < m_count) m_chanBank[channel] = (int8_t)bank;
    }
    int bankForChannel(uint8_t channel) const {
        return (channel >= 1 && channel <= 16) ? m_chanBank[channel] : 0;
    }

    // Load a patch into one bank (its instrument).
    void setBankVoice(int bank, const OpmVoice &v) {
        if (bank >= 0 && bank < m_count) m_banks[bank]->setVoice(v);
    }

    // Optional Program Change support: give the manager a table of voices; a
    // Program Change on a channel then selects table[program % size] for that
    // channel's bank. Pointers must outlive the manager.
    void setVoiceTable(const OpmVoice *const *table, int size) { m_voices = table; m_numVoices = size; }

    // --- performance (call from loop / MIDI handlers, not the ISR) -------------
    void noteOn(uint8_t channel, uint8_t note, uint8_t vel) {
        AudioSynthYmfmOPM *b = channelBank(channel);
        if (b) b->noteOn(note, vel);
    }
    void noteOff(uint8_t channel, uint8_t note) {
        AudioSynthYmfmOPM *b = channelBank(channel);
        if (b) b->noteOff(note);
    }
    void allNotesOff() { for (int i = 0; i < m_count; i++) m_banks[i]->allNotesOff(); }

    void programChange(uint8_t channel, uint8_t program) {
        if (!m_voices || m_numVoices <= 0) return;
        AudioSynthYmfmOPM *b = channelBank(channel);
        if (b) b->setVoice(*m_voices[program % m_numVoices]);
    }

    // Total voices sounding across all banks (telemetry).
    int activeVoices() const {
        int n = 0;
        for (int i = 0; i < m_count; i++) n += m_banks[i]->activeVoices();
        return n;
    }

private:
    AudioSynthYmfmOPM *channelBank(uint8_t channel) {
        int b = bankForChannel(channel);
        return (b >= 0 && b < m_count) ? m_banks[b] : nullptr;
    }

    AudioSynthYmfmOPM *m_banks[kMaxBanks] = {nullptr};
    int8_t             m_chanBank[17] = {0};   // index 1..16 -> bank (index 0 unused)
    int                m_count = 0;
    const OpmVoice *const *m_voices = nullptr;
    int                m_numVoices = 0;
};

} // namespace ymfmopm
} // namespace tdsp
