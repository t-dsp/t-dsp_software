# TDspMPE buildout roadmap

## Where to do the work

**Branch:** `phase-2d-mpe-va` (this worktree at `../t-dsp_software-phase-2d/`).

Phase 2d (already shipped on this branch) gave us a 4-voice MPE-aware
virtual-analog engine: one oscillator + one amp envelope per voice,
per-channel pitch bend / pressure, LRU stealing. Enough to prove the
architecture; not yet enough to sound modern.

This doc lays out the follow-on phases. Each phase is a standalone
merge target — small enough to review in one sitting, big enough to
produce a tangible audible improvement.

## Phase 2d2 — filter + CC#74 timbre routing (NEXT)

Lib: per-voice `AudioFilterStateVariable` inserted between osc and
envelope. MpeVaSink grows:

- `_filters[kMaxVoices]` array of pointers, passed in via `VoicePorts`.
- `_cutoffHz`, `_resonance`, `_envAmount` (shared across voices for MVP).
- `onTimbre(channel, value)` — routes CC#74 to per-voice filter cutoff
  multiplier, so a LinnStrument's vertical finger movement opens /
  closes the filter on that *specific* note only.
- Cutoff calc per voice: `base_cutoff_hz * (0.25 + 1.5 * timbre)`, with
  additional env amount if configured.

OSC:
- `/synth/mpe/filter/cutoff f` — 20..20000 Hz (log taper on the UI side)
- `/synth/mpe/filter/resonance f` — 0.7..5.0 (self-oscillates past ~4)
- `/synth/mpe/filter/env f` — -1..+1 signed envelope-to-cutoff amount

Main.cpp: 4 more `AudioFilterStateVariable` instances, 4 `AudioConnection`s
between osc and env, plus env-control routing (AudioFilter SVF has a
3rd input for cutoff modulation — wire the envelope's post-amp tap
through a scaler into that if we want envelope-to-cutoff).

`AudioMemory` +~8 blocks → 168.

**Unlocks:** plucks, acid basses, anything with filter sweep. Biggest
single jump in "does this sound modern?" per line of code.

## Phase 2d3 — LFO per voice (wobble prerequisite)

Lib: one LFO per voice (`AudioSynthWaveformDc` or a slow-rate
`AudioSynthWaveform` — TBD based on destination routing). Assignable
destination: cutoff / pitch / amp. Global rate + depth for MVP; later
per-voice if MPE pressure can modulate LFO depth.

OSC:
- `/synth/mpe/lfo/rate f` — 0.1..20 Hz
- `/synth/mpe/lfo/depth f` — 0..1
- `/synth/mpe/lfo/dest i` — 0=cutoff, 1=pitch, 2=amp
- `/synth/mpe/lfo/waveform i` — sine/tri/saw/square
- Later: `/synth/mpe/lfo/sync i` once a MIDI clock source exists

`AudioMemory` +~6 blocks → 174.

**Unlocks:** dubstep wobble (LFO→cutoff), vibrato (LFO→pitch), tremolo
(LFO→amp). With Phase 2d2's filter this is where the synth starts to
sound genuinely expressive.

## Phase 2e — web UI (moves in parallel with 2d2/2d3)

Different worktree. New file
`projects/t-dsp_tac5212_audio_shield_adaptor/tools/web_dev_surface/src/ui/synth-mpe-panel.ts`:

- 4 live voice orbs (pitch → Y, channel → hue, pressure → radius, trails)
- Waveform picker (4 canvas-drawn tiles)
- A / R / filter sliders with envelope preview
- Master channel picker (16 pills + crown on active)
- Preset grid (3×3, categories: retro / emulating / abstract)
- Auto-preview: clicking a preset fires `/midi/note/in 60 100 2` for 400ms

Supporting firmware: subscription-gated `/synth/mpe/voices` broadcast
(4 voices × {held, ch, note, bend, pressure}) at ~30 Hz. That's Phase
2e's one firmware change — everything else is UI.

