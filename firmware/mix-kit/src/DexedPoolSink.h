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

    // Restrict this sink to its first `n` engines. Used to SPLIT a shared engine array
    // into two independent windows (Voices 2): the main sink shrinks to engines 0..3
    // while a second sink over &engines[4] drives 4..7. Silences everything first so no
    // note is left sounding on an engine that just dropped out of the window.
    void setEngineCount(uint8_t n) { if (n < 1) n = 1; if (n > 16) n = 16; panic(); _n = n; }
    uint8_t engineCount() const { return _n; }

    // --- MidiSink overrides -------------------------------------------------
    void onNoteOn(uint8_t ch, uint8_t note, uint8_t vel) override {
        if (vel == 0) { onNoteOff(ch, note, 0); return; }
        uint8_t e = _mpe ? allocMpe(ch) : allocNormal();
        // MPE: a recycled engine still holds the PREVIOUS note's latched pitch bend (controllers don't
        // reliably recenter bend on a fresh finger-press), so a new note on that engine would sound
        // pre-bent until the next bend message arrives — audible as "the note right after a drag lands
        // on the wrong pitch, then corrects after a few presses." Recenter the engine at note-on.
        // Skip it ONLY when stacking onto the same still-sounding MPE channel (its live bend applies to
        // both notes); normal (broadcast-bend) mode is left alone so a new note inherits the wheel.
        const bool recenter = _mpe && !(ch >= 1 && ch <= 16 && (int8_t)e == _chEng[ch] && _load[e] > 0);
        Voice *v = freeVoice();
        if (!v) { v = stealOldest(); release(v); }   // table full: reclaim the oldest slot
        v->on = true; v->ch = ch; v->note = note; v->eng = e; v->age = ++_seq;
        _load[e]++;
        if (_mpe && ch >= 1 && ch <= 16) _chEng[ch] = (int8_t)e;   // set AFTER any steal
        if (ch <= 16) { _chPress[ch] = 1.0f; _chTimbre[ch] = 1.0f; }  // fresh note = full until expression arrives
        if (recenter) { _eng[e]->setPitchbendRange((uint8_t)kBendRange); _eng[e]->setPitchbend((int16_t)0); }  // drop the prior note's bend
        _eng[e]->setGain(kEngineGain);   // reset: don't inherit a prior note's expression gain
        _eng[e]->keydown(note, vel);
    }

    void onNoteOff(uint8_t ch, uint8_t note, uint8_t) override {
        for (uint8_t i = 0; i < maxVoices(); ++i)
            if (_v[i].on && _v[i].ch == ch && _v[i].note == note) { release(&_v[i]); return; }
    }

    // Bend sensitivity. The router / player already applied the per-channel range and
    // hands us the final semitone value, so we set Dexed's own sensitivity to a FIXED
    // kBendRange and map the semitones into +/-8192 counts. kBendRange must be >= the
    // widest bend we want to reproduce. The LinnStrument's default per-note bend range is
    // +-24 semis, so use 24 to reproduce a full two-octave slide. (Was 1, which clamped to
    // +-1 semi; then 12, which clamped slides wider than an octave.)
    //
    // IMPORTANT: stock synth_dexed HARD-CAPS the range at 12 (constrain(range,0,12) in
    // dexed.cpp::setPitchbendRange), so this 24 only takes effect because the build-time
    // patch tools/dexed_bend_range.py rewrites that cap to 24 in the fetched lib_dep. Drop
    // that script and Dexed silently clamps back to +-12 (one octave). Verified on HW by the
    // @BOARDTEST self-test (BoardTest.inc.h / tools/board_bend_test.py).
