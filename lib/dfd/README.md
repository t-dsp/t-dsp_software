# `dfd` — a Direct-From-Disk streaming sample engine

A small, **standalone** C++17 library for playing audio samples too big (or too many) to hold in
RAM, with **zero disk I/O in the trigger/restart path**. Each region keeps a short **resident
head** in fast memory and **streams the body** from storage behind it, so note-starts, loop
wraps, and stutter restarts are always instant and glitch-free — the technique of pro streaming
samplers (Kontakt DFD, HALion, EXS virtual memory).

> Full design and rationale: `planning/dfd-sampler/DESIGN.md`. This README is the library's
> orientation; the design doc is authoritative.

## The one idea (DESIGN §1)

A region `[start, start+length)` is split into a **head** `[start, start+H)` held in RAM and a
**body** `[start+H, start+length)` streamed into a sliding ring that `service()` keeps ahead of
the playhead:

```
sample index:  start ........ start+H .................... start+length
               |── HEAD ──────|────────── BODY (streamed) ───────────|
               resident in RAM  a ring that stays ahead of the playhead
```

Because the head is always in RAM, a trigger/loop-wrap/restart never waits on storage. The one
guarantee: **H must exceed the worst-case storage stall** — then the body always has H worth of
playtime to catch up at the trigger. The delicate part is the **head→body handoff**: the body
ring is preloaded starting at `start+H` (never 0), so head sample `H-1` is followed by body
sample `H` with no gap and no double-read. Prove that once (`test/`) and looping / stutter are
compositions of it.

## Layout

```
lib/dfd/
  include/dfd/                 CORE — C++17, portable, includes ONLY <cstdint>/<cstddef> + the
    Source.h  Allocator.h        two injected interfaces. No Arduino/SD/PSRAM/T-DSP anywhere.
    Region.h                     head + body ring + loop; the head→body handoff (the crux)
    Resampler.h                  variable-rate read over a Region (pitch), linear interpolation
    Voice.h  Pool.h              Region+Resampler = Voice; N voices + stealing = Pool
    backends/
      teensy/  SdSource.h PsramAllocator.h AudioPlayDfd.h   (SD File / extmem / AudioStream node)
      host/    MemorySource.h StdAllocator.h                (RAM buffer / malloc — for tests)
  test/  test_region.cpp run_tests.py     desktop unit tests (MSVC or any C++17 compiler)
  library.json  README.md  LICENSE(MIT)
```

> **Layout note (deviation from DESIGN §2's sketch):** the design drew `backends/` as a sibling
> of `include/`. Here the backends live under `include/dfd/backends/` so there is a **single
> include root** (`lib/dfd/include`, which PlatformIO exposes automatically) and every consumer
> include is `<dfd/...>`. The core-vs-backend split is preserved by directory; the reusable-core
> rule (core headers include only `<cstdint>/<cstddef>` + the interfaces) is intact.

## The two injected interfaces (the whole platform surface)

```cpp
struct dfd::Source {                                   // storage
  virtual uint32_t totalSamples() const = 0;           // interleaved int16 sample count
  virtual uint8_t  channels()     const = 0;           // 1 or 2
  virtual uint32_t read(uint32_t offsetSamples, int16_t* dst, uint32_t count) = 0;  // NON-realtime
};
struct dfd::Allocator {                                // memory (PSRAM-aware)
  virtual int16_t* alloc(size_t samples, bool preferFast) = 0;   // MUST fall back, never fail on
  virtual void     free(int16_t* p) = 0;                         //   fast-unavailable
};
```

Implement those two and the whole engine runs on any platform. (DESIGN §2.1 sketched `uint16_t*
dst`; PCM is signed, so the real type is `int16_t*` — identical bytes, honest type.)

## Realtime model (DESIGN §6)

Single-producer / single-consumer per voice, no locks on the audio path:

- **Audio path** (`Region::read`/`readFrame`, `Voice::read`, `AudioPlayDfd::update`): index math +
  copy over already-resident buffers only. Never allocates, never calls `Source`.
- **Stream path** (`Region::service`, `Voice::service`, `AudioPlayDfd::service`): the SOLE caller
  of `Source::read`; refills the ring, bounded per call. Run it from `loop()` / an EventResponder /
  any non-audio context — however the host schedules it.

## Memory scaling (DESIGN §7)

Heads are fixed-size regardless of sample length. `PsramAllocator` puts them in PSRAM when
`external_psram_size > 0` (H scales up, huge libraries fit because only heads live there), and
falls back to the OCRAM heap when there is none (keep `H × voiceCount` within budget — the
no-PSRAM squeeze). The core code is identical both ways.

## Desktop tests

```
python lib/dfd/test/run_tests.py            # discovers MSVC via vswhere, cl /EHsc /std:c++17
# or, any compiler:
g++ -std=c++17 -Ilib/dfd/include lib/dfd/test/test_region.cpp -o t && ./t
```

Covers, entirely off-target on the host backend: head→body contiguity (no gap/overlap at H),
the audio path being source-free over the head, loop wrap, EOF/short-file, fully-resident
regions, `jump()` restart-from-head, cold jump into the body, allocator fallback + OOM safety,
resampler rate-1.0 exactness, and pool stealing.

## Using it on Teensy (what mix-kit's DrumSampler does)

```cpp
dfd::PsramAllocator alloc;                              // PSRAM or heap, chosen once
dfd::AudioPlayDfd   voice;   // an AudioStream node, wire its 2 outputs into your mixer
voice.begin(alloc, {headSamples, ringSamples, historySamples});   // allocate ONCE, off audio path
// per trigger, from a persistent per-sample handle (no per-hit SD.open):
dfd::SdSource src; src.open(myPersistentFile); voice.play(src);    // one-shot; loop=true to sustain
// in loop(): for each sounding voice -> voice.service();
```

## Relationship to `teensy-variable-playback`

`dfd` is a clean re-home of the proven interpolation + buffer-reuse/persistent-handle ideas from
newdigate/teensy-variable-playback (MIT), behind the `Source`/`Allocator` seams, adding resident
heads + regions (+ stutter in a later phase). The Resampler's linear-interpolation is adapted
from it; see `LICENSE`. That library stays vendored until `dfd` is fully validated on hardware.

## Status

Phase 1 (this drop): core `Region` (head + body ring, preload-from-H, clean handoff) + `Resampler`
+ `Voice`/`Pool` + Teensy `SdSource`/`PsramAllocator`/`AudioPlayDfd` + host backend + desktop
tests. Wired into mix-kit's `DrumSampler` behind `-D TDSP_DRUM_DFD`. Phases 2 (loop crossfade
seam) / 3 (stutter) / 4 (streaming SF2) build on this same handoff — see DESIGN §9.
