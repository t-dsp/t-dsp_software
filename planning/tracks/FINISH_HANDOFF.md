# Tracks — finish-the-project handoff / next-agent prompt

You are completing the **Tracks refactor** for the T-DSP box (Teensy 4.1 mix-kit firmware + an
Expo/React-Native control app + an ESP32 BT/BLE/WiFi front-end). Read this whole doc, then
`planning/tracks/DESIGN.md` and the `project_tracks_refactor` memory. Phases 0–4-minimal + the
runtime-repartition feature are DONE and **merged + pushed to master (`08e5c61`)**; this doc is the
punch-list to finish the DESIGN's vision. **GO top-to-bottom; each item is independently shippable.**

## 0. What "done" means
The DESIGN's endgame: **N configurable Track slots, each bindable to any engine kind**, drums are
just-another-Track, every track has a real mixer strip, engine allocation is reconfigurable without a
reflash, and every input (song / looper / live DIN / USB-host / **BT** / **serial**) flows through the
one uniform Track transport. Today the skeleton + a heterogeneous proof + a full 4-voice build + live
engine repartition all exist. "Finish" = the threads in §3, gated so nothing regresses.

## 1. Current state — master `08e5c61` (local + origin, pushed)
- **Phase 3** — 4 independent Dexed voices (Thread A, fixed pool split); data-driven app cards (Thread B);
  the MIDI-input **subscription hub** (Thread C: `midihub::` in main.cpp, per-Track `{liveSrcMask,srcChMask}`,
  `@TRK<i>.SRC/SRCCH`, zero-repatch source switching); tempo auto-follow; drum-note-map (`DrumNoteMapper`);
  the `@TRK<i>.*` uniform command family.
- **Phase 4 Thread D (minimal)** — heterogeneous engine inventory. `HeteroOpll.h` = a melodic OPLL voice
  ALONGSIDE the Dexed pool (sibling of `DrumVoice.h`); env `teensy41_dexed2_opll1` = 2 Dexed + 1 OPLL (track 2).
  Slot abstraction: count flags `TDSP_HETERO/DEXED_VOICES/OPLL_ENGINES`, `kDexedVoices` splits Dexed/OPLL
  voices, `voiceIsOpll()` dispatch, engine-agnostic `@TRK<i>.INSTR=`, top-level `@STATE engines:{dexed,opll}`.
  All count-flag gated → non-hetero builds byte-identical. Serial-verified engine isolation (track 2 audio =
  pure OPLL, Dexed sum = 0).
- **3 hardware-verified fixes:** hetero/4-voice Dexed split is **PERMANENT** (retire `@VOICE2` toggle — fixed
  A/B voice-stealing); an experimental env needs **`TDSP_METRONOME`** (= the master transport `@METRO` — fixed
  dead master-stop/metronome + the loop-clock "stutter"); **master stop clears EVERY voice** ≥2 (fixed stuck
  OPLL / 4-voice voices 2/3). App: the vestigial **Synth B enable switch is removed**.
- **Runtime pool partition (`@POOL`, no reflash)** — the 8-engine Dexed pool is redistributed among the 4 fixed
  voices at runtime: `@POOL=0` 4 voices (2 eng), `=1` 2 voices (4 eng), `=2` 1 voice (8 eng = 8-note MPE/16
  poly), `=3` 4+2+2. Each group's LEAD grows its `DexedPoolSink` engine count from its fixed base; absorbed
  voices go idle (panicked, buses mirror the lead's level+trim, gated off live MIDI via `voiceLive()` +
  `@TRK.PLAY`→`IDLE`). `@STATE pool:{preset,engines[],active[]}`. App: a **"Voice Pool" card** with the 4
  presets (`tp.poolPreset()` on all transports). All `#if TDSP_SYNTH_VOICES>=4`; voice2/drumvoice byte-identical.
  Serial-verified all 4 presets on COM4.
- **Deployed / verified:** local board **SN 18402920, COM4** runs `teensy41_dexed_pool_4voice` (+`@POOL`), user
  audio-loved the 4 voices in lockstep + master transport. **The EAS APK is STALE** (pre-dates every app change
  above). The TSF-drum-stutter fix (loop-seam + one-shot decay + `@DRUMJIT` jitter probe) was USER-verified and
  **MERGED to master** (`ce97e53`); the `drum-smooth` branch is deleted.

