// TrackEngineImpl.inc.h — concrete ITrackEngine adapters + the g_trackEngine[] inventory.
//
// Step 1 of the "kill the primary" refactor (planning/multi-engine-tracks/DESIGN.md): each engine
// kind gets a thin adapter that FORWARDS to its existing functions — no DSP is reimplemented — and
// g_trackEngine[] binds one to every track using the SAME mapping the current code uses
// (voiceIsOpll / kDexedVoices). This is ADDITIVE: the array is populated at boot but nothing reads
// it yet, so every build stays behaviour-identical. main.cpp call sites migrate onto it next.
//
// Included by main.cpp AFTER all engine backends + g_tracks[] + g_drumTrack + the drum functions
// are defined (just before handleTrkCmd). Each adapter is gated by the flags that make its
// functions exist, so it compiles across every env.
#pragma once

// --- Dexed pool voice: per-voice instrument via synthSetInstrumentV (pool builds only) ---------
#if defined(TDSP_SYNTH_DEXED_POOL)
struct DexedVoiceEngineAdapter : ITrackEngine {
    int v = 0;
    tdsp::MidiSink*      sink() override           { return g_tracks[v].sink; }
    // trim() (the FX-send tap node) is config-branched (dxpTrim/dxpTrimB vs dxpVoiceTrim[]); the send
    // matrix still taps those nodes directly, so leave it null until that call site migrates.
    const char*         name() override            { return "Dexed"; }
    bool                isGM() override            { return synthIsGM(); }
    int                 numInstruments() override  { return synthNumInstruments(); }
    const char*         instrumentName(int i) override { return synthInstrumentName(i); }
    // Per-voice instrument state/dispatch mirrors handleTrkCmd's exact guards (only the 4-way pool
    // has the per-voice V arrays; 2-way uses the singleton + its "2" sibling; single is the singleton).
    int instrument() override {
#if TDSP_SYNTH_VOICES >= 4 && !TDSP_HETERO
        return g_synthInstrumentV[v];
#elif TDSP_VOICE2
        return (v == 1) ? g_synthInstrument2 : g_synthInstrument;
#else
        return g_synthInstrument;
#endif
    }
    void setInstrument(int i) override {
#if TDSP_SYNTH_VOICES >= 4 && !TDSP_HETERO
        synthSetInstrumentV(v, i);
#elif TDSP_VOICE2
        if (v == 1) synthSetInstrument2(i); else synthSetInstrument(i);
#else
        synthSetInstrument(i);
#endif
    }
    // Display name = a picked cart voice (4-way pool) if any, else the bundled instrument — matches
    // the exact expressions the @STATE tracks[] loop used per config.
    const char* currentName() override {
#if TDSP_SYNTH_VOICES >= 4 && !TDSP_HETERO
        return g_curCartNameV[v][0] ? g_curCartNameV[v] : synthInstrumentName(g_synthInstrumentV[v]);
#elif TDSP_VOICE2
        return synthInstrumentName((v == 1) ? g_synthInstrument2 : g_synthInstrument);
#else
        return synthInstrumentName(g_synthInstrument);
#endif
    }
    void setVol(int pct) override   { if (g_tracks[v].setLevel) g_tracks[v].setLevel(pct); }
    // synthSetMpeMode reconfigures the WHOLE Dexed pool (all windows), so only voice 0 fires it.
    void setMpeMode(bool mpe) override { if (v == 0) synthSetMpeMode(mpe); }
};
static DexedVoiceEngineAdapter g_dexedVoiceEngine[kDexedVoices];
#else
// --- Single primary voice: OPLL/OPL3/Plaits/SF2 as the one non-pool voice (track 0) -----------
struct PrimaryEngineAdapter : ITrackEngine {
    tdsp::MidiSink*     sink() override            { return g_synthSink; }
    const char*         name() override            { return synthName(); }
    bool                isGM() override            { return synthIsGM(); }
    int                 numInstruments() override  { return synthNumInstruments(); }
    const char*         instrumentName(int i) override { return synthInstrumentName(i); }
    int                 instrument() override      { return synthInstrument(); }
    void                setInstrument(int i) override  { synthSetInstrument(i); }
    void                setVol(int pct) override   { if (g_tracks[0].setLevel) g_tracks[0].setLevel(pct); }
    void                setMpeMode(bool mpe) override { synthSetMpeMode(mpe); }
    // @STATE tracks[].eng — the protocol tag for THIS primary backend, so the app labels track 0 by its
    // real engine (e.g. "opll" on a 0-Dexed OPLL-primary build) instead of falling back to Dexed. Dexed
    // stays "" (the app's default) so Dexed builds are unchanged.
    const char* engTag() override {
#if defined(TDSP_SYNTH_OPLL) || defined(TDSP_SYNTH_OPLL_POOL)
        return "opll";
#elif defined(TDSP_SYNTH_PLAITS)
        return "plaits";
#elif defined(TDSP_SYNTH_OPL3)
        return "opl3";
#elif defined(TDSP_SYNTH_YMFM)
        return "ymfm";
#elif defined(TDSP_SYNTH_RINGS)
        return "rings";
#elif defined(TDSP_SYNTH_VA)
        return "va";
#elif defined(TDSP_SYNTH_SF2) || defined(TDSP_SYNTH_SF2_TSF)
        return "sf2";
#else
        return "";   // single Dexed → "" (app defaults to Dexed; unchanged)
#endif
    }
};
static PrimaryEngineAdapter g_primaryEngine;
#endif

