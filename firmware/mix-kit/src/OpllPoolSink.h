// OpllPoolSink — a MidiSink that fans notes across a POOL of OPLL chips
// (AudioSynthYmfmOPLLPool) so the YM2413 can do true 3-axis MPE.
//
//   * MPE mode  — one chip (slot) PER active note, keyed by member channel. That
//     channel's bend / pressure / TIMBRE drives just that slot, so every note has
//     independent pitch, volume, and brightness (CC#74). This is the axis a single
//     OPLL can't do (one shared user-voice bank).
//   * Normal MIDI — slots packed round-robin for polyphony; expression is applied
//     channel-wide to every slot on that channel.
//
// Each note resolves to an 8-byte user-voice patch: the picker override, else the
// channel's GM program mapped to a ROM voice. ROM voices are provided as user-voice
// byte sets (OpllRomVoices.h) so they're modulatable like the PSS-140 patches.
#pragma once
#include <stdint.h>
#include <AudioSynthYmfmOPLLPool.h>
#include <MidiSink.h>

class OpllPoolSink : public tdsp::MidiSink {
public:
    OpllPoolSink(AudioSynthYmfmOPLLPool *eng, int nSlots,
                 const uint8_t (*rom)[8], int nRom,
                 const uint8_t (*pss)[8], int nPss)
        : _eng(eng), _n(nSlots > AudioSynthYmfmOPLLPool::kMaxSlots ? AudioSynthYmfmOPLLPool::kMaxSlots : nSlots),
          _rom(rom), _nRom(nRom), _pss(pss), _nPss(nPss) {
        for (int c = 0; c < 17; c++) { _program[c] = 0; _override[c] = -1; }   // once; panic() must NOT reset these
        reset();
    }

    void setMpeMode(bool mpe) { panic(); _mpe = mpe; }
    bool mpeMode() const { return _mpe; }

    // Picker: force one voice index (0..nRom+nPss-1) on every channel (audition).
    void setVoiceAll(int idx) { for (int c = 1; c <= 16; c++) _override[c] = idx; }

    // --- MidiSink ------------------------------------------------------------
    void onNoteOn(uint8_t ch, uint8_t note, uint8_t vel) override {
        if (vel == 0) { onNoteOff(ch, note, 0); return; }
        if (ch < 1 || ch > 16) return;
        const uint8_t *patch = patchFor(ch);
        int slot = _mpe ? allocMpe(ch) : allocNormal();
        _slotOn[slot] = true; _slotCh[slot] = ch; _slotNote[slot] = note; _slotAge[slot] = ++_seq;
        if (_mpe) _chSlot[ch] = (int8_t)slot;
        _eng->slotNoteOn(slot, note, vel, patch);
    }
    void onNoteOff(uint8_t ch, uint8_t note, uint8_t) override {
        for (int s = 0; s < _n; s++)
            if (_slotOn[s] && _slotCh[s] == ch && _slotNote[s] == note) {
                _eng->slotNoteOff(s); _slotOn[s] = false;
                if (_mpe && _chSlot[ch] == s) _chSlot[ch] = -1;
                return;
            }
    }
    void onProgramChange(uint8_t ch, uint8_t prog) override {
        if (ch < 1 || ch > 16) return;
        _program[ch] = prog & 0x7F; _override[ch] = -1;   // song re-diversifies per channel
    }
    void onPitchBend(uint8_t ch, float semitones) override {
        forEachSlot(ch, [&](int s) { _eng->slotBend(s, semitones); });
    }
    void onPressure(uint8_t ch, float value) override {          // MPE Z
        forEachSlot(ch, [&](int s) { _eng->slotPressure(s, value); });
    }
    void onTimbre(uint8_t ch, float value) override {            // MPE Y (CC#74) — the new axis
        forEachSlot(ch, [&](int s) { _eng->slotTimbre(s, value); });
    }
    void onModWheel(uint8_t, float) override {}                  // no mod routing on OPLL
    void onAllNotesOff(uint8_t) override { panic(); }

