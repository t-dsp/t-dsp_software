# ReplayGain — per-voice / per-program loudness normalization for the mix-kit synths

This is the *definition* of the mix-kit's ReplayGain system: what it does, the two
normalization tiers, the shared building blocks, the interface every synth backend
implements, and how to (re)generate a backend's baked table on-device.

Goal: **switching instruments — and playing a multitimbral GM song — should not produce
big perceived-loudness jumps.** A bright FM brass patch can be 15–20 dB hotter than a
soft pad; General MIDI programs vary just as widely. There is no field you can read to
predict a patch's loudness — you have to *render it and measure*. We measure once,
per-device, and bake a trim table into firmware.

Loudness is measured **K-weighted (ITU-R BS.1770)** — a high-shelf + RLB high-pass so the
RMS tracks *perceived* loudness (the ear is ~+4 dB hotter at 2–5 kHz), not flat energy.
Bright/percussive patches no longer read "quiet" and then blast when you switch to them.


## Two normalization tiers

A synth is used two ways, and they need different mechanisms:

| Tier | When | Mechanism | Keyed by |
|------|------|-----------|----------|
| **1. Audition** | App picker selects one instrument → forced on *all* channels; also MPE (single-timbre) | One **bus trim** (`AudioEffectGain_F32`) on the synth's summed output | the picker instrument index |
| **2. Song norm** | A GM song plays → each channel runs its *own* program (multitimbral) | **Per-channel gain**, re-applied on every Program Change | the channel's GM program (0–127) |

Tier 1 is what the single-timbre Dexed pool has always done, and it's trivially correct
there (the whole pool plays the one selected voice, so one bus trim normalizes exactly
what's sounding). Tier 2 only matters for the **multitimbral** backends (OPLL, OPL3, OPM,
SF2/TSF), where a single bus trim keyed to the picker index is *meaningless mid-song* —
the picker index isn't even what's playing.

### Tier-2 mechanism differs by backend (honest limits)

Per-channel gain is applied *inside the engine/sink*, and the available lever varies:

- **SF2 / TSF** — clean. `tsf_channel_set_volume(g_tsf, ch, trim × userVol)` is a float
  per-channel gain. Full-resolution Tier-2.
- **OPLL** — coarse. The only per-channel lever is the **4-bit volume nibble** (16 steps,
  ~3 dB each). A per-program trim is quantized into nibble attenuation offsets in
  `voiceVol()`. Honest Tier-2 to ~3 dB granularity; better than nothing, not transparent.
- **OPL3 / OPM** — per-channel/per-operator TL registers exist; Tier-2 folds a per-program
  attenuation into the carrier TL. (Rollout pending — same shape as OPLL.)
- **Dexed pool** — single-timbre, so **Tier 2 does not apply**; Tier 1 fully covers it.


## Shared building blocks (`src/ReplayGain.h`)

- **`LoudnessProbe_F32`** — a reusable `AudioStream_F32` that taps a backend's *raw*
  synth sum (pre-trim, pre-mix-makeup) and accumulates K-weighted RMS + raw peak. This is
  the measurement instrument the sweep reads. Every backend instantiates one and connects
  it to its raw sum. (The Dexed pool's richer `ClipProbe_F32` — which also does click /
  onset-capture diagnostics — implements the same `ILoudnessMeter` interface instead of
  duplicating the meter.)
- **`ILoudnessMeter`** — the tiny interface the generic sweep drives:
  `reset()`, `resetRms()`, `rms()`, `peak()`. Both probes implement it.
- **`applyGmSongTrim(...)`** — helper describing the Tier-2 contract; each sink calls its
  engine's per-channel gain with `gmProgramTrim(prog)`.


## Backend interface (each `SynthBackend*.h` provides)

A backend opts into ReplayGain by defining `TDSP_HAS_REPLAYGAIN` and providing:

```c++
ILoudnessMeter*      synthLoudness();      // the probe on the raw sum (Tier-1 sweep reads this)
AudioEffectGain_F32* synthAuditionTrim();  // the Tier-1 bus gain stage
float                synthVoiceTrim(int i);// Tier-1 baked table lookup (unity if unswept)
const char*          synthTrimSymbol();    // e.g. "kOpllVoiceTrim" — paste-block label
// Tier-2 (multitimbral backends only): the sink applies gmProgramTrim(prog) per channel.
```

`runGainSweep()` in `main.cpp` is backend-agnostic: it already loops on
`synthNumInstruments()` / `synthSetInstrument()` / `synthInstrument()`, and now reads the
probe + forces the trim to unity + prints the paste block via these hooks. The `N` command
and the Dexed-only capture diagnostics are gated: `N` compiles for **any** backend that
defines `TDSP_HAS_REPLAYGAIN`; the onset/PROOF/jump captures stay Dexed-pool-only.


## Regenerating a backend's table (on-device, per build)

The trim table is **device-specific** (it depends on the codec/output chain), so it's
measured on hardware, not computed offline. Ships at **unity** until swept.

1. Flash the target env (e.g. `teensy41_opll`), open the serial monitor.
2. Press **`N`**. It plays a fixed reference note (C4, vel 100) through every instrument,
   prints one `V=<i> loud=<k-weighted> peak=<raw>` line each, then a paste-ready
   `static const float <symbol>[<N>] = { … };` block.
   - Target = **median** perceptual loudness over sounding voices → trims center near 1.0.
   - Each trim = `min(target/loud, peakCeil/peak)`, clamped `[0.10, 6.0]` — loudness-match
     but cap boosts so a normalized voice can't clip harder than the loudest raw voice.
   - If a voice faults the audio ISR mid-sweep, every prior `V=` line already printed;
     resume with control line **`@GAIN=<next index>`** after the auto-reboot; the host
     stitches the `V=` lines.
3. Paste the block over the backend's table header (`DexedVoiceGains.h`,
   `OpllVoiceTrim.h`, …) and reflash.

**Tier-2 GM tables** (multitimbral backends) are swept the same way but with the picker
forced to *audition each GM program 0–127* and measuring — the sweep is the same code; the
table is 128 entries applied per-channel on Program Change.

Downstream, the per-bus `SoftLimit_F32` (Dexed pool) / clamp catches any coincident-voice
peaks that a per-voice trim can't prevent (stacking), so trims can be honest about loudness
without risking hard clip.


## Rollout status

| Backend | Tier-1 audition | Tier-2 song norm | Table swept |
|---------|-----------------|------------------|-------------|
| Dexed pool | ✅ (`kDexedVoiceTrim[320]`) | n/a (single-timbre) | ✅ |
| Dexed (single) | pattern ready | n/a | — |
| OPLL | ✅ (`kOpllVoiceTrim[115]`) | ✅ coarse (3 dB nibble) | ⬜ unity |
| SF2 / TSF | ✅ | ✅ (float ch volume) | ⬜ unity |
| OPL3 | pattern ready | pattern ready (TL) | — |
| OPM | pattern ready | pattern ready (TL) | — |
