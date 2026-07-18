# Drum Note Map — Design

Status: **design** (2026-07-17). Author: agent.
Goal: play GMD (Groove MIDI Dataset) drum grooves with the **correct instruments**
on whatever GM engine is built, and leave the door open for an **authentic
Roland/TD-11 kit** on PSRAM boards — without ever mangling the source `.mid` files.

This document is self-contained: it records the bug, the measurement, the
constraint (GM vs. Roland percussion maps), the recommended architecture
(lossless assets + a capability-gated remap at playback), the honest limits of
the "authentic" path, and a phased plan. Phase 1 is a concrete, standalone fix.

---

## 1. The problem (measured)

GMD was recorded on **Roland electronic kits** and uses the **Roland/TD-11
percussion map**, which places the closed/open hi-hat *edge* articulations on
MIDI notes **22 and 26** — **below** General MIDI's percussion range (35–81).

Our fetch pipeline (`tools/fetch_drums.py`, `_loopify_gmd` / `_force_channel10`)
normalizes the **channel** to 10 but passes the **note numbers through
unchanged**. On a GM engine (TSF/SF2/OPL3/OPLL) there is nothing mapped below 35,
so notes 22/26 are **dropped silently** — the groove loses its hi-hat, which is
often the primary timekeeping voice.

Scan of the staged pack (`c:/tmp/t-dsp-drums/drums`, 1,113 GMD grooves):

| Finding | Value |
| --- | --- |
| Files containing non-GM notes (<35 or >81) | **404 (36%)** |
| Distinct out-of-range notes seen | **only 22 and 26** |
| Note 22 total hits | 2,077 (**4th-most-common note pack-wide**, above pedal-hat 44 and open-hat 46) |
| Note 26 total hits | 597 |

Example — `gmd neworleans-funk 102bpm.mid`: note **22 (×14) is the single most
frequent note** in the file (the steady hi-hat). On our engines that voice is
**missing entirely**, leaving only kick + snare ghosts → the groove sounds thin,
wrong, and rhythmically off.

