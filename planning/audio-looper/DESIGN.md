# Audio Looper — Design

Status: **design + prototype** (2026-07-16). Author: agent, overnight design run.
Goal: capture the device's actual digital audio output into a looper and loop it
smoothly — the audio-domain sibling of the home-page **MIDI** loop recorder
(`lib/TDspMidiLoop` / `@REC*` / the app "Loop Recorder" card). Efficient, fast,
click-free seams. Capability scales with build flags + PSRAM.

This document is self-contained: it records what exists, the constraints, the
recommended architecture, the DSP for smooth loops, the firmware/app integration,
build tiers, and a test plan. A working prototype library
(`lib/TDspAudioLoop`, `tdsp::AudioLooper`) + a green-building spike
(`projects/spike_audio_looper`) accompany it.

---

## 1. What already exists (don't reinvent)

- **`tdsp::Looper`** (`lib/TDspLooper`) — a mono **int16** `AudioStream` looper:
  caller-owned buffer, ISR-safe transport (`record/play/stop/clear`), **beat-snap**
  `play(snapSamples,minSamples)` (rounds recorded length to a whole beat, zero-pads),
  and **clock-follow** `setPlaybackRate()` (linear-interp resample). **Gaps:** mono,
  int16, **hard-cut seam (no crossfade)**, beat-snap only (no bar-lock / downbeat
  anchor), no overdub, no internal `Clock`. Used by
  `projects/t-dsp_tac5212_audio_shield_adaptor` (a full working looper integration:
  source mux, `looperSamplesPerBeat()`, beat-edge arm, OSC surface). **We keep it
  untouched** (that project depends on it) and build a new, richer class.
- **Mix graph** (`firmware/mix-kit/src/main.cpp`): **48 kHz, block 128, F32** mix bus.
  `AudioMixer4_F32 outL/outR` sum slots 0=BT,1=tone/metro,2=S/PDIF-in,3=synth →
  `AudioOutputTDM_F32 tdmOut` → TAC5212. The app master volume is applied *inside*
  `tdmOut.setGain()`, i.e. **after** `outL/outR`. `@CAP`'s `OutCaptureProbe_F32`
  (a 1-in `AudioStream_F32` tapping `outL`) is the exact tap template.
- **Master clock** (`tdsp::Conductor`/`tdsp::Clock`): `positionBeats()` (tear-free
  double), `beatsPerBar()`, `bpm()`, `consumeBeatEdge()`, and `ITempoFollower::onBarEdge()`
  (the `LaunchScheduler` in main.cpp is the "fire on next bar" template). Samples/beat
  = `AUDIO_SAMPLE_RATE_EXACT * 60 / bpm`.
- **Record/play building blocks:** `AudioRecordQueue(_F32)`, `AudioPlayQueue(_F32)`,
  `AudioPlaySdWav` (stereo int16 SD playback), `AudioSDPlayer_F32` (stereo F32 SD
  playback), `WaveHeaderParser` (read). **No** `AudioRecordWAV` / WAV *writer* exists —
  SD persistence needs a hand-written 44-byte header. `@WB`/`tdsp::SdWriteReceiver`
  is the host↔device file path (~1–3 MB/s USB CDC).
- **UX to mirror:** `@RECV/@RECBARS/@RECSIG/@REC/@RECDUB/@RECCLR/@RECPLAY`, the
  `@STATE "rec"` block + `caps.rec`, live `@RECP=` telemetry, and the App.tsx
  "Loop Recorder" card (bars 1/2/4/8 pills, N/4 sig pills, ●/⊕/■ transport).

## 2. Constraints that shape everything: memory

RAM1/DTCM has **no** headroom (buffers are deliberately kept out of it). Storage @48 kHz:

| format | bytes/s | 1 s | note |
|---|---|---|---|
| mono int16 | 96 KB | 96 KB | |
| **stereo int16** | **192 KB** | 192 KB | **chosen storage format** |
| stereo F32 | 384 KB | 384 KB | mix-bus native, 2× the RAM |

