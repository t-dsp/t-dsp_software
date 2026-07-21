# Adding MPE to a T-DSP synth

MPE (MIDI Polyphonic Expression) gives every note its own continuous pitch, pressure
and timbre. In T-DSP the plumbing is already synth-agnostic — the work of adding MPE to
a new backend is entirely in that backend's `MidiSink` (and, if the engine can't do it,
the engine). This is the checklist and the two worked examples.

## The one thing to understand first

`MidiRouter` does **not** do anything MPE-specific. It fans every event out to every
sink **with the source channel intact**, having already pre-scaled the hard parts:

| Router callback              | What it carries                                             |
|------------------------------|------------------------------------------------------------|
| `onNoteOn/Off(ch, note, vel)`| note on member channel `ch`                                |
| `onPitchBend(ch, semitones)` | **float semitones**, already scaled by that channel's bend range (RPN-tracked, ±48 for LinnStrument) |
| `onPressure(ch, 0..1)`       | channel pressure (MPE Z-axis)                              |
| `onTimbre(ch, 0..1)`         | CC#74 (MPE Y-axis)                                         |
| `onModWheel/onSustain(...)`  | typically master-channel (ch 1) only                       |

**MPE is a channel convention, not a code path.** Channel 1 is the master; channels
2..16 each carry exactly one note with its own bend/pressure/timbre. A backend "supports
MPE" when it (a) gives each member channel its own voice and (b) applies that channel's
expression to just that voice. A non-MPE sink simply collapses everything to one channel.

See [`src/MidiSink.h`](src/MidiSink.h) for the interface and
[`src/MidiRouter.cpp`](src/MidiRouter.cpp) for the scaling.

## The checklist

Every MPE backend does these four things. The rest is engine-specific.

### 1. Voice-per-note allocation, keyed by member channel

Two strategies depending on the engine:

- **Native multitimbral** (TSF, SF2, OPL3, ymfm-multi): the engine already has one
  independent voice/state block per MIDI channel. Nothing to allocate — member channel
  *is* the voice. Just forward channel-addressed events straight through.
- **Monotimbral + pool** (Dexed): the engine's bend/pressure are global to one instance,
  so you can't get per-note expression from a single engine. Own **N engines** and map
  member-channel → engine, one active note per engine. See `DexedPoolSink` below.

### 2. Override the three expression virtuals — channel-scoped

Apply each to **only the voice(s) on that channel** (not globally):

```cpp
void onPitchBend(uint8_t ch, float semitones) override; // -> engine bend for ch's voice
void onPressure (uint8_t ch, float v)         override; // -> volume/gain swell on ch's voice
void onTimbre   (uint8_t ch, float v)         override; // -> filter cutoff / brightness on ch's voice
```

Bend arrives pre-scaled in **semitones** — map it to your engine's native bend units and
set that engine's own bend range wide enough (≥12 for a full MPE octave). Don't re-apply
a bend-range curve; the router already did.

### 3. A `synthSetMpeMode(bool)` hook

MPE mode needs setup the router can't do for you:
- **Unify the patch across member channels** — in MPE every note lands on a different
  channel, so channels 2..16 must all play the *same* selected instrument (not GM per-channel
  programs).
- **Free channel 10 from drums** — GM pins ch10 to the drum kit; MPE needs it melodic.
- **Widen bend range** to the MPE default (the ESP32 `applyMidiMode` sets the router to ±48).
- **Panic on switch** so no note carries a stale per-voice bend across the mode change.

Each backend header exposes `synthSetMpeMode()`; `applyMidiMode()` in `main.cpp` calls it.
A backend that hasn't implemented MPE leaves it a no-op.

### 4. Reset on note-on, clear on panic

Per-voice expression **latches**. Two invariants keep it from leaking:
- **On note-on**, reset that voice's expression to neutral (full gain, no bend/swell) so a
  reused voice/channel doesn't inherit the previous note's swell.
- **On panic / mode switch**, clear every latched value — killing the note isn't enough;
  the last bend/pressure/timbre stays latched and the *next* note would start pre-bent.

## Worked example A — TSF (native multitimbral)

TSF is a self-contained GM engine with a per-channel API, so the sink is thin — forward
each event to `tsf_channel_*(ch-1, ...)`. See
[`../../projects/spike_esp32_bt_spdif_mix_kit_f32/src/TsfSink.h`](../../projects/spike_esp32_bt_spdif_mix_kit_f32/src/TsfSink.h).

- **Bend**: `tsf_channel_set_pitchwheel` after setting a 48-semi range once. ✓ per-note.
- **Pressure**: `tsf_channel_set_volume`, which TSF applies to live voices → per-note swell. ✓
- **Timbre**: forwards CC#74 to `tsf_channel_midi_control`. Stock TSF ignored CC#74, so
  this was **inert**. Fixed by patching the engine (below).
- **MPE hook** (`SynthBackendSF2Tsf.h`): points all 16 channels at the selected instrument,
  frees ch10.

### The engine patch that completed the timbre axis

Stock `tsf.h` has a per-voice lowpass but no way to move its cutoff from a controller. The
CC#74→cutoff path was added with a handful of `// T-DSP:`-marked edits:

1. `struct tsf_voice`: `float filterFcOffset` — cents added to the region's `initialFilterFc`.
2. `struct tsf_channel`: `float cutoffCents` — the channel's current CC#74 offset.
3. `tsf_channel_setup_voice`: new notes inherit `c->cutoffCents` (so a note struck mid-slide
   starts correctly darkened).