## 2. THE VERIFICATION CONTRACT (unchanged — read twice)
You can **build (green)**, **serial-test** (`@`-commands over USB; the board prints `@STATE` JSON + a 1 Hz
`alive … cpuMax=XX% memMax=YY` heartbeat), and **flash**. You **cannot hear audio or see the app UI at
runtime** — the USER does. Build green, serial-verify, hand each milestone to the user to audio/UI-test
**before** merging. Never claim audio is "correct" — say "compiles + serial-verified; needs your ears."
- Green-build `teensy41_opll` (cheap net) on EVERY firmware change, plus what a change could touch:
  `..._nobt_voice2`, `..._nobt_drumvoice`, `..._4voice`, `..._dexed2_opll1`. `npx tsc --noEmit` on every app change.
- **RAM: green ≠ boots.** After any inventory/engine change check **OCRAM** `arm-none-eabi-size -A <elf> | grep
  '.bss.dma'` (cap 524288; boot-loops near ~2 KB free) AND DTCM (`.data`+`.bss`). A flapping COM port (in
  `list_ports` but `open()`="device does not exist") = boot-loop.
- **Serial parse:** heartbeat/@BEAT/[synth]/[adc] chatter interleaves `@STATE` — filter it. `outPeak`>0 with
  `dexedSumPeak`~0 proves the OPLL/other slot is sounding, not Dexed. pio.exe:
  `/c/Users/jaysh/AppData/Roaming/Python/Python313/Scripts/platformio.exe`. Board = COM4 @ 115200.

## 3. Remaining work (GO in this order)

### A. Ship the close-out to the phone (cheap, high value)
1. **EAS app rebuild (§3.3)** — the app now has the Synth-B removal + the Voice-Pool card + the 4-voice cards,
   but the APK is stale. `@jayshoes-team/tdsp-control`, `--profile preview`, `EXPO_TOKEN` + `EAS_NO_VCS=1` (see
   `reference_eas_tdsp`). `npx tsc --noEmit` first. **NOTE (multi-agent):** if the main working tree is on another
   branch/held by the parallel agent, run EAS from an isolated `git worktree add /c/tmp/x master` with
   `node_modules` junctioned (see §4).
2. **jay-mint recovery (§3.4)** — press PROGRAM on its Teensy, flash a current 4voice env (`jay@10.0.0.239` /
   `mint`, `/dev/ttyACM0`; use the IP, `.local` is flaky). See `tools/linux-flash-host.md` — APPEND the env to the
   box's `platformio.ini` (keep its `platform=teensy@5.1.0` pin); `printf U > /dev/ttyACM0` → HalfKay →
   `teensy_loader_cli` (2nd try). (This box is also the drum-stutter test rig — coordinate with `drum-smooth`.)

### B. Pool-partition polish
- **Grey the absorbed voice cards** when `@STATE.pool.active[v]==0` (a reduced preset). Today the firmware just
  rejects their launches (`@TRK.PLAY=IDLE`) and the Voice-Pool card's text spells out which voices are off — make
  Synth B/C/D visibly read "off / (idle — pool)" and disable their controls. App-only, tsc-verify. The state is
  already parsed (`pool.active` in App.tsx).

### C. Thread D — the app slot/engine picker (hetero builds)
- On `teensy41_dexed2_opll1` the app already renders a card per voice; add a **slot/engine picker** that reads the
  top-level `engines:{dexed,opll}` inventory + each voice's engine kind, and for an OPLL voice swaps the Dexed cart
  browser for the **OPLL ROM instrument list** (`@TRK<i>.INSTR=<0..14>`, keyed on the voice being in the OPLL
  range). tsc-verify; user UI-tests.