Roland → GM translation (Magenta's canonical GMD mapping):

- **22 = Closed Hi-Hat (edge) → 42** (Closed Hi-Hat)
- **26 = Open Hi-Hat (edge) → 46** (Open Hi-Hat)

Everything else the pack uses (36, 37, 38, 40, 42, 43, 44, 45, 46, 48, 50, 51,
55, …) is already valid GM and renders on a GM font.

> Note: this is **separate** from the tempo issue tracked elsewhere (a groove
> inheriting the wrong master BPM on a quantized bar-join / under a song). A
> broken groove can suffer both at once.

---

## 2. Key decision — the SD groove is the source of truth

The **wrong** fix (an earlier proposal): rewrite 22→42, 26→46 into the `.mid`
files at fetch time. That is **lossy and one-way** — once the edge-hat
distinction is gone from disk, no future engine can recover it, and re-fetching
is the only way back.

The **right** fix: **keep the Roland note numbers on the card untouched** and
move the map decision to **playback**, where we know which engine/font is active
and what it can render. The fetcher's only job is to stop *implying* the notes
are GM (it already forces channel 10; it must not silently corrupt notes — see
the `_force_channel10` running-status history in that file for the class of bug
we avoid).

Corollary: this also future-proofs the `/songs` GM drum tracks — they use GM
notes, which an extended (superset) font renders natively with zero remap.

---

## 3. Architecture

### 3.1 `DrumNoteMapper` — a MidiSink shim

A tiny `tdsp::MidiSink` inserted between `g_drumPlayer` and the real drum sink
(`g_synthSink` / `g_drumTsfSink` / `g_drumVoiceSink`), mirroring the arp-filter
insertion pattern. On channel-10 note-on/note-off it maps the note number by the
current **mode**; velocity, timing, and all other events pass through untouched
(so dynamics and ghosting are preserved in every mode).

```
g_drumPlayer (ch10) ─► DrumNoteMapper ─► <drum sink>
```

Modes:

- **`Passthrough`** — the active font has regions at 22/26 (and the rest of the
  TD-11 map). Emit notes as-is → authentic.
- **`GmReduce`** — collapse the Roland map to GM (`22→42`, `26→46`; the full
  Roland table folded in for safety even though only 22/26 occur today). Emit the
  GM note → audible on any GM font.

The reduce table lives in one place (`lib/TDspMidi` or a small `DrumNoteMap.h`
next to the drum sinks) so both the firmware shim and — if we ever want an
offline path — a tool can share it.

### 3.2 The mode is a property of the FONT, not the groove

Add one field to the drum-kit / engine descriptor. Candidates:

- `EngineCaps` (`firmware/mix-kit/src/CatalogDb.h:252`) already carries
  `hasDrums` / `drumEngine`; add a `drumNoteMap` enum (or a bool
  `drumMapExtended`).
- The `kDrumKits[]` table (`main.cpp:1401`) is orthogonal — it selects the **GM
  program** (Standard/Room/Jazz/808…). The note-map mode is about **key
  coverage**, a font attribute, not a kit-program attribute. Keep them separate.

Default assignment:

| Drum font / engine | Mode |
| --- | --- |
| TimGM6mb via TSF/SF2 (current `gm_tsf.sf2`) | `GmReduce` |
| OPL3 GM, OPLL rhythm voice (`DrumVoice.h`) | `GmReduce` |
| A dedicated **V-Drums (TD-11)** SF2 in the `DrumTsf` slot | `Passthrough` |

### 3.3 Why the extended font is the elegant end-state

An extended (TD-11 superset) drum font renders **both** the GMD-native grooves
**and** the GM `/songs` drum tracks correctly with `Passthrough` always on. On a
PSRAM board it simply becomes the better **default drum font** loaded into the
existing dedicated `DrumTsf` instance (`DrumTsf.h`, mix slot 2) — no per-groove
branching. `GmReduce` remains the fallback for no-PSRAM / OPLL / OPL3 builds.

---

## 4. Honest limits of the "authentic" path

`Passthrough` only *sounds* better than `GmReduce` if the SF2 actually contains
**distinct edge/bow samples** at keys 22/26. Many freely available V-Drum
soundfonts map every closed-hat articulation to a single sample; with such a
font, passthrough-with-duplicated-regions is **sonically identical** to
`GmReduce`.

Therefore:

- The **architecture** (§2–§3) is worth building regardless — it is lossless,
  correctly routed, and future-proof, and it fixes the silent-hat bug now.
- The **audible upgrade** of §3.3 is gated on **sourcing a genuinely
  multi-articulation kit**. Treat that as a research task with an uncertain
  payoff, not a guaranteed win. Building the extended SF2 (or verifying an
  existing one has real 22/26 regions) is P2 work, tracked separately.

---

## 5. Phasing

### Phase 1 — the fix, losslessly (standalone, high-confidence)
- Add `DrumNoteMapper` + the `GmReduce` table; insert it in the drum chain in
  `setup()` for every current engine (default mode `GmReduce`).
- Fix `tools/fetch_drums.py` so it never corrupts notes and (optionally) records
  in `catalog.tsv`/attribution that grooves carry **native Roland** notes.
- **No asset rewrite, no re-fetch, no reflash of the card** — the existing SD
  grooves start playing their hi-hats immediately after a firmware flash.
- Verify: green-build `teensy41_opll`; on HW, load `gmd neworleans-funk 102bpm`
  and confirm the hi-hat (was note 22) now sounds. Off-target, unit-test the
  reduce table.

### Phase 2 — authentic path (PSRAM)
- Source or build a TD-11 / extended-GS drum SF2 with real regions at 22/26.
  Confirm the regions are *distinct* (else document that it degrades to GM parity).
- Add a **"V-Drums (TD-11)"** kit/font option that sets the mapper to
  `Passthrough`; load it into the `DrumTsf` slot; make it the default drum font
  where it fits in PSRAM.
- Surface the mode in the app (so it's clear whether drums are authentic or
  GM-reduced).

### Phase 3 — optional
- True per-articulation handling / velocity-layer nuance if a multi-sample kit
  proves worth it. Only if P2 shows a real audible ceiling.

---

## 6. Files this touches

- `firmware/mix-kit/src/` — new `DrumNoteMap.h` (table + `DrumNoteMapper`
  shim); wire in `setup()` where `g_drumPlayer.setSink(...)` runs
  (`main.cpp` ~2908 and the TSF/OPLL sink overrides ~2947/2954).
- `firmware/mix-kit/src/CatalogDb.h` — `EngineCaps.drumNoteMap` (P1 optional; the
  shim can default to `GmReduce` without a caps field until P2 needs the toggle).
- `firmware/mix-kit/src/DrumTsf.h` — P2 font path / `Passthrough` selection.
- `tools/fetch_drums.py` — P1 note-integrity + attribution note; **do not** bake
  the remap.
- `planning/tracks/DRUMS.md` — cross-link once P1 lands.
