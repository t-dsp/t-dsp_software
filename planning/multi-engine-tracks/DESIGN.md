# Generalized per-track synth engines — design

**Goal.** Any synth engine (Dexed, OPLL, OPL3, Plaits, … + drum engines OPLL-rhythm,
OPL3-drums, Plaits-drums) can be bound to any Track, chosen by a **build-time inventory**.
Drums stop being a special one-off slot — a drum engine is just a Track whose engine plays
ch10. This lets us assemble mixed-engine builds ("OPLL + OPL3 + Plaits + 3 drum kinds") and,
critically, **audition engines to pick favourites**, then ship curated inventories.

Confirmed core to the project (user, 2026-07). Supersedes the hardcoded "Dexed pool + OPLL
hetero + 1 drum voice" shape.

## What already exists (build on, don't reinvent)
- **`tdsp::MidiSink`** — the common engine interface (noteOn/off, bend, pressure, program).
  Every backend already has one: `DexedPoolSink`, `OpllSink`, OPL3 sink, `Plaits2Sink`, the
  drum sinks. **This is the abstraction the inventory binds to.**
- **`HeteroOpll.h` wiring pattern** — the template for a non-primary engine:
  `engine → AudioConvert_I16toF32 → AudioEffectGain_F32 (per-voice Trim) → AudioMixer4_F32
  (sub-mix) → one output mix slot`, plus a `MidiSink*` per voice that binds `track k`.
  Generalizing THIS to N engine *kinds* is the whole job.
- **`Track` struct** — one voice's full stack (player + arp + router + follow + looper + sink +
  caps + level). Already engine-agnostic; it just needs its `sink`/level node to come from the
  inventory instead of hardcoded Dexed/OPLL.
- **`tracks-phase3-voices` branch** (project_tracks_refactor) — unified Voice 1/2 + drums into N
  Track slots; drums = just-another-track. This is the same direction — reconcile/land it.
- **`TDSP_POOL_VOICES`** (project_hetero_n_opll) — already decouples pool engine count from track
  count. The inventory extends this idea to per-track engine *kind*.
- **`@STATE tracks[].eng`** — the app already reads a per-track engine tag ("opll"; absent =
  Dexed) and the FX-send matrix + componentized synth cards already render per-track from it.

## The hard constraint: static audio graph + RAM
- `AudioConnection[_F32]` objects are **statically constructed** — you can't array-build the
  graph at runtime. So the inventory is expressed at **compile time** (conditional include
  blocks / X-macro per track), exactly like HeteroOpll's explicit per-engine connections.
- **RAM is the real ceiling**, not flash (7.75 MB free). Every engine in the inventory is
  resident. Measured today: standalone `teensy41_opl3` overflows RAM1 by ~56 KB and
  `teensy41_plaits` also overflows — both because the default single-engine builds still carry
  **S/PDIF-in** (its async resampler filter is ~87 KB DTCM). Multi-engine builds MUST set
  `-D TDSP_NO_SPDIF_IN=1` and lean on `TDSP_LEAN_RAM`. Rough per-engine working RAM: OPLL ~9 KB,
  Plaits ~16 KB/voice, OPL3 ~40 KB (chip+resampler), Dexed ~30 KB/engine. Two OPL3 resamplers is
  the pain point — one OPL3 chip can serve melodic + ch10 drums to save one.
- **PSRAM board** fits more; the no-PSRAM board needs trimmed voice counts + curated inventories.

## THE core refactor: kill the "primary engine" (decouple Dexed)
The blocker isn't wiring — it's that the code has a **privileged singleton engine**. Exactly one
`SynthBackend<X>.h` is included and it exports a fixed contract of free functions/globals that the
rest of main.cpp calls directly:
`g_synthSink`, `synthBegin()`, `synthSetInstrument()`, `synthInstrument()`, `synthNumInstruments()`,
`synthInstrumentName()`, `synthName()`, `synthIsGM()`, the ReplayGain hooks, etc. Track 0 + the pool
voices ARE this primary; `HeteroOpll`/`DrumVoice` are second-class bolt-ons that assume the primary
is the Dexed pool (`kDexedVoices`). **Generalizing = deleting the notion of a primary.** Every track,
including track 0, must be *just a track with an engine* — symmetric, no special case.

### `ITrackEngine` — the uniform per-track surface
Replace the singleton `synth*()` contract with a per-track vtable each engine kind implements
(HeteroOpll/HeteroPlaits already have every method — this just formalizes them):
```
struct ITrackEngine {
  tdsp::MidiSink*      sink;                 // note/bend/pressure/program in
  AudioEffectGain_F32* trim;                 // per-track level + the FX-send tap node
  const char*  name();                       // "Dexed" / "OPLL" / "Plaits" / …
  bool         isGM();                       // GM song routing / drum handling
  int          numInstruments();
  const char*  instrumentName(int i);
  int          instrument();
  void         setInstrument(int i);
  void         setVol(int pct);
  void         begin();
  // drum-track extras (a drum engine is just a track with these true):
  bool         playsCh10();                  // groove/ch10 feeds this engine
  bool         appliesKit();                 // setInstrument == GM kit program change
};
```
The **inventory** becomes `ITrackEngine* g_trackEngine[kNumTracks]`, assembled at boot from whatever
engine instances the build compiled — no privileged slot 0. main.cpp stops calling `synth*()` and
instead drives `g_trackEngine[i]->…`. `@STATE tracks[i].eng = g_trackEngine[i]->name()`, `@TRK<i>.INSTR`
→ `g_trackEngine[i]->setInstrument()`, etc. — the per-track dispatch the app already expects.

