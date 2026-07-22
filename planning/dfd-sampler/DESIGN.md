# `dfd` — a Direct-From-Disk streaming sample engine

A small, **standalone** C++ library for playing audio samples that are too big (or too many) to
hold in RAM, with **zero disk I/O in the trigger/restart path**. It keeps a short **resident head**
of each region in fast memory and **streams the body** from storage behind it, so note-starts,
loop wraps, and stutter restarts are always instant and glitch-free.

This is the technique pro streaming samplers use (Kontakt DFD, HALion, EXS "virtual memory"): a
**preload buffer** per sample + background disk streaming. This doc is the design; it is written so
the result is a **clean, reusable library** (usable outside T-DSP), which T-DSP then merely consumes.

> Status: DESIGN. Supersedes the ad-hoc streaming in `DrumSampler.h` + `teensy-variable-playback`
> once phase 1 lands. Origin: the drum-sampler skip investigation (SD `open`/first-read stalls at
> trigger) → the realization that a resident head hides all disk latency, and that a *region* is the
> one primitive behind note-start, looping, and stutter.

---

## 1. The one primitive: a *streamed region*

Everything the engine does is "play a **region**, always restarting from its resident head."

```
A region [start, start+length):

  sample index:  start ........ start+H .................... start+length
                 |── HEAD ──────|────────── BODY (streamed) ───────────|
                 resident in RAM  a ring that stays ahead of the playhead
                 (fixed size H)   and wraps as it streams from storage

  restart / loop-wrap  ─────────────┐
  jumps the playhead back to `start` ┘  → reads the HEAD (RAM) → INSTANT,
  while the BODY ring keeps/refetches [start+H, …) behind the head.
```

The head length **H must exceed the worst-case storage stall** (e.g. an SD card's occasional
~100 ms housekeeping pause). Then the body always has H worth of playtime to catch up, and the
trigger never waits on storage. That single inequality is the whole guarantee.

Once regions exist, the higher-level behaviours are all *the same thing*:

| Behaviour | Region |
|---|---|
| Note start | `[0, len)`, head at 0 (the classic "attack") |
| Sustain / looper loop | `[loopStart, loopEnd)`, head at `loopStart`, wrap → head |
| Beginning stutter | loop `[0, L)` — head is already the attack |
| **Mid-note stutter** | loop `[X, X+L)` — head at X, built on demand (§5) |

So "stutter the middle" is not a new mechanism — it's *make a region at X and loop it*, and the
head-at-X is what makes each restart instant.

### On "two rings"
Per voice the natural shape is **one resident head buffer + one streaming body ring** — the head is
a *fixed buffer*, not a ring (you re-read it from RAM on each restart). A genuine *second* read head
is worth having, but for **loop-seam crossfade** (play the loop tail while the loop head restarts,
crossfade → no click), not for the attack. Stutter slices are transient mini-regions, not a fixed
second ring. Net: `head + bodyRing` per voice, plus an optional crossfade head at loop seams.

---

## 2. Library shape — portable core + injected backends

The point of the rewrite is a library that is **not welded to Teensy, SD, or this repo**. Structure:

```
dfd/                      (MIT, self-contained, no T-DSP includes)
  include/dfd/
    Region.h              CORE — portable, no platform deps. The head+body+loop+stutter logic.
    Source.h              interface: read PCM at an offset (see §2.1)
    Allocator.h           interface: alloc/free buffers, PSRAM-aware
    Resampler.h           CORE — variable-rate read over a Region (pitch), interpolation
    Voice.h               CORE — a Region+Resampler+envelope = one playable voice
    Pool.h                CORE — optional N-voice pool with stealing
  src/                    core .cpp (portable, C++17, no Arduino)
  backends/
    teensy/
      SdSource.h          Source over Teensy SD `File`
      PsramAllocator.h    Allocator using extmem_malloc / heap by external_psram_size
      AudioPlayDfd.h      Teensy `AudioStream` node (int16 stereo) wrapping a Voice/Pool
      AudioPlayDfd_F32.h  OpenAudio F32 node variant
    host/
      MemorySource.h      Source over a RAM buffer (for desktop tests + fully-resident mode)
      StdAllocator.h      malloc/free
  test/                   desktop unit tests (run on CI/dev box, NO hardware) — like
                          teensy-variable-playback's linux tests
  examples/
  README.md  LICENSE(MIT)
```

