# Tracks — finish-the-project handoff / next-agent prompt

You are completing the **Tracks refactor** for the T-DSP box (Teensy 4.1 mix-kit firmware + an
Expo/React-Native control app + an ESP32 BT/BLE/WiFi front-end). Read this whole doc, then
`planning/tracks/DESIGN.md`, `planning/tracks/PHASE4_HANDOFF.md` (the phase-4 detail), and the
`project_tracks_refactor` memory. Phases 0–4-minimal are DONE and **merged to master**; this doc is
the punch-list to finish the DESIGN's vision.

## 0. What "done" means
The DESIGN's endgame: **N configurable Track slots, each bindable to any engine kind**, drums are
just-another-Track, every track has a real mixer strip, and every input source (song / looper / live
DIN / USB-host / **BT** / **serial**) flows through the one uniform Track transport. Today we have the
skeleton + a minimal heterogeneous proof (2 Dexed + 1 OPLL) + a full 4-Dexed build. "Finish" = the
remaining threads in §3, each shippable and green, gated so nothing regresses.

## 1. Current state (as of this handoff — 2026-07-18)
**master `6a301b2` (local + origin, in sync, pushed).** Contains:
- **Phase 3** — 4 independent Dexed voices (Thread A, fixed 4-way pool split), data-driven app cards
  (Thread B), the MIDI-input **subscription hub** (Thread C: `midihub::` in main.cpp, per-Track
  `{liveSrcMask, srcChMask}`, `@TRK<i>.SRC/SRCCH`, zero-repatch source switching), tempo auto-follow,
  drum-note-map (`DrumNoteMapper`), and the `@TRK<i>.*` uniform command family.
- **Phase 4 Thread D (minimal)** — heterogeneous engine inventory. A build can mix engine KINDS:
  - `HeteroOpll.h` — a melodic OPLL (YM2413) voice ALONGSIDE the Dexed pool (sibling of `DrumVoice.h`),
    on its own free mix slot. Env `teensy41_dexed2_opll1` = 2 Dexed pool voices + 1 melodic OPLL (track 2).
  - **Slot abstraction:** count flags `TDSP_HETERO / TDSP_DEXED_VOICES / TDSP_OPLL_ENGINES`; `kDexedVoices`
    splits voices `[0,kDexedVoices)`=Dexed / rest=OPLL; `voiceIsOpll()` dispatches per-track. Engine-agnostic
    `@TRK<i>.INSTR=<idx>` selector routes by slot kind. `@STATE` publishes a top-level `engines:{dexed,opll}`
    inventory (NOT inside `caps` — kept out so the shared caps printf stays byte-identical) + per-OPLL-track
    `"eng":"opll"`. All gated by the count flags → non-hetero builds byte-identical.
- **3 hardware-verified fixes** landed while the user tested on COM4:
  - Hetero Dexed split is **PERMANENT** (retire the `@VOICE2` toggle at `TDSP_HETERO`, like N≥4) — fixed
    A/B voice-stealing (un-split pool made both voices fight over the same engines).
  - Hetero env needs **`TDSP_METRONOME=1`** — `@METRO` IS the master transport play/stop; the plain-`nobt`
    parent didn't set it, so master-stop/metronome were dead and the app hammering the missing command
    stalled the loop-driven clock (the "stutter").
  - Master stop clears **EVERY voice** (`transportStop` looped only 0/1; now `v=2..kSynthVoices` too) — a
    started OPLL/Synth-C (or 4-voice voices 2/3) rang forever. Gated `#if TDSP_SYNTH_VOICES >= 3`.
- **Verified:** user audio-loved the 4-voice build (all 4 synths + arps in lockstep on the master grid);
  `teensy41_dexed2_opll1` serial-verified (`@TRK0`→Dexed, `@TRK2`→OPLL, `dexedSumPeak=0` when only the OPLL
  plays = clean engine isolation). Green: opll + voice2 + drumvoice + 4voice + dexed2_opll1.