// --- Melodic OPLL hetero voice (HeteroOpll.h) --------------------------------------------------
// Gated on OPLL_ENGINES (not just HETERO): a hetero inventory may have zero hetero-OPLL voices.
#if TDSP_OPLL_ENGINES >= 1
struct OpllVoiceEngineAdapter : ITrackEngine {
    int eng = 0;
    tdsp::MidiSink*      sink() override           { return g_hoOpllVoiceSink[eng]; }
    AudioEffectGain_F32* trim() override           { return &g_hoOpllTrim[eng]; }
    const char*         name() override            { return "OPLL"; }
    int                 numInstruments() override  { return heteroOpllNumInstruments(); }
    const char*         instrumentName(int i) override { return heteroOpllInstrumentName(i); }
    int                 instrument() override      { return heteroOpllInstrument(eng); }
    void                setInstrument(int i) override  { heteroOpllSetInstrument(eng, i); }
    void                setVol(int pct) override   { heteroOpllSetVol(eng, pct); }
    const char*         engTag() override          { return "opll"; }   // @STATE tracks[].eng
};
static OpllVoiceEngineAdapter g_opllVoiceEngine[TDSP_OPLL_ENGINES];
#endif

// --- Melodic Plaits hetero track (HeteroPlaits.h) ----------------------------------------------
#if TDSP_HETERO_PLAITS
struct PlaitsVoiceEngineAdapter : ITrackEngine {
    tdsp::MidiSink*      sink() override           { return g_hpVoiceSink; }
    AudioEffectGain_F32* trim() override           { return &g_hpTrim; }
    const char*         name() override            { return "Plaits"; }
    int                 numInstruments() override  { return heteroPlaitsNumInstruments(); }
    const char*         instrumentName(int i) override { return heteroPlaitsInstrumentName(i); }
    int                 instrument() override      { return heteroPlaitsInstrument(); }
    void                setInstrument(int i) override  { heteroPlaitsSetInstrument(i); }
    void                setVol(int pct) override   { heteroPlaitsSetVol(pct); }
    const char*         engTag() override          { return "plaits"; }
    // MPE: master ch 1 = per-note member channels 2..16 (one Plaits voice per note); 0 = plain poly.
    void                setMpeMode(bool mpe) override { g_hpPlaitsSink.setMasterChannel(mpe ? 1 : 0); }
};
static PlaitsVoiceEngineAdapter g_plaitsVoiceEngine;   // one Plaits track (a pool behind one sink)
#endif

// --- Drum track: a track whose engine plays ch10 (kit = its "instrument") ----------------------
struct DrumTrackEngineAdapter : ITrackEngine {
    tdsp::MidiSink*     sink() override            { return g_drumTrack.sink; }
    const char*         name() override            { return (kDrumEngineName && kDrumEngineName[0]) ? kDrumEngineName : "Drums"; }
    int                 numInstruments() override  { return numDrumKits(); }
    const char*         instrumentName(int i) override { return drumKitName(i); }
    int                 instrument() override      { return g_drumKit; }
    void                setInstrument(int i) override  { setDrumKit(i); }
    const char*         currentName() override     { return g_curDrumName; }   // loaded groove/kit name (@STATE)
    void                setVol(int pct) override   { setDrumVol(pct); }
    bool                playsCh10() override       { return true; }
    bool                appliesKit() override      { return kDrumKitSelectable; }
};
static DrumTrackEngineAdapter g_drumTrackEngineImpl;

// --- The inventory: one ITrackEngine per synth track, + the drum track -------------------------
static ITrackEngine *g_trackEngine[kNumTracks];
static ITrackEngine *g_drumTrackEngine = &g_drumTrackEngineImpl;

// Bind each track to its engine, using the CURRENT mapping (kept identical during migration).
static void trackEnginesInit() {
    for (int v = 0; v < kNumTracks; v++) {
#if TDSP_HETERO_PLAITS
        if (voiceIsPlaits(v)) { g_trackEngine[v] = &g_plaitsVoiceEngine; continue; }
#endif
#if TDSP_OPLL_ENGINES >= 1
        if (voiceIsOpll(v)) {
            const int e = v - kDexedVoices;
            g_opllVoiceEngine[e].eng = e;
            g_trackEngine[v] = &g_opllVoiceEngine[e];
            continue;
        }
#endif
#if defined(TDSP_SYNTH_DEXED_POOL)
        g_dexedVoiceEngine[v].v = v;
        g_trackEngine[v] = &g_dexedVoiceEngine[v];
#else
        g_trackEngine[v] = &g_primaryEngine;   // single-primary build (kNumTracks == 1)
#endif
    }
}

// Toggle MPE on every synth track's engine uniformly (called from applyMidiMode). Each engine maps
// it to its own sink via the vtable, so Dexed / OPLL / Plaits / any future engine all switch together.
static void trackEnginesSetMpe(bool mpe) {
    for (int v = 0; v < kNumTracks; v++) g_trackEngine[v]->setMpeMode(mpe);
}