### Incremental migration (keep the build green at every step)
1. Define `ITrackEngine` + a tiny adapter that wraps the CURRENT primary (`synth*()` funcs) as
   `g_trackEngine[0]` — behaviour identical, nothing removed yet. Wrap HeteroOpll voices as
   `g_trackEngine[kDexedVoices+k]`, drum voice as the drum track. Build stays byte-equivalent.
2. Migrate main.cpp call sites (`handleTrkCmd`, `@STATE tracks[]`, `songPrep`, setup `begin()`)
   from `synth*()`/`heteroOpll*()`/`drum*()` to `g_trackEngine[i]->…` one at a time, green-building
   `teensy41_opll` + a hetero env after each.
3. Once every call site goes through the vtable, the `SynthBackend<X>.h` "primary" is just *one more*
   `ITrackEngine` provider — Dexed loses its privilege. A build with **no Dexed at all** (e.g.
   OPLL + Plaits) is then only an inventory choice, not a code change.
4. Fold `HeteroOpll`/`HeteroPlaits`/`DrumVoice`/OPL3 into uniform per-kind "track-engine provider"
   headers that each contribute their instances to `g_trackEngine[]` via a registration macro.

Only after step 3 does "remove Dexed from the mix" become trivial — which is the whole point.

## Target architecture
```
inventory (build-time list of {engine-kind, voices}) — e.g. opll, opl3, plaits, opll_drum, plaits_drum
   └─ per track T:
        engineT (backend instance) ── [I16toF32 if int16] ── trimT (AudioEffectGain_F32) ──┐
        sinkT (tdsp::MidiSink) ◄── track T's player/arp/router/live-MIDI                    │
                                                                          busL/busR (mixer tree) ── FX send taps ── post ── DAC
```
- Each track owns a `trim` node (its volume + the FX-send tap point — the send matrix already
  taps `dxpTrim`/`g_hoOpllTrim[k]`; new engines expose the same `trim` node → sends work for free).
- Drum-engine tracks are identical except the player feeds ch10 and `caps.appliesKit`/note-map
  apply. OPLL-rhythm / OPL3-drums / Plaits-drums are just three more engine kinds.

## Phases
1. **Engine adapter normalization.** Give every backend a uniform "track engine" surface: a
   `{ MidiSink* sink; AudioStream/F32 trimNode; const char* name; instrument API }` — so the
   inventory can wire any of them the HeteroOpll way. Mostly wrapping what exists.
2. **Inventory mechanism (2 engines first).** A compile-time way to declare N tracks of mixed
   kinds + generate the graph. Prove it with **OPLL + Plaits** (both cheap, no-PSRAM) as two
   melodic tracks in one build. Green-build `teensy41_opll` canary + the new env; measure RAM.
3. **Drums as engine tracks.** Add OPLL-rhythm, OPL3-drums (shared OPL3 chip, ch10), Plaits-drums
   (from spike) as inventory engine kinds. Land/reconcile `tracks-phase3-voices`.
4. **Protocol.** `@STATE tracks[].eng` for every kind ("opll"/"opl3"/"plaits"/…); per-track
   instrument namespace (each engine's own voice list) via the existing `@TRK<i>.INSTR`.
5. **App — componentized per-track card.** One reusable card+detail (the earlier ask), rendered
   per track from `tracks[]`, engine-agnostic: engine label, instrument picker, volume, arp, FX
   send, player. Synth and drum tracks share it.
6. **RAM tooling + curated presets.** A per-inventory RAM report; ship a few build presets that
   are known to fit (no-PSRAM lean set vs PSRAM full set). Document what coexists.

## First concrete target
A **no-PSRAM, no-Dexed** audition build: **OPLL + OPL3 + Plaits** melodic tracks (+ one drum
engine), `TDSP_NO_SPDIF_IN`, trimmed voice counts — the smallest build that proves the
generalized wiring AND lets the user compare the three FM/wavetable engines side by side. Expand
to the full 6-engine inventory once it fits / on PSRAM.

## Open items / risks
- `AudioConnection` static-graph explosion → keep per-engine wiring in per-kind include blocks
  (HeteroOpll style), inventory selects which compile.
- ITCM boundary cliff (reference_itcm_boundary_cliff) — watch stack when engines stack up.
- Fix the `spike_plaits_drums` crash-loop before promoting Plaits-drums (it built + fit but
  reboot-looped on HW — likely the ESP32-kit boot path or an audio-init issue; diagnose first).