#ifndef TDSP_MPE_BEND_RANGE
#define TDSP_MPE_BEND_RANGE 24
#endif
    static constexpr int kBendRange = TDSP_MPE_BEND_RANGE;   // must match applyMidiMode()'s router range; synth_dexed's cap is patched to >= this by tools/dexed_bend_range.py
    void onPitchBend(uint8_t ch, float semitones) override {
        const int16_t counts = clampCounts((int)(semitones / (float)kBendRange * 8192.0f));
        forEachTarget(ch, [&](AudioSynthDexed *e) { e->setPitchbendRange((uint8_t)kBendRange); e->setPitchbend(counts); });
    }
    // ---- Expression routing (Mod Wheel + Pressure) -------------------------
    // Two performance SOURCES — the mod wheel (CC1, on every keyboard) and channel
    // pressure (mono aftertouch on a normal keyboard / per-note MPE-Z) — each route to any
    // combination of DESTINATIONS. Mod wheel drives the Dexed controller targets; pressure
    // drives them plus a per-note VOLUME swell. VOL applies to pressure only (mod wheel is
    // for the LFO/EG effects). kLfoForce vs respect: see applyExprConfig.
    enum {
        DEST_VOL    = 1,   // per-note output gain — VOLUME swell (pressure only), ANY patch
        DEST_BRIGHT = 2,   // Dexed controller -> EG bias — brightness/timbre, ANY patch
        DEST_VIB    = 4,   // Dexed controller -> PITCH (LFO vibrato)
        DEST_TREM   = 8,   // Dexed controller -> AMP   (LFO tremolo)
    };
    static constexpr float kEngineGain = 0.8f;   // must match synthBegin()'s setGain
    static constexpr float kPressFloor = 0.22f;  // VOL: 0 pressure -> 22% gain; full -> 100%

    void     setPressureMask(uint8_t m) { _pressMask = m; applyExprConfig(); }
    void     setModMask(uint8_t m)      { _modMask = (uint8_t)(m & ~DEST_VOL); applyExprConfig(); }
    void     setTimbreMask(uint8_t m)   { _timbreMask = m; applyExprConfig(); }   // timbre may drive VOLUME too
    void     setLfoForce(bool on)       { _lfoForce = on; applyExprConfig(); }
    uint8_t  pressureMask() const       { return _pressMask; }
    uint8_t  modMask() const            { return _modMask; }
    uint8_t  timbreMask() const         { return _timbreMask; }
    bool     lfoForce() const           { return _lfoForce; }

    static uint8_t dexedTarget(uint8_t mask) {   // dest bits -> Dexed target bitmask (1=pitch 2=amp 4=eg)
        return (uint8_t)(((mask & DEST_VIB) ? 1 : 0) | ((mask & DEST_TREM) ? 2 : 0) | ((mask & DEST_BRIGHT) ? 4 : 0));
    }

    // (Re)program each engine's mod-wheel + aftertouch targets from the masks. In FORCE
    // mode, if either source routes to vibrato/tremolo we also force the LFO (speed +
    // pitch sensitivity) so it works on ANY patch; in RESPECT mode we leave the patch's
    // own LFO alone (only natively-LFO patches — the [V]/[T]-tagged ones — respond). Call
    // after every voice load (loadVoice resets controller/LFO state).
    void applyExprConfig() {
        // Three sources -> three Dexed controllers: mod wheel, aftertouch, breath (= CC74
        // timbre, the MPE Y-axis). Each routes to pitch/amp/eg per its mask.
        const uint8_t all = _modMask | _pressMask | _timbreMask;
        for (uint8_t i = 0; i < _n; ++i) {
            _eng[i]->setAftertouchRange(99);      _eng[i]->setAftertouchTarget(dexedTarget(_pressMask));
            _eng[i]->setModWheelRange(99);        _eng[i]->setModWheelTarget(dexedTarget(_modMask));
            _eng[i]->setBreathControllerRange(99); _eng[i]->setBreathControllerTarget(dexedTarget(_timbreMask));
            if (_lfoForce && (all & (DEST_VIB | DEST_TREM))) {
                _eng[i]->setLFOSpeed(30);
                _eng[i]->setLFOWaveform(0);
                if (all & DEST_VIB) _eng[i]->setLFOPitchModulationSensitivity(7);
            }
        }
    }

    // Per-note output gain, combining every VOLUME-routed source (pressure and/or timbre)
    // multiplicatively so they coexist without fighting. Each contributes kPressFloor..1.
    float combinedGain(uint8_t ch) const {
        const float pv = (ch <= 16) ? _chPress[ch]  : 1.0f;
        const float tv = (ch <= 16) ? _chTimbre[ch] : 1.0f;
        const float pf = (_pressMask  & DEST_VOL) ? kPressFloor + (1.0f - kPressFloor) * pv : 1.0f;
        const float tf = (_timbreMask & DEST_VOL) ? kPressFloor + (1.0f - kPressFloor) * tv : 1.0f;
        return kEngineGain * pf * tf;
    }
    void onPressure(uint8_t ch, float value) override {
        if (ch <= 16) _chPress[ch] = value;
        const uint8_t v = toMidi7(value);
        const float g = combinedGain(ch);
        forEachTarget(ch, [&](AudioSynthDexed *e) { e->setAftertouch(v); e->setGain(g); });
    }
    void onModWheel(uint8_t /*ch*/, float value) override {
        const uint8_t v = toMidi7(value);
        forEachEngine([&](AudioSynthDexed *e) { e->setModWheel(v); });   // target set in applyExprConfig
    }
    // MPE Y-axis: CC74 timbre. Per-note (forEachTarget) — Dexed BREATH controller for
    // brightness, and (default) a per-note VOLUME swell so the slide is clearly audible.
    void onTimbre(uint8_t ch, float value) override {
        if (ch <= 16) _chTimbre[ch] = value;
        const uint8_t v = toMidi7(value);
        const float g = combinedGain(ch);
        forEachTarget(ch, [&](AudioSynthDexed *e) { e->setBreathController(v); e->setGain(g); });
    }
    void onSustain(uint8_t /*ch*/, bool on) override {
        forEachEngine([&](AudioSynthDexed *e) { e->setSustain(on); });
    }
    void onAllNotesOff(uint8_t /*ch*/) override { panic(); }

    void panic() {
        for (uint8_t i = 0; i < _n; ++i) {
            _eng[i]->panic();
            // Clear stale per-engine expression too — panic() alone kills the notes but
            // leaves the last bend / controller values latched, so the next note (or the
            // first note of the next song) would start pre-bent or pre-swelled.
            _eng[i]->setPitchbend((int16_t)0);      // recenter (0 counts = no bend)
            _eng[i]->setModWheel(0);
            _eng[i]->setAftertouch(0);
            _eng[i]->setBreathController(0);
            _eng[i]->setGain(kEngineGain);          // drop any swelled VOLUME gain
        }
        for (uint8_t i = 0; i < 17; ++i) { _chPress[i] = 1.0f; _chTimbre[i] = 1.0f; }
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
    uint8_t  _pressMask  = DEST_VOL | DEST_BRIGHT;   // pressure routing (default: volume + brightness)
    uint8_t  _modMask    = DEST_VIB;                 // mod-wheel routing (default: vibrato)
    uint8_t  _timbreMask = DEST_VOL | DEST_BRIGHT;   // CC74 timbre routing (default: volume + brightness = punchy)
    bool     _lfoForce   = true;                     // force LFO so vib/trem work on any patch
    float    _chPress[17]  = {0};                    // per-channel latest pressure 0..1 (for combined VOL gain)
    float    _chTimbre[17] = {0};                    // per-channel latest CC74 timbre 0..1

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