**Rules that keep it reusable:**
- `include/dfd/*` (the core) includes **only** `<cstdint>`/`<cstddef>` and the two injected
  interfaces — never `Arduino.h`, `SD.h`, `extmem_*`, or anything T-DSP.
- All platform touchpoints go through **`Source`** (storage) and **`Allocator`** (memory), injected
  at construction. Teensy/SD/PSRAM live only under `backends/teensy/`.
- The core is **allocation-free on the audio path** (buffers allocated once at load; the audio
  callback only reads them).
- Desktop-testable: `backends/host/` lets the entire engine run and be unit-tested off-target,
  so the tricky head→body handoff and stutter logic are proven without a board in the loop.

### 2.1 The two injected interfaces (the whole platform surface)

```cpp
namespace dfd {

// Random-access PCM source. offsetSamples/count are in int16 SAMPLES (channel-interleaved).
// Returns samples actually read (< count at EOF). Called from a NON-realtime context only
// (the streamer service, §6) — never from the audio callback.
struct Source {
  virtual ~Source() = default;
  virtual uint32_t totalSamples() const = 0;         // length of the sample data
  virtual uint8_t  channels()     const = 0;         // 1 or 2
  virtual uint32_t read(uint32_t offsetSamples, uint16_t* dst, uint32_t count) = 0;
};

// Buffer allocator. `preferFast` asks for the fastest pool available (PSRAM on Teensy);
// implementations fall back to normal RAM when it's unavailable or preferFast is false.
struct Allocator {
  virtual ~Allocator() = default;
  virtual int16_t* alloc(size_t samples, bool preferFast) = 0;
  virtual void     free(int16_t* p) = 0;
};

} // namespace dfd
```

Everything else in the core is expressed against these. A consumer on a totally different platform
implements `Source`/`Allocator` and gets the whole engine.

---

## 3. Core API sketch

```cpp
namespace dfd {

struct RegionConfig {
  uint32_t headSamples;     // H — MUST exceed worst-case storage stall (§7 picks it)
  uint32_t ringSamples;     // body ring size (per voice); >= a few blocks past H
  uint32_t historySamples;  // trailing look-behind kept in the ring (for backward stutter, §5)
};

// One streamed region bound to a Source. Not thread-safe across the audio/stream boundary except
// through the documented single-producer(stream)/single-consumer(audio) buffer discipline (§6).
class Region {
public:
  Region(Source& src, Allocator& alloc, const RegionConfig&);

  void  loadHead(uint32_t start = 0);        // fill the resident head for [start, start+H); ONE read
  void  play(uint32_t start, uint32_t length, bool loop, uint32_t loopStart = 0);
  void  jump(uint32_t sample);               // seek the playhead (restart-from-head if within a head)
  bool  read(int16_t* dst, uint16_t frames); // AUDIO PATH: pull `frames` (native rate). No FS, no alloc.
  bool  active() const;

  // service() runs the actual Source.read() to refill the body ring. Call from loop()/EventResponder,
  // NOT the audio callback. Returns true if it did work. Bounded per call (§6).
  bool  service();

  // ---- stutter (§5) ----
  void  stutterOn(uint32_t sliceLen);        // loop [currentPos, currentPos+sliceLen)
  void  stutterOff();                        // resume the underlying region where it would be
};

} // namespace dfd
```

`Voice` = `Region` + `Resampler` (pitch) + a tiny amplitude envelope; `Pool` = `Voice[N]` with
idle→oldest stealing (the policy we already tuned). A Teensy `AudioStream` node owns a `Pool` and
calls `read()` in `update()` and `service()` from a periodic hook.

---

## 4. Playback mechanics (the head→body handoff)

The only genuinely delicate part; get it right once and everything composes.

- **Contiguity at H.** Head covers samples `[start, start+H)`. The body ring is **preloaded starting
  at `start+H`** (never 0), so head sample `H-1` is followed by body sample `H` with no gap and no
  double-read.
