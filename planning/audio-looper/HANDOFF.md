# Audio Looper — Handoff (overnight design run, 2026-07-16→17)

## What you asked for
Capture the device's actual digital audio into a looper that works like the home-page
MIDI looper, but loops real audio — efficient, fast, smooth seams, scaling with build
flags + PSRAM. Look at existing libraries; reuse or build.

## What's delivered (all NEW; no mix-kit/app files touched)
1. **`planning/audio-looper/DESIGN.md`** — the full design: what exists, the memory
   constraints, architecture (record-bus tap + final-mix to avoid overdub feedback),
   the smooth-seam DSP (equal-power overlap-crossfade), storage tiers
   (DMAMEM / EXTMEM-PSRAM / SD-stream), firmware `@AL*` + app-card integration mirroring
   the MIDI looper, a test plan, and open questions.
2. **`lib/TDspAudioLoop/`** — a working prototype: `tdsp::AudioLooper`, a **stereo
   `AudioStream_F32`** node, **int16** storage, **bar-locked** to `tdsp::Clock`,
   **click-free overlap-crossfade** seam, **overdub** (with tail-mirror), ISR-safe
   transport, `poll()` downbeat-quantized start. API mirrors `tdsp::MidiLooper`
   (`begin/setBars/armRecord/armOverdub/stop/clear/resume/poll/state/positionPermille`).
3. **`projects/spike_audio_looper/`** — a spike wiring the anti-feedback topology
   (record bus → looper → final mix → I2S) driven by a free-running Clock, with serial
   keys `r/o/s/c`.

## Build status — GREEN
`pio run -e teensy41` in `projects/spike_audio_looper` → **SUCCESS** (12 s). Compiles +
links against the real OpenAudio F32 framework. Footprint: RAM1 tiny (fade tables +
code), RAM2 ~200 KB = the 1 s stereo loop buffer (192 KB DMAMEM) + audio blocks, 318 KB
free. Confirms: **1 s stereo fits in DMAMEM on a no-PSRAM board; longer needs PSRAM or SD.**

## Key design decisions (rationale in DESIGN.md)
- **Reuse patterns, build new class.** `tdsp::Looper` (existing audio looper) is mono/
  int16/hard-cut/no-bars/no-overdub and is used by the shield-adaptor project — left
  untouched. New `tdsp::AudioLooper` adds stereo + crossfade + bar-lock + overdub +
  Clock integration.
- **Tap the record bus, not the post-loop master** — a new `finalL/finalR` mixer sums
  record-bus + loop-return; the looper records the record bus only → overdub can't feed
  back.
- **int16 stereo storage** (192 KB/s @ 48k) — half of F32, inaudible for loops.
- **Overlap-crossfade** (record loopFrames + 256-frame tail; equal-power sin/cos blend of
  head+tail over the first 256 frames of each pass) — the click-free seam `tdsp::Looper`
  lacks. This is THE smoothness mechanism.
- **Storage tiers:** DMAMEM small (no PSRAM, ~1 s), EXTMEM large (PSRAM, ~40 s @ 8 MB),
  SD-stream (phase 2, any board, long loops, needs a hand-written WAV header — none
  exists in-repo).

## NOT done (deliberately — needs decisions / your concurrent WIP)
- **No wiring into `firmware/mix-kit` or the app.** The mix-kit `main.cpp`/`transport.*`/
  `App.tsx` are being actively edited (voice-2 + 2nd-MIDI-player WIP); wiring in now would
  collide. §6–7 of DESIGN.md specify the exact `@AL*` commands, `@STATE` block, and app
  card to add when ready.