### D. Thread D — the firmware engine-inventory generalization (the real payoff)
Today `HeteroOpll.h` `static_assert`s `TDSP_OPLL_ENGINES==1` and the extra-voice song STATE is single-index
(`g_curSong3*`, `g_buf3`, `g_songFollow3`). To reach arbitrary mixes (`4dexed_2opll`, a TSF slot, …):
- **Array-ify the extra-voice song state** (name/arg/loop/bpm/bpb/loopBeats/launchPending/buffer/follower) so N>3
  total voices work — the same array-ify Phase 3 used for players/arps/routers. Then lift the `static_assert` and
  window N OPLL engines like the Dexed pool.
- **X-macro / small codegen for static wiring** (DESIGN §"Build-time engine inventory"): an engine array + each
  engine's `AudioConnection` to its sub-bus, kept in sync with the `TDSP_*_ENGINES` counts. Hand-wiring 8 Dexed +
  2 OPLL + 1 TSF is unmaintainable.
- **Coexisting backends:** `synthBegin`/expr-config/ReplayGain hooks are per-backend and assume ONE compiled;
  `HeteroOpll.h` already does OPLL-alongside-Dexed — generalize it (a per-kind companion header included by count,
  or an engine-kind vtable/registry).
- **Other engine kinds as slots:** OPLL (done) → Plaits/Rings/VA (FM/synthesis, CPU-bound, multi-instance cheap)
  → TSF/SF2 (RAM-bound; 1 today, 2 on a PSRAM core — §G).
- **Mix-slot pressure:** `outL/outR` are `AudioMixer4_F32` (4 inputs: 0=BT,1=tone/metronome,2=SPDIF/drum,3=synth).
  More engines than free slots need a **summing sub-bus tree** before the master limiter — design when the cap bites.
- **Runtime repartition already generalizes the Dexed side** (`@POOL`); extend the same idea to bind a Track to any
  compiled engine slot at runtime (the DESIGN's slot→Track binding).

### E. Thread E — per-track mixer strip (P3.5)
Each Track has `setLevel` + (pool) a per-voice trim node. Generalize to a real strip: per-track level/mute (and
optionally pan) → the track's sub-bus → master limiter → out (the Dexed `dxpTrim`/`dxpMix` pattern generalized to
every engine kind). App gets a mixer view (N faders + mutes) reading `tracks[]`. Keyboard-owner is already the
subscription hub — no separate work.

### F. Deferred Thread-C extension — BT/serial MIDI input sources
The hub has `SrcBtMidi`/`SrcSerial` enum slots, unwired. The ESP32 forwards BLE-MIDI / serial-MIDI bytes to the
Teensy → `midihub::<fanout>(SrcBtMidi/SrcSerial, …)`. Keep the ESP32 relay verbatim; new ESP32 parse + a Teensy
dispatch call. Then `@TRK<i>.SRC=bt|serial` works.

### G. Bigger inventories (PSRAM core)
TSF/SF2 slots (sample-RAM-bound) need the 32–64 MB OPI PSRAM core — see `project_core_opi_psram` (ADR-002, OPI
FlexSPI init). Two soundfont engines at once becomes feasible there. Until then keep soundfont count at 1 and
refuse/grey configs that exceed RAM.

## 4. Gotchas & lessons (this project — several bit hard)
- **MULTI-AGENT / ISOLATED WORKTREE (do this).** A parallel agent shares the repo. NEVER edit the main working
  tree if another agent may be active. Work in `git worktree add /c/tmp/<name> master`; for app tsc, junction the
  deps: PowerShell `New-Item -ItemType Junction -Path <wt>/app/tdsp-control/node_modules -Target <main>/…/node_modules`;
  commit+push from the worktree; then `git worktree remove --force` (the dir sometimes stays locked — `git worktree
  prune`, harmless). This kept the pool feature + handoff off the drum agent's tree cleanly.
- **`git add <file>` sweeps ANOTHER agent's uncommitted hunks in that file** into your commit — stage narrowly and
  `git diff --cached` before committing (a stray `setOneShotTail` line broke a build once). **UNTRACKED files vanish
  in branch churn** (not in git objects) — `git stash -u` / commit untracked docs before branch ops; make a
  `backup/<name>` branch before any merge/reset/cherry-pick.
- **An experimental env must inherit the board's usual feature flags.** `dexed2_opll1` extended bare `nobt` and
  silently lost `TDSP_METRONOME` (= the master transport) → dead master stop + clock stutter. Extend
  `nobt_drumvoice`/`voice2` (or copy their flags).
