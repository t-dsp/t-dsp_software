// DexedPoolSink — a MidiSink that fans notes across a POOL of AudioSynthDexed
// engines so a monotimbral FM engine can do real MPE.
//
// Dexed's pitch bend / mod / aftertouch are GLOBAL to one engine (see DexedSink.h),
// so a single engine can't give each note independent expression. This sink owns N
// engines and allocates them two ways depending on mode:
//
//   * MPE mode  — one engine PER active note (keyed by MPE member channel). A
//     channel's pitch bend / pressure is applied to just that engine, so note A can
//     bend while note B holds. Falls back to voice-stealing the oldest note when all
//     engines are busy.
//   * Normal MIDI — the pool is packed round-robin for polyphony (N * voicesPerEngine
//     total voices) and expression is BROADCAST to every engine, which is exactly
//     right for normal MIDI (channel-wide bend/pressure applies to all notes).
//
// The engines themselves live at file scope in the backend header (Teensy Audio
// objects must be statically allocated); this sink just drives them.
#pragma once

#include <stdint.h>
#include <synth_dexed.h>
#include <MidiSink.h>

class DexedPoolSink : public tdsp::MidiSink {
public:
    // engines: array of N engine pointers. voicesPerEngine: internal Dexed voices
    // each engine was constructed with (a note may need >1 for release overlap).
    DexedPoolSink(AudioSynthDexed **engines, uint8_t n, uint8_t voicesPerEngine)
        : _eng(engines), _n(n > 16 ? 16 : n), _vpe(voicesPerEngine) { reset(); }

    // Switch allocation strategy. Silences everything first so no stuck notes carry
    // a stale per-engine bend across the mode change.
    void setMpeMode(bool mpe) { panic(); _mpe = mpe; }
    bool mpeMode() const { return _mpe; }

    // --- MidiSink overrides -------------------------------------------------
    void onNoteOn(uint8_t ch, uint8_t note, uint8_t vel) override {
        if (vel == 0) { onNoteOff(ch, note, 0); return; }
        uint8_t e = _mpe ? allocMpe(ch) : allocNormal();
        Voice *v = freeVoice();
        if (!v) { v = stealOldest(); release(v); }   // table full: reclaim the oldest slot
        v->on = true; v->ch = ch; v->note = note; v->eng = e; v->age = ++_seq;
        _load[e]++;
        if (_mpe && ch >= 1 && ch <= 16) _chEng[ch] = (int8_t)e;   // set AFTER any steal
        _eng[e]->keydown(note, vel);
    }

    void onNoteOff(uint8_t ch, uint8_t note, uint8_t) override {
        for (uint8_t i = 0; i < maxVoices(); ++i)
            if (_v[i].on && _v[i].ch == ch && _v[i].note == note) { release(&_v[i]); return; }
    }

    // Bend sensitivity. The router / player already applied the per-channel range and
    // hands us the final semitone value, so we set Dexed's own sensitivity to a FIXED
    // kBendRange and map the semitones into +/-8192 counts. kBendRange must be >= the
    // widest bend we want to reproduce — a full MPE octave is 12 semitones. (Was 1,
    // which clamped every per-note bend to +/-1 semitone.)
    static constexpr int kBendRange = 12;
    void onPitchBend(uint8_t ch, float semitones) override {
        const int16_t counts = clampCounts((int)(semitones / (float)kBendRange * 8192.0f));
        forEachTarget(ch, [&](AudioSynthDexed *e) { e->setPitchbendRange((uint8_t)kBendRange); e->setPitchbend(counts); });
    }
    void onPressure(uint8_t ch, float value) override {
        const uint8_t v = toMidi7(value);
        forEachTarget(ch, [&](AudioSynthDexed *e) { e->setAftertouch(v); });
    }
    void onModWheel(uint8_t /*ch*/, float value) override {
        const uint8_t v = toMidi7(value);
        forEachEngine([&](AudioSynthDexed *e) { e->setModWheel(v); });
    }
    void onSustain(uint8_t /*ch*/, bool on) override {
        forEachEngine([&](AudioSynthDexed *e) { e->setSustain(on); });
    }
    void onAllNotesOff(uint8_t /*ch*/) override { panic(); }

