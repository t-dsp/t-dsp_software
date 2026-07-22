# SD drum sampler v2 — loop-seam fix, @DRUMRESCAN (excellence pass)

Local board (no-PSRAM, COM4) now plays Mars kits via `DrumSampler.h` (`TDSP_DRUM_SD`). Two
improvements, both in `firmware/mix-kit/src/DrumSampler.h` + `main.cpp`. Keep the streaming path
as the always-safe baseline — every new bit must fall back to it, never crash.

## 1. `@DRUMRESCAN` — reload /drums without a reflash (SAFE, do first)
Today `scanKits()` runs only in `drumSamplerBegin()` at boot, so newly-pushed kits need a reboot.
Add a serial/BLE command `@DRUMRESCAN`:
- calls `g_drumSamplerSink.scanKits()`, then `setKit(currentKit clamped)`, updates
  `g_engineHasDrums = (numKits()>0)`, and rebuilds the catalog (`buildCatalog(engineCaps(),
  catdbWriteBundled, millis())`) so the app's kit list refreshes; reply `@DRUMRESCANNED\t<n>`.
- Wire only under `#if defined(TDSP_DRUM_SD)`; other drum engines ignore it. Put the handler next
  to `@REINDEX`/`@DRUMKIT` in main.cpp's control-line dispatch.

## 2. Loop-seam hesitation fix — RAM-cache hot one-shots (gated `TDSP_DRUM_SD_CACHE`)
**Cause:** on a groove's loop downbeat several hits fire at once; each `AudioPlaySdResmp::playWav()`
does a synchronous SD **file-open + first-buffer read** (FAT lookup, several ms each) in the tick
handler → cumulative stall → audible hesitation at the seam. Streaming is fine mid-loop; it's the
dense simultaneous OPEN at the wrap that stalls.

**Fix:** preload the frequently-struck SHORT one-shots into RAM and play them with **no SD open**,
via `AudioPlayArrayResmp` (teensy-variable-playback; plays an int16 array, `playWav(int16_t*,len)` /
`playRaw(int16_t*,len,channels)`, same `AudioPlayResmp` base + stereo outputs as `AudioPlaySdResmp`).

Design (all gated `#if defined(TDSP_DRUM_SD_CACHE)`, default ON in the env; building WITHOUT the
flag must give the exact current streaming behavior — the baseline I flash first):
- Add a small pool: `AudioPlayArrayResmp g_drumCacheV[TDSP_DRUM_SD_CACHE_VOICES]` (default 4).
- Widen the mixer tree to take the extra voices: stage-1 becomes 3 `AudioMixer4` per channel
  (SD 0-3, SD 4-7, cache 0-3); stage-2 one `AudioMixer4` sums the 3. Keep gains 1.0; slot-2 make-up
  unchanged. Update the AudioConnection block accordingly (still all fixed/valid — remember the v1
  crash: object arrays + connections must match their declared sizes exactly).
- Cache budget: `TDSP_DRUM_SD_CACHE_KB` (default 96). At `setKit()`, AFTER building `path[]`, free any
  previous cache, then for the priority notes IN ORDER {36,35,38,40,42,44,39,37} load each WAV's PCM
  into a `malloc`'d int16 buffer (parse the WAV: skip to `data` chunk, keep 16-bit stereo frames,
  store frame count + channels) while the running total ≤ budget and the file's data ≤ a per-sample
  cap (e.g. 48 KB, ~0.25 s stereo). Store `{note -> buf, frames, ch}`. On `malloc` fail or oversize,
  just skip caching that note (it streams — no crash).
- Trigger (`onNoteOn`): if the note is cached → play on a cache voice (`playWav`/`playRaw` from RAM,
  rate 1.0, interp off); else → the existing SD-stream path. Voice-pick within each pool.
- **Hi-hat choke must span BOTH pools**: track `_voiceNote` for cache voices too and, when a hat
  (42/44/46) fires, stop any ringing hat in EITHER pool. `onAllNotesOff`/`panic` stop both pools.
- Free cache buffers on `setKit()` and don't leak across kit switches.

## Constraints
- No-PSRAM: `malloc` comes from the shared heap that also feeds the streaming ring buffers
  (~28 KB/sounding SD voice). 96 KB cache + streaming must coexist — keep the cap conservative; a
  failed alloc must degrade to streaming, never crash.
- Match the v1 lesson: audio object arrays and their `AudioConnection`s are FIXED-SIZE and must
  agree exactly; never index an object array past its declared length.
- Green-build (report each): `teensy41_dexed_pool_nobt_drumsd` WITH and WITHOUT `-D
  TDSP_DRUM_SD_CACHE` (both must pass), plus `teensy41_opll`. Do NOT flash. No git.