- **RAM1/DTCM:** unusable for the buffer.
- **OCRAM/DMAMEM (~512 KB, shared with heap):** tight; on lean pool builds it's
  reserved for the SD-song heap. Realistic spare ≈ 96–192 KB → **~0.5–1 s stereo**.
- **PSRAM (EXTMEM):** only jay-mint (8 MB) has it → **~40 s stereo int16**. Auto-detected
  via `external_psram_size` (MB). Default/COM4 dev boards have **0 PSRAM**.
- **SD:** on every Teensy 4.1; the only place long loops fit on no-PSRAM boards.

**Conclusion:** store **stereo int16**, in a **caller-owned buffer** whose size and
placement are picked at init from `external_psram_size` (EXTMEM if present, else a
small DMAMEM budget). Offer an **SD-streamed** backend (phase 2) for long loops on
no-PSRAM boards. int16 (not F32) halves RAM and is inaudible for loop material.

## 3. Architecture

### 3.1 The node — `tdsp::AudioLooper : public AudioStream_F32`

A **stereo** (2-in / 2-out) F32 audio node that stores **int16** internally, mirroring
`MidiLooper`'s clock-integrated shape so firmware/app integration is nearly identical:

```
begin(Clock*, int16_t* buf, uint32_t frames)   // frames = stereo sample-pairs
setBars(1|2|4|8)                                // loop length in bars (× beatsPerBar)
armRecord()  armOverdub()  stop()  clear()  resume()
poll()                                          // foreground: bar-quantized arm/finalize
State: Idle Armed Recording Overdub Playing     // same codes as MidiLooper (0..4)
positionPermille()  state()  bars()  hasClip()
setReturnLevel(float)  setClockFollow(bool)
```

- **update()** (audio ISR): reads the two input blocks (the record bus, see §3.2),
  writes/sums into the buffer when recording/overdubbing, and emits the loop
  (with crossfade, §4) when playing. int16↔F32 convert at the boundary.
- **poll()** (foreground `loop()`): consumes the clock's bar edge to start recording
  exactly on a **downbeat**, finalizes the loop length after `bars×beatsPerBar` beats,
  and flips to Playing. Transport methods are called from the command handler and
  guard multi-field writes with `__disable_irq/__enable_irq` (as `tdsp::Looper` does).

### 3.2 Where to tap — avoid the feedback loop

The looper's **playback is summed back into the master**, so tapping the *post-loop*
master would record the loop itself → runaway feedback on overdub. Insert a **record
bus** the loop return is NOT part of:

```
 BT ┐
tone├─ outL/outR (AudioMixer4_F32)      ← RECORD BUS (tap here; = everything the user makes)
SPDIF│      │
synth┘      ├──────────────► finalL/finalR (new AudioMixer4_F32) ──► tdmOut ──► DAC
            │                   ▲
         AudioLooper ───────────┘  (loop return, slot 1 of final mix)
            ▲  ▲
       outL/outR (record-bus L/R)   ← looper's 2 inputs
```

- Keep today's `outL/outR` as the **record bus**; tap it into the looper's inputs.
- Add **`finalL/finalR`** (one `AudioMixer4_F32` pair): slot 0 = record bus, slot 1 =
  loop return; `finalL/R → tdmOut`. The app master fader (`tdmOut.setGain`) is unchanged.
- **First record:** buffer = record bus. **Overdub:** buffer **+=** record bus (the old
  layers are already in the buffer; the loop return is excluded from the tap, so no
  feedback). This is the correct, click-free overdub model.
- Costs: 2 `AudioMixer4_F32` nodes + a couple of `AudioConnection_F32`. `@CAP` keeps
  tapping `outL` (now the record bus — fine).