    void panic() {
        for (uint8_t i = 0; i < _n; ++i) _eng[i]->panic();
        reset();
    }

private:
    struct Voice { bool on; uint8_t ch, note, eng; uint32_t age; };
    static constexpr uint8_t kMaxVoices = 32;   // >= max supported _n*_vpe

    AudioSynthDexed **_eng;
    uint8_t  _n, _vpe;
    bool     _mpe = false;
    Voice    _v[kMaxVoices];
    uint8_t  _load[16];     // active notes per engine
    int8_t   _chEng[17];    // MPE: member channel (1..16) -> engine, -1 = unmapped
    uint8_t  _rr = 0;       // normal-mode round-robin cursor
    uint32_t _seq = 0;      // monotonic age stamp for oldest-note stealing

    uint8_t maxVoices() const {
        uint16_t m = (uint16_t)_n * _vpe;
        return m > kMaxVoices ? kMaxVoices : (uint8_t)m;
    }

    void reset() {
        for (uint8_t i = 0; i < kMaxVoices; ++i) _v[i].on = false;
        for (uint8_t i = 0; i < 16; ++i) _load[i] = 0;
        for (uint8_t i = 0; i < 17; ++i) _chEng[i] = -1;
        _rr = 0; _seq = 0;
    }

    Voice *freeVoice() {
        for (uint8_t i = 0; i < maxVoices(); ++i) if (!_v[i].on) return &_v[i];
        return nullptr;
    }
    Voice *stealOldest() {
        Voice *o = &_v[0];
        for (uint8_t i = 1; i < maxVoices(); ++i) if (_v[i].age < o->age) o = &_v[i];
        return o;
    }
    void release(Voice *v) {
        _eng[v->eng]->keyup(v->note);
        if (_load[v->eng]) _load[v->eng]--;
        v->on = false;
        // If an engine just went idle, drop any MPE channel->engine mapping to it so
        // a later note on that channel re-allocates cleanly.
        if (_mpe && _load[v->eng] == 0)
            for (uint8_t c = 1; c <= 16; ++c) if (_chEng[c] == v->eng) _chEng[c] = -1;
    }

    uint8_t allocNormal() {
        for (uint8_t k = 0; k < _n; ++k) {
            uint8_t e = (uint8_t)((_rr + k) % _n);
            if (_load[e] < _vpe) { _rr = (uint8_t)((e + 1) % _n); return e; }
        }
        return stealOldest()->eng;   // all engines full -> reuse the oldest's engine
    }
    uint8_t allocMpe(uint8_t ch) {
        if (ch >= 1 && ch <= 16 && _chEng[ch] >= 0 && _load[_chEng[ch]] < _vpe)
            return (uint8_t)_chEng[ch];              // channel still sounding: keep its engine
        for (uint8_t e = 0; e < _n; ++e) if (_load[e] == 0) return e;   // a fully idle engine
        return stealOldest()->eng;                   // none free: steal oldest note's engine
    }

    template <class F> void forEachEngine(F f) { for (uint8_t i = 0; i < _n; ++i) f(_eng[i]); }
    template <class F> void forEachTarget(uint8_t ch, F f) {
        if (_mpe) { if (ch >= 1 && ch <= 16 && _chEng[ch] >= 0) f(_eng[_chEng[ch]]); }
        else      { forEachEngine(f); }
    }

    static uint8_t toMidi7(float v) {
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        return (uint8_t)(v * 127.0f + 0.5f);
    }
    static int16_t clampCounts(int c) {
        if (c < -8192) return -8192;
        if (c >  8191) return  8191;
        return (int16_t)c;
    }
};
