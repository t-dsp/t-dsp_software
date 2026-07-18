# Handoff — TSF drums stutter / drift on jay-mint (work from the known-good image)

> ⚠️ RECONSTRUCTED 2026-07-18: this file was an untracked working-tree doc that was lost during a
> multi-branch merge/checkpoint operation (never committed, so unrecoverable from git). The content
> below was restored from a captured copy that ended mid-sentence at the "Flash cycle" bullet — the
> tail after that point is LOST. Whoever holds the original (the drum-stutter session) should paste
> the remainder back and commit this file so it can't vanish again.

## The known-good image (START HERE)
Everything below builds on the **verified-green snapshot** on the remote:
- branch `tracks-phase3-voices` @ `31e11cd`
- immutable tag `tracks-snapshot-2026-07-17-p4d-green` (same commit)
- backup branch `origin/backup/tracks-p4d-2026-07-17`

Restore/branch from it, never force-push over it:
```bash
git fetch --tags origin
git switch -c drum-smooth tracks-snapshot-2026-07-17-p4d-green   # work here
# if you ever need to recover: git reset --hard tracks-snapshot-2026-07-17-p4d-green
```
This image is 4 envs green (opll, dexed_pool_nobt_voice2, dexed2_opll1, and the drum env below) and
flash-verified on local COM4 + jay-mint.

## The problem
On **jay-mint** running `teensy41_dexed_pool_jaymint_voice2_tsfdrums_serial` (2 Dexed MPE voices + a
resident **TSF sampled GM-drum** engine on mix slot 2 + BT A2DP), the drum groove **stutters and
sometimes drifts off the beat**. The user hears it; we quantify it over serial.

## Diagnosis (most-likely first) — MEASURE before changing anything
1. **Audio-ISR CPU overrun (prime suspect).** TSF renders up to `tsf_set_max_voices(24)` sampled
   voices (`src/DrumTsf.h`), the Dexed pool renders 8 FM engines, and the BT async resampler
   (`lib/TDspAsyncI2S`, `MAX_FILTER_SAMPLES=5121` here) all run in the audio update. On a busy groove
   the drum voices stack → the audio update overruns its block budget → blocks drop (**stutter**), and
   because the master `Conductor` clock advances off the audio block count, an overrun also **jitters
   the beat** (drift). The firmware prints the evidence once/second on the USB serial:
   `alive up=… cpuMax=XX.X% memMax=YY` (main.cpp ~L3697, NOT diagnostics-gated). **If cpuMax rides
   near/at 100% while drums play, this is it.** `memMax` near the pool caps (`AudioMemory(80)` /
   `AudioMemory_F32(60)`, main.cpp ~L3230) = block starvation, the same symptom.
2. **BT resampler load.** A2DP's async resampler is heavy. Test with BT idle/disconnected — if the
   drums smooth out, the resampler is the load (lower oversampling further, or accept lower BT quality).
3. **loop() stall starving the groove tick.** `g_drumPlayer.tick()` runs in `loop()` (main.cpp ~L3417/
   3461); a long blocking op in loop() delays it → hitches. SD streams are already covered by
   `pumpTransport()` (commit `1c1bc36`), but a chatty app (frequent `@STATE`/browse) or a slow serial
   op could still stall it. Less likely for *steady* stutter, more for occasional hitches.
4. **Groove density / note map.** Dense grooves + the `DrumNoteMapper` fanning notes to many samples
   tax TSF more. A sparse groove that's smooth while a dense one stutters points back to (1).

## How to test it on jay-mint
jay-mint runs this build already. Drive it over serial from the box (SSH `jay@10.0.0.239`, pass `mint`,
`/dev/ttyACM0`; mDNS `.local` is flaky → use the IP). The USER listens; you correlate with the numbers.
```bash
# open the port, start the clock + a groove, then WATCH the heartbeat + drift probe:
#   @METRO=1            start the master transport (the metronome IS the clock)
#   @DRUMF=<groove>     play a groove by name (bar-quantized); pick a DENSE one to provoke it
#   @DRUMKIT=<i>        (optional) choose the GM kit
#   @SYNCPROBE=1        1 Hz print of master-beat vs each synced player's cursor (relative phase must
#                       stay CONSTANT for a drift-free lock; a walking delta = drifting)
# then read the once/second "alive … cpuMax=XX% memMax=YY" line while it plays.
```
- **cpuMax** is the headline number. Reset each second, so it's a rolling peak — watch it under a busy
  groove + both Dexed voices playing + BT streaming (worst case).
- **@SYNCPROBE** quantifies the "off beat": if `drum cur` vs `master` drift apart over a minute, the
  groove is losing lock (usually a downstream symptom of overrun, not the cause).
- Optionally `@CAP=<n>` grabs DAC-bound samples to the PC for `tools/capture_analyze.py` (offline
  dropout/FFT), but note @CAP is stubbed to 64 samples on lean-RAM builds — cpuMax is the reliable tool.
- **Flash cycle (touch-free now — the board runs good firmware):** stream `firmware/mix-kit/src` +

<!-- ⚠️ CAPTURED COPY ENDED HERE — the remainder of the original doc (rest of the flash-cycle steps
     and anything after) was lost. Restore from the drum-stutter session's copy and commit. -->