- **The master `Conductor` clock is foreground (loop-driven).** ANYTHING blocking `loop()` stutters it — including
  USB-serial backpressure when a client hammers a missing `@`-command (the app retrying dead `@METRO`).
- **Byte-identity for `TDSP_SYNTH_VOICES<4` (voice2/drumvoice):** gate ALL new code behind the count flags; VERIFY
  by building pre/post commit **in the SAME directory** and diffing the `.hex` (a worktree at a different PATH gives
  a spurious uniform-address-shift diff — NOT a real change). The optimizer erases `voiceLive()→true` on non-`>=4`.
- **F32 update order** (`project_f32_update_order`): the first hardware output constructed owns
  `update_responsibility` — `AudioOutputTDM_F32` must be declared FIRST or the whole F32 graph freezes.
- **Flash quirks:** `teensy_loader_cli` 1st try after HalfKay often prints `error writing` — rerun. App holding
  COM4 → `Access denied`; ask the user to disconnect. The board re-enumerates after flash — poll the port before
  serial-verifying.

## 5. Key reference
- **Firmware:** `firmware/mix-kit/` (NOT `projects/spike_esp32_bt_spdif_mix_kit`). main.cpp ~3.6k lines; backends
  `src/SynthBackend*.h` + `HeteroOpll.h`/`DrumVoice.h`; `src/Track.h`. Pool partition = `SynthBackendDexedPool.h`
  (`poolSetPreset`/`poolApplyPartition`) + main.cpp (`@POOL`, `voiceLive`, `@STATE pool`).
- **App:** `app/tdsp-control/` — `App.tsx` (cards + `@STATE` parse), `src/transport.{ts,web,native,wifi}.ts`
  (add a method to the interface + all THREE impls or tsc fails).
- **Envs:** `teensy41_opll` (net), `..._nobt_voice2` (2 Dexed), `..._nobt_drumvoice` (Dexed+OPLL drums),
  `..._4voice` (4 Dexed + OPLL drums + metronome + `@POOL`; the shipped board), `dexed2_opll1` (2 Dexed + 1 melodic OPLL).
- **Boards/hosts:** local COM4 (SN 18402920, nobt, no ESP32); jay-mint (`jay@10.0.0.239`/`mint`, `/dev/ttyACM0`,
  LINE out, ESP32 BT). App: `@jayshoes-team/tdsp-control`, EAS preview, `EXPO_TOKEN`+`EAS_NO_VCS=1`.
- **Safety branches:** `backup/pre-merge-2026-07-18`, `backup/tracks-p4d-2026-07-17` (tag
  `tracks-snapshot-2026-07-17-p4d-green`).

## 6. Don't-break list
- master + the deployed boxes are the working state — only merge **green + USER-verified**; backup branch first.
- **Work in an isolated worktree** while the parallel agent is active; don't touch its `drum-smooth` WIP.
- Keep the ESP32 relay verbatim; new `@TRK<i>.*`/`@POOL` just pass through.
- The subscription hub stays repatch-free — no `loadVoice`/AudioConnection edit on a routing switch.
- Never enable A2DP + WiFi together on the classic ESP32 (`feedback_never_a2dp_wifi`; `#error` guard exists).
- `TDSP_SYNTH_VOICES<4` (voice2/drumvoice) stays byte-identical; gate all new inventory/pool code behind the flags.

## 7. One-line status of every thread
- Phase 0–3: **DONE**, on master. Thread D minimal (2 Dexed + 1 OPLL): **DONE**, serial-verified. 3 fixes:
  **DONE**. Synth-B switch removal: **DONE**. Runtime `@POOL` repartition (fw+app): **DONE**, serial-verified.
- **NEXT:** §A EAS rebuild + jay-mint → §B grey pool cards → §C app slot/engine picker → §D firmware inventory
  generalization (multi-engine) → §E per-track mixer strip → §F BT/serial inputs → §G PSRAM soundfont slots.
- TSF-drum-stutter fix: **DONE**, user-verified + merged to master (`ce97e53`).