- **read() (audio path):** if `playhead < start+H` → copy from the head buffer; else → copy from the
  body ring at `playhead - (start+H)`. Purely index math + memcpy; no branches into storage.
- **service() (stream path):** keeps the ring filled ahead of `playhead` and retains `historySamples`
  behind it. Reads from `Source` in bounded chunks so it never stalls the caller's loop.
- **Loop wrap:** at `loopEnd`, set `playhead = loopStart`. If `loopStart` has a resident head (it does
  for the region's own start; a loop point inside the body uses a small **loop head**, §5 Case B),
  the wrap reads RAM → instant. Optional crossfade head smooths the seam.
- **Underrun policy:** if the ring is somehow dry (shouldn't happen when `H > worst stall`), emit
  silence for that block and let service() catch up — never read the filesystem from the audio path.
  A debug counter (opt-in) tallies these, exactly like the `@DRUMSTRESS`/underrun probe we built.

---

## 5. Stutter mechanics (mid-note)

A stutter/beat-repeat = *capture position X now, loop `[X, X+L)`, restart it sample-accurately at a
fast rate.* Split by slice length L vs head length H:

**Case A — short slice (`L ≤ H`), the common musical case (1/8…1/32 ≈ 60–250 ms):**
The whole slice fits in fast RAM → **no streaming during the stutter** → tightest possible.
1. `stutterOn(L)` captures `X = playhead`.
2. `[X, X+L)` is almost always already in the body ring (X is right where you're playing), so **copy
   it RAM→RAM** into a small slice buffer (from the `Allocator`, `preferFast`). Cold jump = one read.
3. Loop the slice buffer. Each restart replays slice sample 0 → instant, sample-accurate.

**Case B — long region (`L > H`), e.g. a bar-length looper loop:**
Can't hold it all → it's just a **full DFD region** at X: resident head `[X, X+H)` + streamed
`[X+H, X+L)`, wrap → head. Identical to a normal loop; the looper's bar-loop and a long stutter are
the same code.

**Look-behind.** "Repeat the last 1/16" looks *backward*, so the ring keeps `historySamples` behind
the playhead (§3). Forward/"from-here" stutter needs no history.

**Release/resume.** Case A copies the slice *out*, so the underlying region's stream is never frozen;
`stutterOff()` resumes exactly where playback would have been — an insert effect, not a transport jump.

---

## 6. Realtime / threading model

Single-producer / single-consumer per voice, no locks on the audio path:

- **Audio callback (ISR/high-prio):** only `read()` — index math + memcpy over buffers that already
  exist. Never allocates, never touches `Source`.
- **Streamer (loop()/EventResponder/low-prio task):** only `service()` — the sole caller of
  `Source.read()`; refills rings, fills stutter-slice buffers, loads heads. Bounded work per call so it
  never stalls the host loop (the mistake the old lib made: a big synchronous read at trigger).
- **Handoff** uses release/acquire on the ring's head/tail indices (or `AudioNoInterrupts` windows on
  Teensy, matching the existing lib) — a documented, minimal critical section.
- **Load** (heads + first ring fill) happens off the audio path at `play`-prep time; heads are already
  resident by the time the first `read()` runs.

This is also what makes it portable: the core just needs "someone calls `service()` regularly from a
non-audio context," however the host schedules that.

---

## 7. Memory & PSRAM scaling (`external_psram_size`)

Heads are **fixed-size regardless of sample length**; slices are short and transient. So:

- **No fast RAM:** heads from normal RAM with a small H (or `H = 0` → pure stream, today's fallback).
  Short stutters still work (slice copied to heap).
- **PSRAM present:** `PsramAllocator` puts heads (and slice buffers) in PSRAM via `extmem_malloc`; H
  scales up for more stall margin; more voices/simultaneous stutters cached. **Huge** libraries fit
  because *only heads* live in PSRAM, not whole samples.

One knob (`external_psram_size`, read once in the backend) picks the allocator pool and the H budget;
the **core code is identical** both ways. This is the elegant form of the scaling idea: PSRAM buys
**latency-hiding**, not residency.

---

## 8. Pitch / format

- **Storage format:** 16-bit PCM (mono/stereo), 48 kHz preferred. The `Source` yields int16; that's
  what samples/soundfonts store.
- **Pitch:** a `Resampler` (variable-rate read with interpolation) sits *on top of* a `Region` — clean
  separation of "buffering/streaming/loop" (Region) from "pitch" (Resampler). Drums use rate 1.0; a
  pitched sampler / SF2 varies it. (We can lift the proven interpolation from
  `teensy-variable-playback`, which is MIT.)
- **Output:** the Teensy backend node emits int16 stereo; an F32 variant converts for the OpenAudio
  graph (as `DrumSampler` does today with `AudioConvert_I16toF32`).

---

## 9. Build phases (each independently useful + testable)

1. **DFD core + Teensy backend** — `Region` (head + body ring, preload-from-H, clean handoff) +
   `SdSource` + `PsramAllocator` + `AudioPlayDfd`. Re-point `DrumSampler` at it. **This alone removes
   SD from the trigger path** → kills the residual skips. *Validate:* `@DRUMJIT`/underrun probes show
   **zero** trigger spikes even with a cold cache; desktop unit tests on the handoff.
2. **Loop regions** — `loopStart/End` + reuse the crossfade dual-head for click-free seams. Backs the
   **looper**. *Validate:* `@CAP` waveform is seam-clean; desktop loop tests.
3. **Stutter engine** — `stutterOn/off`, Case A (short, fully resident) then Case B (long, DFD).
   *Validate:* sample-accurate restart timing + a cold-jump test; a UI hook in the app.
4. **Streaming SF2 engine (later, the big one)** — parse the `.sf2`, one DFD `Region` per sample zone
   (heads resident, bodies streamed from the file). Lets a *really* large soundfont play without
   loading it. Sits entirely on phase-1's core.

The crux in every phase is the **head→body handoff** (contiguity at H, ring preload from H, wrap →
head). Prove it once in phase 1 and 2–4 are compositions of it.

---

## 10. Testing

- **Desktop unit tests** (`test/`, host backend): handoff contiguity (no gap/overlap at H), loop
  correctness, stutter restart accuracy, look-behind, EOF/short-file handling, allocator fallback.
  Runs with no hardware — the reason the portable-core split matters.
- **On-target probes** (this repo's harness): the existing `@DRUMJIT` (dispatch jitter) and the
  underrun counter (`@DRUMSTRESS`, fixed to run under a *real* groove, not a busy-wait) confirm zero
  trigger spikes and zero underruns on hardware.
- **Golden-audio**: `@CAP` a loop/stutter and diff the waveform for seam clicks.

---

## 11. Relationship to existing code

- **`teensy-variable-playback`** (MIT, vendored): source of the proven interpolation + crossfade
  ideas, and the buffer-reuse/persistent-handle fixes we already made. `dfd` is a clean re-home of
  those ideas behind the `Source`/`Allocator` seams, adding heads + regions + stutter. We keep the old
  lib until `dfd` phase 1 is validated, then migrate `DrumSampler`.
- **T-DSP consumes, never leaks in:** the mix-kit side is just `AudioPlaySdResmp` → `AudioPlayDfd`
  swap plus a `PsramAllocator`/`SdSource` wired from `external_psram_size` and the `/drums` files.
  Nothing T-DSP-specific enters `dfd/`.
- **TSF stays separate:** TSF is fully-resident GM and is not a DFD engine; the phase-4 streaming SF2
  player is the DFD-based alternative for *large* fonts, not a change to TSF.

---

## 12. Open questions (to settle before phase 1 code)

- Exact **H** default per storage class (measure worst-case SD stall on the target cards; the
  underrun probe already gives us the distribution).
- Ring vs history sizing defaults, and whether history is per-voice or a shared pool.
- Block-based `read()` (per Teensy 128-sample audio block) vs sample-based — block is faster; keep the
  API block-oriented.
- Whether `Pool`/stealing lives in `dfd` (convenient) or the consumer (leaner core). Leaning: ship it
  but keep `Region`/`Voice` usable standalone.
- License/attribution for any interpolation lifted from `teensy-variable-playback` (MIT → keep notice).