Slot availability: `finalL/finalR` are new mixers, so no slot pressure. (The old idea
of reusing mixer slot 2 would fight S/PDIF-in and can't express "exclude loop from the
record tap" — the sub-mixer is the correct structure.)

### 3.3 Reuse vs build

Build **new** (`lib/TDspAudioLoop`), don't extend `tdsp::Looper` (would break the
adaptor project and it lacks stereo/crossfade/overdub/bars). Reuse its **proven
patterns** (ISR-safe transport, beat math, clock-follow) as reference. This keeps the
firmware/app wiring a near-copy of the MIDI looper for UX consistency.

## 4. Smooth seams — the core DSP

`tdsp::Looper` hard-cuts at the wrap → clicks on sustained material. The new looper
uses **overlap-crossfade**, the pro-looper technique:

1. **Record `loopFrames + XFADE` frames** (XFADE ≈ 256–512 frames ≈ 5–11 ms). The extra
   tail is the real audio that continued *past* the loop point.
2. **On playback**, for the last `XFADE` frames of each loop iteration, output
   `tail[k]·fadeOut(k) + head[k]·fadeIn(k)`, where `tail` = `buf[loopFrames..+XFADE)`,
   `head` = `buf[0..XFADE)`, and the ramps are **equal-power** (`cos²/sin²`, precomputed
   table). The decaying tail blends into the head → no discontinuity in level *or* slope.
3. **Record-in/out ramps:** apply a ~2 ms fade at the very start/end of a fresh capture
   so arming/disarming can't latch a step. **DC-block** the record bus (one-pole HPF,
   ~5 Hz) so any DC offset doesn't accumulate on overdub.
4. **Bar-locked length** keeps the seam on a musical boundary in the first place;
   crossfade covers the sub-sample/rounding mismatch and any non-silent seam.

Loop length: `loopFrames = round(bars × beatsPerBar × SR × 60 / bpm)` sampled at
record-start (sample-accurate length). The loop then free-runs at `loopFrames`;
**clock-follow** (`setPlaybackRate(cur/recordedBpm)`, linear interp) keeps it tracking
tempo changes (pitch shifts with tempo — document the trade-off; optional off).
For tight long-term lock to the MIDI grid, phase 2 can re-anchor to `positionBeats()`
each wrap (±1 sample, hidden by the crossfade).

## 5. Storage tiers (build-flag / PSRAM adaptive)

Buffer is caller-owned; the firmware picks size + placement at init:

| tier | condition | placement | capacity (stereo int16) |
|---|---|---|---|
| **RAM-small** | no PSRAM (COM4/default) | `DMAMEM` static, modest | ~0.5–1 s (few-bar stutter/rhythm) |
| **RAM-large** | `external_psram_size > 0` (jay-mint) | `EXTMEM` | ~40 s @ 8 MB |
| **SD-stream** (phase 2) | any board, long loops | SD `.wav`, double-buffered | minutes; needs prebuffer + the crossfade |

- `TDSP_AUDIOLOOP` gates the whole feature (like `TDSP_RECORDER`).
- `TDSP_AUDIOLOOP_FRAMES` (or runtime from `external_psram_size`) sets RAM capacity.
- The app hides the card unless `caps.audioloop`; it reports capacity so bars that
  don't fit are disabled (e.g. 8 bars @ 120 BPM 4/4 = 16 s needs PSRAM).

## 6. Firmware integration (mirror the MIDI looper)

- Objects: `tdsp::AudioLooper g_aloop;` + the record-bus/final-mix rewiring (§3.2),
  `g_aloop.begin(&g_conductor.clock(), g_aloopBuf, kAloopFrames)` in setup, `g_aloop.poll()`
  in `loop()`.
