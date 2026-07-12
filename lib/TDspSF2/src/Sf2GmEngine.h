// Sf2GmEngine.h — General-MIDI sample-playback engine on top of AudioSynthWavetable
// (Teensy Audio Library voice) + manicken/sf22aswt (runtime SF2 loader, PSRAM-resident).
//
//   GM MIDI (note/program/pitchbend/drum)
//     -> Sf2GmEngine: per-channel program map + voice allocator
//        + program/bank -> SF2-instrument resolver (reads phdr/pbag/pgen)
//        + a pool of ReaderLazy "cache slots" (one resident SF2 instrument each,
//          samples spilled to PSRAM by sf22aswt, adaptive to external_psram_size)
//     -> N AudioSynthWavetable voices (this class only DRIVES them; the audio graph
//        wiring — voices -> mixer tree -> int16 -> F32 mix slot — lives in the backend).
//
// This is the SF2 analogue of AudioSynthYmfmOPL3: a self-contained GM synth. Unlike
// OPL3 (one chip renders its own 18 channels), here the *voices are external* Teensy
// AudioStream objects owned by the backend, because that is how the Teensy audio graph
// is built (global streams + global AudioConnections). attachVoices() hands us the pool.
//
// Design notes / rationale live in lib/TDspSF2/SF2_GM_ENGINE_PLAN.md.
#pragma once

#include <Arduino.h>
#include <Audio.h>              // AudioSynthWavetable
#include "sf22aswt/sf22aswt.h"  // SF22ASWTreader (ReaderLazy), converter

class Sf2GmEngine {
public:
    static constexpr int kMaxVoices = 32;   // hard cap on the pool the backend may attach
    static constexpr int kMaxSlots  = 32;   // hard cap on resident-instrument cache slots
    static constexpr int kDrumChannel = 10; // GM: channel 10 (1-based) is percussion
    static constexpr int kDrumBank    = 128;
    static constexpr int kDrumProg    = 128; // note-map row index for the drum kit (after 0..127)

    Sf2GmEngine() = default;

    // Hand the engine the backend-owned voice pool (already in the audio graph).
    // Call BEFORE begin(). `count` is clamped to kMaxVoices.
    void attachVoices(AudioSynthWavetable* voices, int count);

    // Load the SF2 off SD, resolve the GM program/bank -> instrument map, size the
    // resident cache from external_psram_size, and preload a default melodic patch
    // (GM 0, Acoustic Grand) + the drum kit so the first note never waits. Returns
    // false if the file can't be read (engine then plays nothing, but never crashes).
    bool begin(const char* sf2Path);
    bool isReady() const { return m_ready; }

    // ---- GM MIDI (call from loop / MIDI handlers, NOT the audio ISR) -----------
    // channel is 1..16 (GM). Channel 10 = drums: `note` picks the percussion sample.
    void noteOn (uint8_t channel, uint8_t note, uint8_t velocity);
    void noteOff(uint8_t channel, uint8_t note);
    void programChange(uint8_t channel, uint8_t program);
    void pitchBend(uint8_t channel, float semitones);        // per-channel, pre-scaled
    void controlChange(uint8_t channel, uint8_t cc, uint8_t v);
    void allNotesOff();

    // Global output level, applied as each voice's amplitude (0..1). SF2 samples are
    // full-scale, so unlike OPL3 this is a <=1.0 attenuator, not a boost.
    void setGain(float g);

    int  activeVoices() const;

    // ---- catalog (app instrument picker) --------------------------------------
    int         numMelodic() const { return 128; }
    const char *melodicName(int gm) const;

private:
    // --- voice pool (owned by the backend, driven here) ---
    AudioSynthWavetable* m_voice = nullptr;
    int      m_numVoices = 0;
    int8_t   m_vNote[kMaxVoices];       // MIDI note a voice is sounding, -1 = free
    uint8_t  m_vChan[kMaxVoices];       // owning MIDI channel (1..16)
    uint32_t m_vAge[kMaxVoices];        // note-on order, for oldest-voice stealing
    float    m_vBaseNote[kMaxVoices];   // played note (for live pitch-bend retune)
    int16_t  m_vScale[kMaxVoices];      // SF2 scaleTuning % of the sounding instrument (100=normal)
    int16_t  m_vRoot[kMaxVoices];       // its root key, for scaleTuning-correct retune on bend
    const AudioSynthWavetable::instrument_data* m_vInst[kMaxVoices]; // for cache pinning
    uint32_t m_ageCounter = 0;

    // --- per-MIDI-channel state (index 1..16) ---
    uint8_t  m_program[17];             // current GM program per channel
    float    m_bend[17];                // pitch bend, semitones

    // --- SF2 loading / resident cache ---
    struct Slot {
        SF22ASWTreader reader;          // clone of the master (holds file layout)
        int  instIndex = -1;            // SF2 instrument resident here, -1 = empty
        AudioSynthWavetable::instrument_data* inst = nullptr;
        uint32_t lastUsed = 0;
        bool cloned = false;
    };
    Slot     m_slot[kMaxSlots];
    int      m_numSlots = 0;
    uint32_t m_useCounter = 0;

    SF22ASWTreader m_master;            // holds the parsed SF2 (ReadFile once)
    bool     m_ready = false;
    float    m_gain  = 0.9f;

    // Per-(program, note) -> SF2 instrument index, PSRAM-resident (129*128 int16).
    // Rows 0..127 = GM melodic programs (bank 0); row 128 = drum kit (bank 128).
    // Built at begin() by proper SF2 zone matching (preset keyRange, then each
    // instrument's own key coverage) so key/velocity-split presets and layered GM
    // drum kits resolve correctly. -1 = no instrument sounds that note.
    int16_t *m_noteInst = nullptr;

    // Per-SF2-instrument pitch traits (PSRAM, sized to the font's instrument count),
    // used to apply scaleTuning that AudioSynthWavetable doesn't handle itself.
    int16_t *m_instScale = nullptr;     // scaleTuning %, 100 = normal key tracking
    int16_t *m_instRoot  = nullptr;     // representative root key (for the scale pivot)
    int      m_instCount = 0;

    // resolve which SF2 instrument a channel+note should sound right now
    int  resolveInstrument(uint8_t channel, uint8_t note) const;
    bool isDrumInstrument(int idx) const;   // referenced by the drum kit? (-> force one-shot)
    // ensure the instrument is resident (load/evict), return its data (or nullptr)
    AudioSynthWavetable::instrument_data* ensureInstrument(int instIndex);
    bool instrumentInUse(int instIndex) const;   // any live voice pinning it?
    int  pickVoice();                             // free voice, else oldest
    bool buildPresetMap(const char* sf2Path);     // read phdr/pbag/pgen -> maps
};