## Phase 2f — unison / detune + drums

**Unison** extends each MPE voice from 1 osc to 2-3 stacked saws with
cents-detune. `AudioMemory` grows significantly (+16 blocks per voice);
may need to drop `kVoiceCount` from 4 to 3 when unison is active.

- `/synth/mpe/unison/voices i` — 1, 2, 3
- `/synth/mpe/unison/detune f` — 0..50 cents

**Drums** — new `lib/TDspDrums/` sink. Two flavors:
- `TDspDrumSampler`: `AudioPlayMemory` × 8, samples in PROGMEM, triggered
  by MIDI notes 36..43 (GM drum map). Amen break as a single sample for
  instant DnB.
- `TDspDrumSynth`: programmatic kick (sine + fast pitch env), snare
  (noise + bandpass), hats (noise + HPF + short env). Mono bus into
  preMix slot 3 (reserved).

Both register as `tdsp::MidiSink` listening on channel 10 (GM convention).

## Phase 2g — shared FX bus (production polish)

New chain on a dedicated preMix slot: bitcrush → waveshaper → delay →
reverb. Each stage has wet/dry + params. One instance, shared across
all synths.

- `/fx/bitcrush/{mix,bits,rate}`
- `/fx/drive/{mix,amount,type}`
- `/fx/delay/{mix,time_ms,feedback,pingpong}`
- `/fx/reverb/{mix,size,damping}`

`AudioMemory` +~30 blocks. Biggest single memory jump; worth watching
`AudioMemoryUsageMax()` after this lands.

## Phase 2h — arp + sidechain + risers

Mature production tools:
- `lib/TDspArp/` — registers as a MidiSource, latches held-chord notes
  and emits rhythmic note-on/off sequences into the router. Modes:
  up/down/updown/random/played. Needs a tempo source (USB MIDI clock).
- **Sidechain ducking** — envelope follower on the drum bus → inverse
  gain on the MPE/Dexed bus. `/fx/sidechain/{src,target,depth,release}`.
- **Noise risers** — `AudioSynthNoisePink` → sweeping filter → reverb,
  triggered by `/fx/riser/trigger`.

## Phase 2i — wavetable oscillator (stretch)

Replace the VA oscillator with `AudioSynthWaveformModulated` + arbitrary
256-sample tables. Morph position between tables as a controllable
parameter, modulatable by LFO / pressure / CC.

- `/synth/mpe/wave/table i` — preset table index
- `/synth/mpe/wave/position f` — morph 0..1
- Table set: saw stack, vocal formants, bells, distorted square, etc.

This is where "TDspMPE" graduates into a proper modern synth. Biggest
design question is the table data pipeline: PROGMEM constants vs.
runtime upload via OSC vs. SD card load.

## Budget

Teensy 4.1 @ 600 MHz:
- Current (Phase 2d): ~30% CPU, 160 AudioMemory blocks
- Through Phase 2g: ~60% CPU, ~250 AudioMemory blocks
- Through Phase 2i: ~75% CPU, ~300 AudioMemory blocks

Fan-out is the pool-pressure driver, not CPU. Monitor
`AudioMemoryUsageMax()` via the 's' CLI after each phase.

## The minimum viable modern synth

If we stop at Phase 2g we have:
- Expressive MPE leads/basses with filter + LFO
- Drums (programmed or sampled)
- Usable FX chain

That's enough to finish an actual track on this hardware. Phase 2h+ is
polish on a system that's already useful.

## Merge order considerations

Phase 2d lives in this worktree. Phase 2d2 and 2d3 stack on top of it.
Phase 2e needs Phase 2d2 at minimum (orbs look boring without filter
reacting to pressure) but doesn't *block* on it. Phase 2f, 2g, 2h, 2i
are independent add-ons that can land in any order after Phase 2e.

When Phase 2c lands on master, rebase this branch onto it — the Phase
2c overlap hunks drop cleanly. Each subsequent phase should commit as
its own merge-ready branch so review stays focused.