- **Deployed:** local dev board (SN 18402920, **COM4**) runs `teensy41_dexed_pool_4voice`. The EAS APK is
  still an OLD build. `drum-smooth` branch holds a **parallel TSF-drum-stutter investigation** (WIP, not merged;
  see `planning/tracks/DRUM_STUTTER_HANDOFF.md`, partially reconstructed).

## 2. THE VERIFICATION CONTRACT (unchanged — read twice)
You can **build (green)**, **serial-test** (`@`-commands over USB; the board prints `@STATE` JSON + a
1 Hz `alive … cpuMax=XX% memMax=YY` heartbeat), and **flash**. You **cannot hear audio or see the app
UI at runtime** — the USER does. So: build green, serial-verify, hand each milestone to the user to
audio/UI-test **before** merging. Never claim audio is "correct" — say "compiles + serial-verified;
needs your ears."
- Green-build `teensy41_opll` (cheap net, fits anywhere, GM+drums) on EVERY firmware change, plus the
  builds a change could affect: `..._nobt_voice2`, `..._nobt_drumvoice`, `..._4voice`, `..._dexed2_opll1`.
  `npx tsc --noEmit` on every app change.
- **RAM: green ≠ boots.** The nobt profile suppresses teensy_size's RAM report → after every inventory
  change check **OCRAM** `arm-none-eabi-size -A <elf> | grep '.bss.dma'` (cap 524288; boot-loops near ~2 KB
  free) AND DTCM (`.data`+`.bss`). A flapping COM port (in `list_ports` but `open()`="device does not exist")
  = boot-loop, not a driver glitch.
- **Serial-verify pattern:** the heartbeat/@BEAT/[synth]/[adc] chatter interleaves `@STATE` replies — filter
  it when parsing (see the throwaway scripts this session used; `outPeak`>0 with `dexedSumPeak`~0 proves the
  OPLL slot is sounding, not Dexed). pio.exe: `/c/Users/jaysh/AppData/Roaming/Python/Python313/Scripts/platformio.exe`.

## 3. Remaining work (in suggested order)

### A. Finish the Phase-3 close-out (cheap, ships the payoff to the phone)
1. **EAS app rebuild (§3.3)** so the phone gets the 4-voice cards: `@jayshoes-team/tdsp-control`,
   `--profile preview`, `EXPO_TOKEN` + `EAS_NO_VCS=1` (see `reference_eas_tdsp`). The app already renders a
   card per `@STATE tracks[]` entry; a 4-voice board shows Synth C/D. Verify `npx tsc --noEmit` first.
2. **jay-mint recovery (§3.4)** — get someone to press PROGRAM on its Teensy, then flash a current 4voice
   env (`jay@10.0.0.239` / pass `mint`, `/dev/ttyACM0`; use the IP, mDNS `.local` is flaky). See
   `tools/linux-flash-host.md` — append the env to the box's `platformio.ini` (don't overwrite its
   `platform=teensy@5.1.0` pin); `printf U > /dev/ttyACM0` → HalfKay → `teensy_loader_cli` (2nd try).

### B. Thread D — the app side (slot/engine picker)
- The data-driven card already reads `tracks[]`. Add: read the top-level `engines:{dexed,opll,…}` inventory,
  show each track's engine kind, and (where the build compiles ≥1 engine of a kind a track could bind to) a
  **slot picker** ("Synth C → OPLL #1"). For the OPLL voice, swap the Dexed cart browser for the OPLL ROM
  instrument list (`@TRK<i>.INSTR=<0..14>`), keyed on `"eng":"opll"`.
- **Hide the "Synth B enable" toggle on a hetero build** — the split is permanent now, so the toggle is a
  no-op wart. Gate it on the inventory (or a new caps flag). tsc-verify only; user UI-tests.

### C. Thread D — the firmware generalization (the real inventory)
Today `HeteroOpll.h` `static_assert`s `TDSP_OPLL_ENGINES == 1` and the extra-voice song STATE is single-index
(`g_curSong3*`, `g_buf3`, `g_songFollow3`). To reach arbitrary mixes (e.g. `4dexed_2opll`, or a TSF slot):
- **Array-ify the extra-voice song state** (name/arg/loop/bpm/bpb/loopBeats/launchPending/buffer/follower) so
  N>2 total voices work — the same array-ify pattern Phase 3 used for players/arps/routers. Then lift the
  `static_assert` and window N OPLL engines like the Dexed pool windows.