- Commands (new `@AL*` namespace so they don't collide with MIDI `@REC*`):
  `@ALBARS=`, `@ALSIG=` (→ `applyMeter`), `@AL=1|0` (arm replace / stop),
  `@ALDUB=1|0` (overdub), `@ALCLR`, `@ALPLAY=1|0` (resume), optional `@ALFOLLOW=`,
  `@ALLEVEL=`, `@ALSAVE=<name>` (phase 2, WAV to SD). Each echoes back.
- `@STATE`: add `"aloop":{"st","bars","p","cap_s"}` and a `caps.audioloop` bit.
- Live telemetry: `@ALP=<st>,<p>` (mirror `@RECP`, USB-only push ~4×/sec while active).
- Count-in: reuse `recArmTransport`-style click + `ensureTransportStarted()`; arm fires
  the record start on the next bar downbeat (audio has no "first note" — quantize to the
  bar). Auto-stop the click when it flips to Playing (same `recPollClick` pattern).

## 7. App integration

Clone the "Loop Recorder" card as an **"Audio Loop"** card, `show: caps.audioloop`,
distinct accent. Same primitives: bars 1/2/4/8 pills (disable those exceeding capacity),
N/4 sig pills, ●/⊕/■ transport + Clear, live permille bar from `@ALP`. Transport methods
`audioLoopBars/Sig/Arm/Overdub/Play/Clear` → the `@AL*` lines, in all three transport
files. `caps.audioloop` + an `aloop` state + hydrate + `@ALP` parse, exactly like `rec`.

## 8. Test plan (green-buildable now; audio needs hardware)

1. **Compile:** the prototype builds via `projects/spike_audio_looper` (teensy41).
   Later, `teensy41_opll` + `teensy41_dexed_pool_nobt_voice2` once wired into mix-kit.
2. **Off-target DSP unit test** (host g++, no hardware): feed a synthetic ramp/sine
   through the crossfade + bar-length math with a fake clock; assert loopFrames, the
   equal-power ramp sums to unity power, and the seam has no sample-value/slope step
   above threshold. (This is the real, hardware-free correctness check.)
3. **On hardware (manual):** play the synth, arm on a bar, confirm the loop captures
   N bars, loops click-free, overdub layers, tempo-follow tracks BPM, and PSRAM vs
   no-PSRAM capacity behaves. Flash caveat: COM4 held → press PROGRAM.
4. **Feedback check:** verify overdub does not run away (tap excludes loop return).

## 9. Decisions (locked 2026-07-17)

1. **N independent loops.** Each is its own `tdsp::AudioLooper` instance + buffer,
   armed/played independently — a multi-track audio looper. Capacity is the gate:
   N × (bars × …) must fit RAM, so meaningful N needs PSRAM; the no-PSRAM board gets
   a small N (or short loops). Firmware owns an array of loopers; `@AL*` commands carry a
   loop index (like `@RECV` picks a voice). The final mix sums all loop returns.
2. **Mono toggle.** `setMono(true)` stores one channel (sum L+R on record, duplicate to
   L+R on play) → **halves RAM** so no-PSRAM boards get 2× the loop length. Per-loop.
3. **Follows the shared master clock.** The looper reads the same `tdsp::Conductor` clock
   the MIDI player, arpeggiator, metronome, and drums follow — so tempo/meter are global.
   Because those followers are tempo-elastic, the audio loop **tracks tempo** too via
   clock-follow resample (`rate = curBpm / recordedBpm`, linear interp) — pitch shifts
   with tempo (DJ-style). Default **on**; `@ALFOLLOW=0` frees a loop to free-run at pitch.
4. **SD `.wav` save in v1.** `@ALSAVE=<idx>\t<name>` writes `/loops/<name>.wav` (16-bit
   PCM, the loop body). Needs a hand-written 44-byte header (no WAV writer exists in-repo)
   — provided in `lib/TDspAudioLoop` (`AudioLoopWav.h`). Load via `AudioPlaySdWav` /
   `AudioSDPlayer_F32` (phase-2 "load take into a loop").
5. **Selectable record source, default = main.** Per-loop source select like the
   adaptor's `loopSrc` mux: **main** (full master mix) is the default/first target; later
   options = synth-only, BT-only, etc. Firmware routes the chosen source into that loop's
   two inputs; the record-bus/loop-return split (§3.2) still applies to prevent feedback.

### Still deferred to phase 2
- Loading a saved `.wav` back into a loop slot; sample-accurate downbeat start (v1 is
  block-granular ±2.7 ms, hidden by the crossfade); antialiased resample at extreme rates.
