// TrackEngine.h — the uniform per-track engine surface (the "kill the primary" refactor).
//
// Historically main.cpp drove ONE privileged synth via a singleton contract of free functions
// (synthSetInstrument(), synthName(), g_synthSink, …) and treated OPLL/drum voices as bolt-ons
// dispatched by hand (the `voiceIsOpll(i) ? heteroOpll…() : synth…()` branches). That makes track 0
// special and hardwires "Dexed is the primary". ITrackEngine removes the privilege: EVERY track —
// including track 0 — is just a Track bound to an ITrackEngine, and main.cpp drives
// g_trackEngine[i]->… uniformly. A build with no Dexed at all becomes an inventory choice, not a
// code change. See planning/multi-engine-tracks/DESIGN.md.
//
// The concrete engines (Dexed pool window, HeteroOpll, HeteroPlaits, OPL3, the drum voices) each
// provide an adapter subclass that forwards to their existing functions — so this layer is added
// WITHOUT reimplementing any DSP. Migration is incremental: adapters + g_trackEngine[] land first
// (no behaviour change), then main.cpp call sites move onto the vtable one at a time.
#pragma once
#include <stdint.h>

namespace tdsp { class MidiSink; }
class AudioEffectGain_F32;

// One track's engine, uniform across kinds. Instrument semantics are engine-defined (a Dexed voice,
// an OPLL ROM voice, a Plaits model, a GM drum kit) but the app drives them all through the same
// @TRK<i>.INSTR / @STATE tracks[].eng path.
struct ITrackEngine {
    virtual ~ITrackEngine() {}

    // --- audio/MIDI binding -------------------------------------------------
    virtual tdsp::MidiSink*      sink()  = 0;    // note/bend/pressure/program in (== Track.sink)
    virtual AudioEffectGain_F32* trim()  { return nullptr; }  // per-track level + FX-send tap node (null = fixed-slot engine)

    // --- identity -----------------------------------------------------------
    virtual const char* name()  = 0;             // engine kind for @STATE tracks[].eng ("Dexed"/"OPLL"/"Plaits"/…)
    virtual bool        isGM()  { return false; }// GM song/drum routing

    // --- instrument (engine-defined voice list) -----------------------------
    virtual int         numInstruments()        = 0;
    virtual const char* instrumentName(int i)   = 0;
    virtual int         instrument()            = 0;
    virtual void        setInstrument(int i)    = 0;
    // The current voice's DISPLAY name for @STATE (a Dexed cart pick, an OPLL ROM voice, a drum kit).
    // Defaults to the plain instrument name; Dexed overrides it to surface a picked cart voice.
    virtual const char* currentName() { return instrumentName(instrument()); }
    // Lowercase protocol tag for @STATE tracks[].eng ("opll"/"plaits"/…). "" = the app's inferred
    // default (kept empty for Dexed so the existing wire format is byte-identical during migration).
    virtual const char* engTag() { return ""; }
    // The loaded /dexed CART for THIS track (a @DXPICK pick), so @STATE tracks[] can surface each
    // track's cart+voice — not just voice 0/1 via the top-level voice/voice2 objects — letting the app
    // list/step every synth voice's library position (esp. Synth C/D). Sets *rel to the cart path and
    // *cv to the 0-based voice index and returns true; false for engines with no cart concept
    // (OPLL/Plaits/…) or when a bundled/ROM voice is current (not a cart pick). Default: no cart.
    virtual bool currentCart(const char** rel, int* cv) { (void)rel; (void)cv; return false; }

    // --- level --------------------------------------------------------------
    virtual void setVol(int pct) = 0;            // 0..150 %

    // --- MPE (per-note bend / pressure / timbre) ----------------------------
    // applyMidiMode() toggles the whole box between normal MIDI and MPE; each engine maps that to its
    // own sink (Dexed pool -> setMpeMode; Plaits -> setMasterChannel; …). Default no-op for engines
    // with no per-note-expression path — so a new engine opts in by overriding, "once and for all".
    virtual void setMpeMode(bool mpe) { (void)mpe; }

    // --- drum-track extras (a drum engine is just a track with these true) ---
    virtual bool playsCh10()  { return false; }  // groove/ch10 feeds this engine
    virtual bool appliesKit() { return false; }  // setInstrument == a GM kit program change

    // begin() is intentionally NOT here: engines with pools/shared slots bring the whole group up
    // once (heteroOpllBegin/heteroPlaitsBegin/synthBegin) — kept as-is during migration.
};