- **X-macro / small codegen for static wiring** (DESIGN §"Build-time engine inventory"): an engine array +
  each engine's `AudioConnection` to its sub-bus, kept in sync with the `TDSP_*_ENGINES` counts by macro
  expansion (hand-wiring 8 Dexed + 2 OPLL + 1 TSF is unmaintainable). Each `SynthBackend*.h` grows from "the
  one engine" to "N engines + bus taps."
- **Coexisting backends:** `synthBegin`/expr-config/ReplayGain hooks are per-backend and assume ONE compiled.
  A mixed build needs each engine's hooks called (namespacing or an engine-kind dispatch). `HeteroOpll.h`
  already does this for OPLL-alongside-Dexed; generalize it (a `SynthBackend` vtable/registry, or per-kind
  companion headers included by count).
- **Other engine kinds as slots:** OPLL (done), then Plaits/Rings/VA (FM/synthesis, CPU-bound → multi-instance
  cheap), then TSF/SF2 (RAM-bound → 1 today, 2 on a PSRAM core — see §F).
- **Mix-slot pressure:** `outL/outR` are `AudioMixer4_F32` (only 4 inputs: 0=BT,1=tone/metronome,2=SPDIF/drum,
  3=synth). Two engines already fill the free slots on nobt. More engines need a **summing sub-bus stage**
  (a mixer tree) before the master — design this when the 4-slot cap bites.

### D. Thread E — per-track mixer strip (P3.5)
Each Track has `setLevel` + (pool) a per-voice trim node. Generalize to a real strip: per-track level/mute
(and optionally pan) → the track's sub-bus → master limiter → out (the Dexed `dxpTrim`/`dxpMix` pattern
generalized to every engine kind). App gets a mixer view (N faders + mutes) reading `tracks[]`. Keyboard-owner
is already the subscription hub (Thread C) — no separate work.

### E. Deferred Thread-C extension — BT/serial MIDI input sources
The subscription hub has `SrcBtMidi`/`SrcSerial` enum slots but they're NOT wired. The ESP32 forwards
BLE-MIDI / serial-MIDI bytes to the Teensy → `midihub::dispatch(SrcBtMidi/SrcSerial, ev)`. Keep the ESP32
relay verbatim; this is new ESP32 parse + a Teensy dispatch call. Then `@TRK<i>.SRC=bt|serial` works.

### F. Bigger inventories (PSRAM core)
TSF/SF2 slots (sample-RAM-bound) need the 32–64 MB OPI PSRAM core — see `project_core_opi_psram` (ADR-002,
OPI FlexSPI init). Two soundfont engines at once (a GM piano track + a sampled-drums track) becomes feasible
there. Until then, keep soundfont slot count at 1 and gate configs that exceed RAM.

## 4. Gotchas & lessons (this session + the memories)
- **RAM is the wall.** Each song buffer ~6 B/event; TSF/SF2 hold MB of samples; OPLL is RAM-cheap (~9 KB).
  The 4voice build needed `MAX_EVENTS3=2000` + `TDSP_DIAGNOSTICS=0` just for 4 Dexed voices. Check DTCM AND
  OCRAM after every inventory change.
- **An experimental env must inherit the board's usual feature flags.** `teensy41_dexed2_opll1` extended plain
  `nobt` and silently lost the metronome/master-transport (`TDSP_METRONOME`) → dead master stop + clock
  stutter. Extend `nobt_drumvoice`/`voice2` (or copy their flags), not bare `nobt`, for a usable build.
- **The master `Conductor` clock is foreground (loop-driven).** ANYTHING blocking `loop()` stutters it —
  including USB-serial backpressure when a client hammers an unhandled command (the app retrying dead
  `@METRO`). A missing/renamed `@`-command isn't harmless; it can floods-back and stall the clock.