    void panic() {
        _eng->allOff();
        reset();
    }

private:
    AudioSynthYmfmOPLLPool *_eng;
    int  _n;
    const uint8_t (*_rom)[8]; int _nRom;
    const uint8_t (*_pss)[8]; int _nPss;
    bool _mpe = false;

    bool     _slotOn[AudioSynthYmfmOPLLPool::kMaxSlots];
    uint8_t  _slotCh[AudioSynthYmfmOPLLPool::kMaxSlots];
    uint8_t  _slotNote[AudioSynthYmfmOPLLPool::kMaxSlots];
    uint32_t _slotAge[AudioSynthYmfmOPLLPool::kMaxSlots];
    int8_t   _chSlot[17];            // MPE: channel -> slot, -1 = none
    uint8_t  _rr = 0;               // normal-mode round-robin cursor
    uint32_t _seq = 0;

    uint8_t  _program[17];           // per-channel GM program
    int      _override[17];          // per-channel forced voice index, -1 = GM map

    // GM program -> ROM voice index (0..14). Mirrors the single-chip gmToInstrument
    // map (instrument 1..15) minus one.
    static uint8_t gmRom(uint8_t prog) {
        static const uint8_t k[128] = {
            2,2,2,2,2,2,10,10, 11,11,11,11,11,11,10,10, 7,7,7,7,7,7,7,7,
            1,1,1,14,14,14,14,14, 13,13,13,13,12,12,12,12, 0,0,0,0,0,0,0,11,
            0,0,0,0,9,9,9,9, 6,6,6,6,6,6,6,6, 5,5,5,5,4,4,4,4,
            3,3,3,3,3,3,3,3, 9,9,9,9,9,9,9,9, 9,9,9,9,9,9,9,9,
            9,9,9,9,9,9,9,9, 1,1,1,1,1,1,1,1, 11,11,11,11,11,11,11,11,
            9,9,9,9,9,9,9,9
        };
        return k[prog & 0x7F];
    }

    const uint8_t *patchFor(uint8_t ch) const {
        int idx = (_override[ch] >= 0) ? _override[ch] : (int)gmRom(_program[ch]);
        if (idx < 0) idx = 0;
        if (idx >= _nRom + _nPss) idx = _nRom + _nPss - 1;
        return (idx < _nRom) ? _rom[idx] : _pss[idx - _nRom];
    }

    // Clears NOTE/slot allocation only. Does NOT touch _program/_override — those are
    // voice selections that must survive panic()/all-notes-off/MPE-mode switches (else
    // entering MPE mode wipes the picked voice and notes fall back to GM program 0).
    void reset() {
        for (int s = 0; s < AudioSynthYmfmOPLLPool::kMaxSlots; s++) _slotOn[s] = false;
        for (int c = 0; c < 17; c++) _chSlot[c] = -1;
        _rr = 0; _seq = 0;
    }

    int stealOldest() {
        int o = 0;
        for (int s = 1; s < _n; s++) if (_slotAge[s] < _slotAge[o]) o = s;
        // vacate the stolen slot's channel mapping
        if (_mpe && _slotOn[o]) for (int c = 1; c <= 16; c++) if (_chSlot[c] == o) _chSlot[c] = -1;
        if (_slotOn[o]) _eng->slotNoteOff(o);
        return o;
    }
    int allocNormal() {
        for (int k = 0; k < _n; k++) {
            int s = (_rr + k) % _n;
            if (!_slotOn[s]) { _rr = (s + 1) % _n; return s; }
        }
        return stealOldest();
    }
    int allocMpe(uint8_t ch) {
        if (_chSlot[ch] >= 0 && _slotOn[_chSlot[ch]]) return _chSlot[ch];   // retrigger same channel
        for (int s = 0; s < _n; s++) if (!_slotOn[s]) return s;
        return stealOldest();
    }

    template <class F> void forEachSlot(uint8_t ch, F f) {
        if (ch < 1 || ch > 16) return;
        if (_mpe) { if (_chSlot[ch] >= 0 && _slotOn[_chSlot[ch]]) f(_chSlot[ch]); }
        else { for (int s = 0; s < _n; s++) if (_slotOn[s] && _slotCh[s] == ch) f(s); }
    }
};
