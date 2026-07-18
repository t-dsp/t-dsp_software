# Phase 3 completion — handoff / next-agent prompt

You are finishing **Phase 3 of the Tracks refactor** for the T-DSP box (Teensy 4.1 mix-kit firmware +
an Expo/React-Native control app + an ESP32 BT/BLE/WiFi front-end). Read this whole doc first, then
`planning/tracks/DESIGN.md`, `planning/tracks/PHASE3.md`, and the `project_tracks_refactor` memory.

## 0. What "done" means
Three threads, in this order (each independently green + shippable):
- **A. 4 independent Dexed synth voices** (the real "N synths" payoff).
- **B. Data-driven app cards** — the app renders one card per `@STATE tracks[]` entry, so a new voice
  needs zero app edits.
- **C. MIDI input routing** — a channel/source **subscription** model so any input device feeds any
  synth with **zero audio/loop/clock impact** on switch (the user's spec — see §4, it's the important one).

## 1. Current state (as of this handoff)
- **master @ `bd8f397`** (pushed): Phase 1 (Voice 1/2 unified behind `Track`), Phase 2 (drums as a
  Track + app card + generic SD `/midi` browser via `@LS`), Phase 3 P3.1 (`@STATE tracks[]` + `caps.tracks`)
  and P3.2 (`@TRK<i>.<CMD>` uniform per-track transport). This is what's deployed + sounds great.
- **branch `tracks-phase3-voices`** (off master, NOT merged): the array foundation for N voices, in
  green behavior-identical increments —
  - `8129435` players → `g_playerV[kSynthVoices]` (`g_player`/`g_player2` are aliases for [0]/[1])
  - `99e2692` arps + routers → `g_arpFilterV[]`/`g_routerV[]` (same alias pattern)
  - `255fe7d` the executable P3.4/P3.3 plan appended to PHASE3.md
  `kSynthVoices = TDSP_SYNTH_VOICES` (default 2 on voice2 / 1 else). At N=2 everything is byte-identical.
- **Deployed:** local COM4 (`teensy41_dexed_pool_nobt_voice2`), jay-mint (`teensy41_dexed_pool_jaymint_
  voice2_serial` Teensy + `esp32dev` ESP32 BLE), EAS APK (jayshoes-team/tdsp-control, preview). Keep them
  working — don't merge unverified audio.

## 2. THE VERIFICATION CONTRACT (critical — read twice)
You can **build (green)**, **serial-test** (drive `@`-commands over USB, read `@STATE`), and **flash**.
You **cannot hear audio or see the app UI at runtime.** So:
- Audio correctness (mix balance, per-voice ReplayGain loudness, MPE across voices, no clipping) and app
  UX are verified by the **USER**. Build in green, serial-verified increments on the branch; hand each
  milestone to the user to audio/UI-test **before** merging to master. Never claim audio is "correct" —
  say "compiles + serial-verified; needs your ears."
- **Green-build every change on `teensy41_opll` too** (it fits any board and is GM+drums) — it's the
  cheap regression net besides `..._voice2` / `..._drumvoice`.
- **Flashing quirks (real, will bite you):**
  - PlatformIO sometimes reuses a stale `main.cpp.o` → the board runs OLD code though upload "succeeds".
    Before every upload run `rm -f firmware/mix-kit/.pio/build/<env>/src/main.cpp.o` and confirm the log
    shows `Compiling .pio\build\<env>\src\main.cpp.o`. This bit us ~4×.
  - `teensy_loader_cli` first attempt after HalfKay often prints `error writing to Teensy` — just rerun.
  - Board re-enumerates for a few seconds after flash → retry `serial.Serial('COM4')` in a loop.
  - The app holding COM4 → `Access denied`; ask the user to disconnect, or press the PROGRAM button.
  - pio.exe: `/c/Users/jaysh/AppData/Roaming/Python/Python313/Scripts/platformio.exe`.
- **jay-mint** (Linux flash host, `jay@jay-mint.local`, pass `mint`): see `tools/linux-flash-host.md`.
  Stream `firmware/mix-kit/src` + changed `lib/TDsp*` over SSH; **don't** overwrite the box's
  `platformio.ini` (it has the box-only `platform = teensy@5.1.0` pin). Teensy: `printf U > /dev/ttyACM0`
  → HalfKay → `~/.platformio/packages/tool-teensy/teensy_loader_cli --mcu=TEENSY41 -w -v <hex>` (2nd try).
  ESP32: `esp32dev` needs `-D TDSP_CTRL_BLE`; flash via `printf g` → esptool `--before/--after no_reset
  --baud 115200` → `printf @BOOTAPP@`.