4. `tsf_voice_render`: `filterFcOffset` forces the dynamic-lowpass path so a **held** note's
   cutoff tracks the slide live, even on patches with no LFO/env→Fc routing.
5. `tsf_channel_midi_control` case 74: maps CC#74 → cents and calls `tsf_channel_applycutoff`,
   which pushes the offset onto that channel's live voices (mirrors `tsf_channel_applypitch`).

**Convention chosen:** CC#74 = 127 is neutral (patch-open); lower values close the filter,
up to 4 octaves darker. Rest-at-open means normal GM playback — which never sends CC#74 — is
untouched, matching the Dexed pool's "note starts full, expression swells *down*."

## Worked example B — Dexed pool (monotimbral + pool)

Dexed's bend/mod/aftertouch are global to one engine, so `DexedPoolSink` owns 8 engines and
allocates **one engine per member channel** in MPE mode (falling back to oldest-note
stealing when all engines are busy). See
[`../../projects/spike_esp32_bt_spdif_mix_kit_f32/src/DexedPoolSink.h`](../../projects/spike_esp32_bt_spdif_mix_kit_f32/src/DexedPoolSink.h).

Key ideas worth stealing for any pooled backend:
- `forEachTarget(ch, fn)` — in MPE, runs `fn` on just the channel's engine; in normal MIDI,
  broadcasts to all engines (channel-wide bend applies to every note — correct for legacy MIDI).
- **Expression routing masks** — pressure / mod / timbre each route to any combination of
  destinations (volume swell, brightness, vibrato, tremolo). Overkill for a filter-only
  engine, but the multiplicative `combinedGain()` (several sources → one gain without fighting)
  is a good pattern when >1 source drives volume.
- `panic()` recenters bend and zeroes every controller on every engine — invariant #4.

## Current per-backend status

| Backend             | Model            | Bend | Pressure | Timbre        | MPE hook | Notes |
|---------------------|------------------|------|----------|---------------|----------|-------|
| **Dexed pool**      | 8-engine pool    | ✓    | ✓        | ✓             | ✓        | reference impl |
| **TSF**             | native GM        | ✓    | ✓        | ✓ *(patched)* | ✓        | the chosen GM engine |
| SF2 (Sf2GmEngine)   | native GM        | ✓*   | —        | —             | no-op    | bend plumbed; needs hook + pressure/timbre. Older engine |
| OPL3                | 18-voice FM      | ✓*   | —        | —             | no-op    | bend plumbed; timbre could map to operator level. Weak GM bank |
| OPLL (single chip)  | 9-voice FM       | ✓    | ✓        | ✗ *(chip)*    | n/a      | `teensy41_opll`. 2-axis: bend + pressure→volume. One shared user-voice bank → no per-note timbre |
| **OPLL pool**       | N×YM2413, 1 note each | ✓ | ✓     | ✓             | ✓        | `teensy41_opll_pool`. **Full 3-axis** — one whole chip per note, CC#74 → that chip's modulator TL |
| Multisample sampler | 8-voice pool     | —    | —        | —             | n/a      | has voice pool + `setPlaybackRate` for bend |
| ymfm OPM (single)   | mono-timbre      | —    | —        | —             | no-op    | furthest behind — no per-note state anywhere |
| ymfm OPM (multi)    | per-ch banks     | —    | —        | —             | no-op    | needs per-ch bend state + expression |
| Dexed (single)      | 1 engine, global | glob | glob     | —             | no-op    | superseded by the pool |

`✓*` = per-channel bend is plumbed through the engine but the backend has no `synthSetMpeMode`,
so member channels aren't actually unified into an MPE setup yet.

`✗ *(chip)*` = a hard limit **of one chip**, not missing code. A single YM2413's melodic
voices are fixed ROM patches and its one user voice is a single register bank shared by all
9 channels, so per-note timbre (CC#74) can't exist on it — `OpllSink::onTimbre` is a
deliberate no-op. That single-chip backend is per-channel-native (like TSF) for pitch/pressure,
so it needs no `synthSetMpeMode` hook: `onPitchBend` and `onPressure` (→ live volume-nibble
rewrite) act on the channel's voice directly. 2-axis (bend + pressure→volume) is its ceiling.

**The OPLL *pool* breaks that ceiling** by spending hardware instead of cleverness: run N
independent `ym2413` instances (`AudioSynthYmfmOPLLPool` + `OpllPoolSink`), one note per chip,
and each note gets its OWN user-voice bank — so CC#74 modulates that chip's modulator Total
Level ($02) for genuine per-note brightness. This is the same "monotimbral + pool" strategy as
the Dexed pool (one engine per member channel in MPE, round-robin poly otherwise), and it makes
OPLL a full 3-axis MPE backend. Cost: N whole chips of RAM/CPU (pool is DMAMEM/RAM2, and the
build drops S/PDIF-in to fit); pick the single-chip backend when you want 9-voice GM + rhythm
on the cheap, the pool when you want expressive per-note MPE performance.

MPE member-channel bend range is ±24 semitones (`kMpeMemberBendRange` in the mix-kit's
`applyMidiMode`) — the LinnStrument default, two octaves each way. `computeFB` spans the OPLL's
8 frequency blocks continuously, so a full-surface slide glides across octaves without clamping.

> Note: the Teensy-side project (`t-dsp_f32_audio_shield`) has several already-MPE-native
> engines — `MpeVaSink` (per-voice cutoff reference), Plaits, Supersaw, Neuro, Chip, Acid.
> The unfinished backends are the GM/chip engines and the sampler listed above.