- **Byte-identity for `TDSP_SYNTH_VOICES<4` (voice2/drumvoice) is required.** Gate ALL new inventory code behind
  the count flags (`#if TDSP_HETERO`, `>=3`/`>=4`). VERIFY by building the pre-change and post-change commit **in
  the SAME directory** and diffing the `.hex` — a git worktree at a different PATH gives a spurious diff (uniform
  RAM-address shift), NOT a real change.
- **F32 update order** (`project_f32_update_order`): the first hardware output constructed owns
  `update_responsibility` — `AudioOutputTDM_F32` must be declared FIRST or the whole F32 graph freezes.
- **Flash quirks:** `teensy_loader_cli` 1st try after HalfKay often prints `error writing` — rerun. The
  `.pio/build/<env>/` dir got wiped mid-flash once → keep a stable hex copy. App holding COM4 → `Access denied`;
  ask the user to disconnect (or the control page's `web/release.flag` handoff).
- **MULTI-AGENT GIT HYGIENE (bit hard this session).** A parallel `drum-smooth` agent shared the working tree.
  Two real hazards: (1) `git add <file>` sweeps ANOTHER agent's uncommitted hunks in that file into YOUR commit
  — stage narrowly and `git diff --cached` before committing; a stray `setOneShotTail` line slipped into a fix
  commit and broke the build. (2) UNTRACKED files vanish in branch churn and aren't in git objects — an untracked
  handoff doc was lost and only ~90% reconstructable. **Commit or `git stash -u` untracked docs before branch ops;
  make a `backup/<name>` branch before any merge/cherry-pick/reset.**

## 5. Key reference
- **Firmware:** `firmware/mix-kit/` (NOT `projects/spike_esp32_bt_spdif_mix_kit`, that's the old spike).
  main.cpp ~3600 lines; backends `src/SynthBackend*.h` + `HeteroOpll.h`/`DrumVoice.h`; `src/Track.h`.
- **Envs:** `teensy41_opll` (cheap net), `..._nobt_voice2` (2 Dexed), `..._nobt_drumvoice` (Dexed+OPLL drums),
  `..._4voice` (4 Dexed + OPLL drums + metronome; the shipped board), `dexed2_opll1` (2 Dexed + 1 melodic OPLL).
- **Boards/hosts:** local COM4 (SN 18402920, nobt, no ESP32); jay-mint (`jay@10.0.0.239`/`mint`, `/dev/ttyACM0`,
  LINE out, has ESP32 BT). App: `@jayshoes-team/tdsp-control`, EAS preview, `EXPO_TOKEN`+`EAS_NO_VCS=1`.
- **Safety branches:** `backup/pre-merge-2026-07-18`, `backup/tracks-p4d-2026-07-17` (tag
  `tracks-snapshot-2026-07-17-p4d-green`).

## 6. Don't-break list
- master + the deployed boxes are the working state — only merge **green + USER-verified**; make a backup
  branch first.
- Keep the ESP32 relay verbatim (`@`-lines); new `@TRK<i>.*` just pass through.
- The subscription hub must stay repatch-free — no `loadVoice`/AudioConnection edit on a routing switch.
- Never enable A2DP + WiFi together on the classic ESP32 (`feedback_never_a2dp_wifi`; there's an `#error` guard).
- `TDSP_SYNTH_VOICES<4` (voice2/drumvoice) stays byte-identical; gate all new inventory code behind the count flags.
- Don't touch the `drum-smooth` branch's WIP without coordinating — it's an active parallel investigation.

## 7. Suggested execution order
1. **Close-out A** — EAS rebuild + jay-mint recovery (ships Phase 3 to the phone; small).
2. **Thread D app (B)** — slot/engine picker + hide the Synth-B toggle (tsc-verifiable, unblocks hetero UX).
3. **Thread D firmware generalization (C)** — array-ify extra-voice state → lift the 1-OPLL cap → X-macro
   wiring → a second engine kind (Plaits/Rings) as a slot. Prove each with a SMALL env + serial-verify.
4. **Thread E** — per-track mixer strip + app mixer view.
5. **Thread-C extension (E)** — BT/serial MIDI inputs via the ESP32.
6. **PSRAM inventories (F)** — once a PSRAM core exists.
7. (Parallel, separate owner) the `drum-smooth` TSF-drum-stutter fix.