## 3. Thread A — 4 independent Dexed voices (P3.4)
Decided with the user: **2+2+2+2, independent busses** (each voice = its own mix bus / volume /
ReplayGain trim). 8 engines ÷ 4 = 2 engines/voice ≈ 2-note MPE / 4-note normal poly (thin, accepted).
The array foundation (players/arps/routers) is already done. Remaining (one coupled edit — nothing
compiles until it's all in; do it on the branch, build-verify, then USER audio-tests):

**`firmware/mix-kit/src/SynthBackendDexedPool.h`** (gate the new path on `#if TDSP_SYNTH_VOICES==4`;
leave the `N<=2` code BYTE-IDENTICAL so voice2/drumvoice are unaffected):
- Graph: replace `dxpMixA/dxpMixB` (2× 4-in) with `dxpMix0..3` (each sums `dxpc[2i],dxpc[2i+1]`),
  `dxpTrim0..3` (`dxpMix[i]→dxpTrim[i]`), a 4-in `dxpSum` (`dxpTrim[i]→dxpSum(i)`). Limiter taps `dxpSum`
  (trims pre-sum, like today's split case). `synthAuditionTrim()` → `&dxpTrim0` (or the current
  audition voice). `synthBegin()` mixer-gain init loops 0..3.
- Sinks: `g_poolSink0..3` over `&g_pool[2i]` (2 engines, `kPoolVpe`). `g_synthSink`=&poolSink0;
  main.cpp binds track i's sink = &poolSink[i].
- State → per-voice: `g_synthInstrument[4]`, `g_curCart{Rel,Voice,Name}[4]`, `g_voiceVolPct[4]`. Fold the
  `synthSetInstrument`/`synthSetInstrument2` (and `…PickCartVoice`/`…2`, `synthSetSongVol`/`…Voice2Vol`,
  `reloadVoice{1,2}Window`) twins into ONE indexed family `synthSetInstrumentV(i,idx)` etc. (they already
  take `(start,count)` → call with `(2i,2)`). `applyPoolVols`: `dxpTrim[i].gain = replayGain[i]·userVol[i]`;
  slot-3 = the fixed makeup. Retire the runtime `@VOICE2` split toggle at N=4 (it's a fixed 4-way).
- `synthName()`/desc can note "4× Dexed".

**`firmware/mix-kit/src/main.cpp`:** grow the arrays to `kSynthVoices=4`; followers stay individual
(`PlayerFollower` ctor takes a `player&` reference member — add `g_songFollow3{g_playerV[2]}`,
`g_songFollow4{g_playerV[3]}` under `#if`), `g_loop`[i] recorders (or null looper on voices 2/3 initially).
`g_tracks[kSynthVoices]` — bind [2],[3] in `tracksInit` + `trackWireSetup(g_tracks[i])` in a loop (already
generic). `@STATE tracks[]` already loops (P3.1); make it emit N synth entries. `@TRK<i>.*` already index-
routes (P3.2). Add the state globals (name/arg/loop/bpm/… ) for voices 2/3 or array them.

**New env `teensy41_dexed_pool_4voice`** = the pool + `-D TDSP_SYNTH_VOICES=4` (+ drums + serial as fits).
**Verify:** green all envs; serial `@TRK0..3.PLAY=<song>` each drives its own voice, `@STATE tracks[]`
shows 4 synth + drum. **Then the user audio-tests balance/MPE/no-clip before merge.**

## 4. Thread C — MIDI input routing (the user's spec — DO THIS RIGHT)
### The requirement (verbatim intent)
The box may have **several MIDI INPUT DEVICES at once**: DIN MIDI-in (Serial1), USB-host (LinnStrument
etc.), Bluetooth MIDI (via ESP32), and a serial-line source. And there are **N synth voices** (4/5/…).
We want:
1. A **simple way to select which synth receives the sum of {its MIDI player} + {a live input device}.**
2. Per synth **card**: pick **which input device it defaults to when enabled**, with **on-the-fly switching**.
3. **Zero impact on loop latency, the player, the clock, or audio** on switch — the synth is *just paying
   attention to a stream*. (The previous keyboard-owner switch had a noticeable delay — because `@VOICE2`
   **repatched the pool engines** (`loadVoice` per engine). NEVER repatch audio on a routing switch.)

### The design — source/channel SUBSCRIPTION (no repatch, ever)
The per-track routers are already arrays (`g_routerV[N]`), which is exactly the seam. Model:
- **`enum MidiSourceId { SrcNone, SrcDin, SrcUsbHost, SrcBtMidi, SrcSerial, … }`** — one per physical input.
- Each input source, when it produces an event, calls a central **`MidiHub::dispatch(SourceId src, event)`**
  (a tiny new component, or fold into the existing router fan-out). The hub forwards the event to **every
  Track whose live subscription matches** — into that Track's `g_routerV[i]` (→ its arp → its sink).
- **Each Track carries a lightweight subscription: `{ MidiSourceId liveSource; uint16_t channelMask; }`**
  (default per track, e.g. `SrcDin` all-channels; app-configurable). "Which device feeds this synth" =
  `liveSource`. "Subscribe to a channel" = `channelMask` filter. **Switching = one field assignment.**
- The Track's **MIDI player always feeds its sink** (unchanged); the live input is simply *added* via the
  hub → the same `router → arp → sink`. So the synth hears **player + subscribed live device**, exactly as
  asked. Selecting the *active* synth for a device = set that Track's `liveSource` to the device (and
  optionally clear it on the others, or allow several — app policy).
- **Why zero-impact:** `dispatch` is an O(N-tracks) pointer/enum compare + forward — no audio-graph edit,
  no `AudioConnection` change, no `loadVoice`/engine repatch, no clock/player touch. The audio graph and
  engines are **static and always wired**; only *which router sees the event* changes, and that's a field
  read. Contrast the old `@VOICE2` toggle which reloaded engines (the audible delay). This must replace /
  bypass the `usbRouter()` keyboard-owner switch (`g_voice2On ? g_kbdRouter : g_router`).

### Where the sources are today (wire them into the hub)
- **DIN**: `Serial1` MIDI, `while (MIDI.read())` in `loop()`, callbacks `midiNoteOn/Off/CC/Pitch/Pressure`
  → currently `g_router`. Re-point: `hub.dispatch(SrcDin, ev)`.
- **USB host**: `g_usbHost/g_usbMidi`, callbacks `usbNoteOn/…` → currently `usbRouter()`. Re-point:
  `hub.dispatch(SrcUsbHost, ev)`. (This removes the keyboard-owner delay.)
- **BT MIDI**: not yet an input — the ESP32 currently does A2DP audio + BLE/WiFi **control** (`@`-lines),
  not MIDI. Adding BLE-MIDI (or MIDI-over-the-control-link) is a sub-task: ESP32 forwards MIDI bytes on a
  tagged channel to the Teensy, which calls `hub.dispatch(SrcBtMidi, ev)`. Scope this after the local
  sources work.
- **Serial**: a MIDI-over-serial source (e.g. the control UART carrying MIDI frames) → `hub.dispatch(SrcSerial, ev)`.

### Wire protocol + app
- Firmware: `@TRK<i>.SRC=<din|usb|bt|serial|none>` and `@TRK<i>.SRCCH=<0=all|1..16>` set the Track's
  subscription (pure field writes). Report in `@STATE tracks[]` per entry: `"src":"din","srcch":0`.
- App: each synth card gets a **MIDI Input** control (device dropdown + optional channel) — the default-
  when-enabled + live-switch. Reuse the data-driven card (Thread B) so it's one control for all voices.
- **Latency claim to verify with the user:** switching `@TRK<i>.SRC` mid-performance produces NO dropout
  in any playing loop/arp and no note stall — because nothing repatches. (The user explicitly flagged the
  old delay; make this the acceptance test.)

## 5. Thread B — data-driven app cards (P3.3)
`app/tdsp-control/App.tsx`: build the synth+drum card sections from a list derived from `@STATE tracks[]`
— one `<TrackCard>` per entry, reusing the existing components indexed by `i` (`makeSongDeck` with an
`@TRK<i>` wire, `arpBody(arpSlot[i])`, the voice/kit browser, and the new §4 MIDI-input selector). A new
firmware voice → a new `tracks[]` entry → a new card, **no app edit**. Needs `@TRK` extended beyond
transport (P3.2 did PLAY/RESTART/STOP/VOL/LOOP/ARPON) to cover voice-select (`@TRK<i>.DXPICK=`), full arp
params (`@TRK<i>.ARP{PAT,RATE,OCT,LATCH}=`), and the §4 `SRC`/`SRCCH`. **tsc-verify here; USER UI-tests.**
The deck/arp/browser are ALREADY parameterized (`songDeck1/2`, `arpSlot1/2`, `drumDeck`, `voiceBrowserBody
(target)`) — generalize the `target: 1|2` params to an index, and generate the sections in a loop.

## 6. Suggested execution order (each = green + a user test gate)
1. **Thread C local sources** first (biggest UX win, low risk, no audio-graph change): add `MidiHub` +
   per-track `{liveSource,channelMask}` + `@TRK<i>.SRC/SRCCH` + `@STATE`; re-point DIN + USB-host into the
   hub; retire the `usbRouter()` owner switch. Serial-verify routing; USER tests zero-latency switching.
2. **Thread B** app: data-driven cards + the MIDI-input selector (works with today's 2 voices + drum).
   USER UI-tests.
3. **Thread A** 4 voices: the pool-header graph + env. USER audio-tests balance/MPE. Then the 4th/5th card
   appears automatically (Thread B) and each gets a MIDI source (Thread C). Merge to master.
4. **BT/serial MIDI sources** (Thread C extension) once the model is proven.
5. Reflash local + jay-mint + rebuild EAS; update the `project_tracks_refactor` memory + PHASE3.md.

## 7. Don't-break list
- master + the deployed boxes are the working state — only merge green + user-verified.
- `N<=2` behavior must stay byte-identical (voice2/drumvoice are deployed).
- No audio-graph repatch on a MIDI routing switch (the whole point of §4).
- Keep the ESP32 relay verbatim (`@`-lines) — new `@TRK<i>.SRC` etc. just pass through.
- Green-build opll + voice2 + drumvoice (+ the new 4voice) on every firmware change; `npx tsc --noEmit`
  on every app change.