- **Clock-follow (tempo-track) playback** is stubbed at 1.0× in the prototype (design
  reuses `tdsp::Looper`'s frac-interp resample — add in phase 2).
- **SD `.wav` persistence** — designed, not built (needs the WAV header writer).
- **Off-target crossfade unit test** — specified in DESIGN §8.2; not yet written.
- **On-hardware audio verification** — can't be done without ears/board; flash the spike
  (`pio run -e teensy41 -t upload`) and listen (r/o/s/c) to hear the loop + seam.

## Decisions (locked 2026-07-17) — folded into DESIGN §9 + the prototype
1. **N independent loops** — one `AudioLooper` instance + buffer per loop; `@AL*` carries a
   loop index; final mix sums all returns. Meaningful N needs PSRAM.
2. **Mono toggle** — `setMono(true)` (pre-`begin`) halves RAM → 2× loop length on no-PSRAM.
3. **Follows the shared master clock** (same Conductor as MIDI player/arp/metro/drums) →
   clock-follow **on** by default (tempo-track via resample; toggle off to free-run at pitch).
4. **SD `.wav` save in v1** — `tdsp::saveWavFile()` in `AudioLoopWav.h` (hand-written header).
5. **Selectable source, default = main** (full master mix); synth-only/BT-only later.

## WIRED IN (2026-07-17) — firmware + app, GREEN, flashed + HW-checked
- **Firmware** (`firmware/mix-kit/src/main.cpp`, `TDSP_AUDIOLOOP` / `TDSP_AUDIOLOOP_N=2`):
  graph rewired to the anti-feedback topology (outL/outR = **record bus** → `finalL/finalR`
  ← loop returns → `tdmOut`); N `tdsp::AudioLooper` instances; buffers allocated at boot
  from **PSRAM (8 s/loop) else OCRAM (1 s/loop)**; `@ALSEL/@ALBARS/@ALMONO/@ALFOLLOW/
  @ALLEVEL/@AL/@ALDUB/@ALCLR/@ALPLAY/@ALSAVE`; `@STATE "aloop"` + `caps.audioloop`
  (a COUNT of loops that actually allocated); live `@ALP=` push; `AudioMemory_F32` bumped.
- **App**: `audioLoop*` transport methods (interface + web + native), `caps.audioloop`,
  `aloop` state + hydrate + `@ALP` parse, and a pink **"Audio Loop"** card (loop selector,
  bars 1/2/4/8 with lengths that don't fit greyed out, Mono + Follow-tempo switches,
  ●/⊕/■ + Save .wav + Clear).
- **Builds**: `teensy41_opll` **SUCCESS**, `teensy41_dexed_pool_nobt_voice2` **SUCCESS**,
  app `tsc --noEmit` **0 errors**. Flashed to COM4.

### THE HARDWARE FINDING (important)
On the COM4 dev board `@STATE` reports `"aloop":{...,"n":0,"cap":0}` / `"caps":{...,"audioloop":0}`
— **zero loops allocated**. That board has **no PSRAM** *and* runs `TDSP_LEAN_RAM` (OCRAM is
reserved for the SD-song heap), so even a 1 s stereo buffer (192 KB) won't fit. Verified the
allocator itself works by briefly allowing smaller rungs (it took 0.24 s); that's useless
(1 bar @120 BPM = 2 s), so the fallback is now **floored at 1 s stereo / 2 s mono** — below
that the loop simply doesn't exist and the app hides the card rather than showing one with
every control greyed out. **Audio looping needs PSRAM** (jay-mint 8 MB → 8 s × 2 loops).
The device boots and runs normally with the new graph (`"clock":{"run":1,...}`), and the
inserted final mix is a unity pass-through by construction (`AudioMixer4_F32` defaults all
4 gains to 1.0), so the audio path is preserved — **still worth confirming by ear**.

### To make it work WITHOUT PSRAM (phase 2, pick one)
- **SD streaming** (already the designed tier-3) — long loops, any board.
- **Lower the loop store rate** (e.g. 16 kHz mono ≈ 32 KB/s) — lo-fi but a 2-bar loop fits.
- **ADPCM 4:1 compression** in the buffer — ~4× the length for modest CPU.

## Prototype now implements (all GREEN)
- Stereo **and mono** storage; **clock-follow resample** (fractional read + linear interp,
  crossfade-aware); **WAV save** to SD. Rebuilt clean with the WAV/SD path linked.
- Still design-only: **N-loop wiring**, **selectable-source mux**, and the mix-kit `@AL*`/
  app-card integration (§6–7) — deliberately not wired in (mix-kit is mid-WIP).
