# Audio Looper — STATUS: in progress, UNTESTED

**Do not assume this works.** The audio looper (record the device's digital audio into
looping audio, the sibling of the MIDI loop recorder) is wired end-to-end but has **not
been verified by ear on hardware**.

What's in the tree:
- `lib/TDspAudioLoop` — `tdsp::AudioLooper` (stereo/mono `AudioStream_F32`, bar-locked,
  overlap-crossfade seam, overdub, clock-follow) + `AudioLoopWav.h` (SD `.wav` save).
- mix-kit firmware behind `TDSP_AUDIOLOOP` — record-bus → loops → final-mix graph,
  `@AL*` commands, `@STATE "aloop"` + `caps.audioloop`, live `@ALP=` telemetry.
- App "Audio Loop" card (gated on `caps.audioloop`).

Both firmware envs build green and the app typechecks; it flashes and boots fine (audio
graph unaffected — the inserted final mix is a unity pass-through by construction). But:

- **No on-hardware audio confirmation** — nobody has heard a loop record/play/overdub yet.
- **Needs PSRAM to be usable.** At 48 kHz stereo a 1-second loop is 192 KB. No-PSRAM
  boards (incl. the lean COM4 dev unit) can't spare it, so `audioLoopSetup()` allocates
  **zero** loops → `caps.audioloop = 0` → the card hides. Confirmed on hardware:
  `@STATE` reports `"aloop":{"n":0,"cap":0}`. Real capacity needs PSRAM (jay-mint 8 MB).

## Future work (planned, not started)

To make the audio looper usable **without PSRAM**, build BOTH:

1. **SD streaming** — record/play the loop as a `.wav` streamed to/from the SD card
   (double-buffered, prebuffered, crossfaded at the seam). Long loops on any board.
2. **Lower loop store rate** — capture into RAM at a reduced sample rate (e.g. ~16 kHz
   mono ≈ 32 KB/s) so a musical loop fits in OCRAM. Lo-fi but works with no PSRAM.

See `DESIGN.md` (architecture, tiers, DSP) and `HANDOFF.md` (what's done + the hardware
finding) in this folder for detail.
