# Phase 4 — handoff / next-agent prompt

You are continuing the **Tracks refactor** for the T-DSP box (Teensy 4.1 mix-kit firmware + an
Expo/React-Native control app + an ESP32 BT/BLE/WiFi front-end). Read this whole doc first, then
`planning/tracks/DESIGN.md` (the engine-inventory section), `planning/tracks/PHASE3.md`, and the
`project_tracks_refactor` memory.

## 0. What "done" means
Phase 3 (data-driven N synth voices) is code-complete on branch `tracks-phase3-voices`. Phase 4 is
the **heterogeneous engine inventory** — the DESIGN.md endgame. Two threads, in order:
- **CLOSE-OUT (do FIRST):** verify Phase 3 audio, merge `tracks-phase3-voices` → master, rebuild the
  EAS app, recover jay-mint. This ships the 4-voice payoff before starting the big refactor.
- **D. Engine inventory (the payoff):** today all N pool voices are the SAME engine (Dexed). Let a
  build declare N engines of DIFFERENT kinds (e.g. 2 Dexed + 1 OPLL + 1 TSF) and let each Track bind
  to any slot. This is "N synths of different kinds" — a Dexed lead + an OPLL bass + a sampled piano.
- **E. Per-track mixer strip (P3.5):** each Track its own level/mute/pan node → sub-bus → master.

## 1. Current state (as of this handoff)
- **branch `tracks-phase3-voices`** (18 commits ahead of master, NOT merged). Phase 3 A/B/C done:
  - **Thread C** `1aad980` — MIDI input **subscription hub** (`midihub::` in main.cpp): `enum
    MidiSourceId` + per-Track `{liveSrcMask, srcChMask}`; `@TRK<i>.SRC/SRCCH`; @STATE `src`/`srcch`.
    Switching a synth's input device is a field write — NO audio repatch (the zero-latency spec). The
    old `usbRouter()` keyboard-owner switch is gone. DIN + USB-host wired; **BT/serial NOT wired yet**.
  - **Thread A** `333ea0f` + `2107baf` — **4 independent Dexed voices** (fixed 2+2+2+2 pool split),
    each its own player/arp/router/subscription/bus/level/ReplayGain, gated `#if TDSP_SYNTH_VOICES>=4`
    (N<=2 byte-identical). Env `teensy41_dexed_pool_4voice` (+ `..._jaymint_4voice_serial`). **CRASH
    FIX** `2107baf`: `MAX_EVENTS3` 12000→2000 (the two extra song buffers had starved OCRAM → boot
    loop; free OCRAM 2208→122208). **Flash-verified running on local COM4.**
  - **Thread B** `6228df6`/`11d1491`/`cac04db` — data-driven app cards: a **card per synth voice** from
    `@STATE tracks[]`; Synth C/D are FULL peers of A/B (submenu: Synth/Voices + MIDI Player + Arp),
    reusing every component via `@TRK<i>.*`. Generic `Transport.trk(i,cmd)`; MIDI-input selector.
- **Deployed/verified:** local COM4 runs `teensy41_dexed_pool_4voice` (4 voices + drum in @STATE).
  jay-mint got the fixed env built on-box but is **stuck on the crashing build** — needs a physical
  PROGRAM press to reflash (its serial `U`→HalfKay is dead once crashed). EAS APK is still the OLD
  (2-voice) build — the phone won't show Synth C/D until an EAS rebuild.

## 2. THE VERIFICATION CONTRACT (unchanged — read twice)
You can **build (green)**, **serial-test** (`@`-commands over USB, read `@STATE`), and **flash**. You
**cannot hear audio or see the app UI at runtime** — the USER does. So: build in green, serial-verified
increments; hand each milestone to the user to audio/UI-test **before** merging. Never claim audio is
"correct" — say "compiles + serial-verified; needs your ears."
- Green-build `teensy41_opll` (fits anywhere, GM+drums) on every firmware change — the cheap net —
  plus `..._voice2`, `..._drumvoice`, `..._4voice`. `npx tsc --noEmit` on every app change.
- **Flash quirks that WILL bite (all hit this session):**
  - The nobt board profile suppresses teensy_size's RAM report → check OCRAM with
    `arm-none-eabi-size -A <elf> | grep '.bss.dma'` (cap 524288) and DTCM likewise; a build can be
    green but crash at boot on **OCRAM/heap starvation** (only ~2 KB free → SD/catalog malloc fails).
  - A **flapping COM port** (shows in `list_ports` but `open()` = "device does not exist") = the
    firmware is **boot-looping** (crashing in `setup()`), not a driver glitch.
  - `teensy_loader_cli` 1st try after HalfKay often prints `error writing` — just rerun (2nd works).
  - The `.pio/build/<env>/` dir got **wiped mid-flash** once → flash from a stable hex copy.
  - App holding COM4 → `Access denied`; ask the user to disconnect or press PROGRAM.
  - pio.exe: `/c/Users/jaysh/AppData/Roaming/Python/Python313/Scripts/platformio.exe`.
- **jay-mint** (Linux flash host, `jay@jay-mint.local` / IP `10.0.0.239`, pass `mint`): see
  `tools/linux-flash-host.md`. mDNS `.local` is flaky → **use the IP**. Stream src + changed `lib/TDsp*`
  over SSH; **don't overwrite the box's `platformio.ini`** (it has the `platform = teensy@5.1.0` pin) —
  APPEND the env instead. Teensy flash: `printf U > /dev/ttyACM0` → HalfKay → `teensy_loader_cli
  --mcu=TEENSY41 -w -v <hex>` (2nd try). If the running firmware is CRASHED, `U` won't work — needs a
  physical PROGRAM press at the box.

## 3. CLOSE-OUT — ship Phase 3 first (each = a user gate)
1. **USER audio-tests `teensy41_dexed_pool_4voice`** on COM4 (already flashed + running): 4-voice
   balance, per-voice ReplayGain loudness, MPE across voices (each 2-note), no clipping when all four
   stack. Also UI-test the app: 4 cards, each with the full submenu; the MIDI-input selector switches a
   synth's live device with **zero dropout** (the Thread C acceptance test).
2. **Merge `tracks-phase3-voices` → master** once the user signs off. (18 commits; a couple are parallel
   drum-note-map + tempo-follow work already on the branch — fine.)
3. **EAS rebuild** the app so the phone gets the 4-voice cards: `@jayshoes-team/tdsp-control`,
   `--profile preview`, `EXPO_TOKEN` + `EAS_NO_VCS=1` (see `reference_eas_tdsp`).
4. **Recover jay-mint:** get someone to press PROGRAM on its Teensy, then flash the fixed
   `teensy41_dexed_pool_jaymint_4voice_serial` (hex already built on the box).
5. **Per-voice ReplayGain sweep on 4voice** (optional): the 4voice env has `TDSP_DIAGNOSTICS=0` (DTCM
   budget), so the `N` sweep is off. Either make a diagnostics-on 4voice variant that fits (free DTCM
   elsewhere) or accept the unity per-voice tables until sizing allows it.

## 4. Thread D — heterogeneous engine inventory (the Phase-4 payoff)
### The idea (DESIGN.md §"Build-time engine inventory")
Teensy Audio objects + their `AudioConnection`s are STATIC — you can't `new` an engine at runtime. So
the **inventory** (how many engines of each kind, wired to the mix bus) is a BUILD fact; only the
**slot→Track binding** is runtime. Replace today's single-backend selection with per-type COUNTS:
```
-D TDSP_DEXED_ENGINES=8   ; the pool (windowed into Dexed slots, as today)
-D TDSP_OPLL_ENGINES=2    ; N OPLL FM instances (FM = cheap to multiply — CPU-bound, RAM-cheap)
-D TDSP_TSF_ENGINES=1     ; N TinySoundFont engines (RAM-bound — 2 needs a PSRAM core)
-D TDSP_SF2_ENGINES=0 / TDSP_PLAITS_ENGINES=0 / …
```
**Sizing rule (DESIGN, two axes):** FM/synthesis = CPU-bound (multi-instance fine); soundfont/sample =
RAM-bound (count scales with (PS)RAM). A config is valid only if it fits BOTH budgets; the runtime
should refuse/grey a slot config that exceeds either.

### The work
- **A `Slot` abstraction:** `{ engine-kind, engine-or-window, polyphony, cost{cpu,ram}, caps(melodic/
  drums/MPE) }`. A Track binds to a Slot (runtime). The Dexed pool is already "windowed into slots" —
  generalize that so an OPLL/TSF engine is *also* a slot. `Track.sink` already abstracts the engine
  binding; the new part is a **slot registry** + binding, not new per-Track plumbing.
- **Static wiring of N engines per kind** — an **X-macro / small codegen** so the engine array + each
  engine's `AudioConnection` to its sub-bus stays in sync with the count flags (hand-wiring 8 Dexed +
  2 OPLL + 1 TSF is unmaintainable). Each backend (`SynthBackend*.h`) grows from "the one engine" to
  "N engines + their bus taps," fed by its `TDSP_*_ENGINES` count.
- **Boot publishes the compiled inventory in `caps`/`tracks[]`** (kinds × counts); the app shows exactly
  this board's slots and lets the user **bind a Track to a slot** (a picker: "Synth C → OPLL #1"). This
  extends today's data-driven cards — the card already renders from `tracks[]`; add the slot/engine
  the track is bound to + a slot picker.
- **Named env presets** of the count flags (e.g. `..._4dexed_2opll`, `..._psram_2sf2`); default keeps
  today's shape. Start with a SMALL heterogeneous env to prove it (e.g. 2 Dexed + 1 OPLL) before big
  mixes — green-build it, serial-verify `@TRK0` drives Dexed and `@TRK2` drives OPLL, then USER audio.
- **The subscription hub (Thread C) already generalizes** across engines — a Track's live input feeds
  its router→arp→sink regardless of the sink's engine kind. So per-Track MIDI routing is done; this
  thread is about the SINK side (which engine each Track's sink is).

### Gotchas (from this session + the memories)
- **RAM is the wall.** Each song player buffer is ~6 B/event; TSF/SF2 hold sample data (MB); OPLL is
  RAM-cheap. Check DTCM **and** OCRAM after every inventory change (§2). The 4voice build needed
  `MAX_EVENTS3=2000` + diagnostics off just for 4 Dexed voices; adding a TSF slot on a no-PSRAM board
  likely won't fit — that's the [[project_core_opi_psram]] roadmap (32–64 MB OPI PSRAM).
- **F32 update order** ([[project_f32_update_order]]): the first hardware output constructed owns
  `update_responsibility` — declare `AudioOutputTDM_F32` FIRST or the whole F32 graph freezes.
- **`synthBegin`/expr-config/ReplayGain hooks are per-backend** — a mixed-engine build needs each
  engine's hooks called, not one backend's. The current `SynthBackend*.h` files assume ONE backend
  compiled; the inventory refactor must let several coexist (namespacing or an engine-kind dispatch).

## 5. Thread E — per-track mixer strip (P3.5)
Each Track already has `setLevel` + (pool) a per-voice trim node. Generalize to a real strip:
per-track level/mute (and optionally pan) → the Track's sub-bus → master limiter → out (the Dexed
`dxpTrim`/`dxpMix` pattern generalized to every engine kind). The app gets a mixer view (N faders +
mutes) reading `tracks[]`. Keyboard-owner is already the subscription hub (Thread C) — no separate work.

## 6. Suggested execution order
1. **CLOSE-OUT §3** — verify + merge + EAS + jay-mint. Ship the 4-voice payoff.
2. **Thread D, minimal** — the Slot abstraction + X-macro wiring + ONE small heterogeneous env
   (2 Dexed + 1 OPLL). Serial-verify per-Track engine binding; USER audio-tests a Dexed voice next to
   an OPLL voice. This proves the model on the cheapest mix.
3. **Thread D, app** — slot/engine binding picker on the data-driven card (reads `caps` inventory).
4. **Thread E** — per-track mixer strip + app mixer view.
5. **Bigger inventories** (TSF/SF2 slots) once a PSRAM core exists ([[project_core_opi_psram]]).
6. **BT/serial MIDI input sources** (the Thread C extension deferred in Phase 3): ESP32 forwards
   BLE-MIDI/serial-MIDI bytes to the Teensy → `hub.dispatch(SrcBtMidi/SrcSerial, ev)`.

## 7. Don't-break list
- master + the deployed boxes are the working state — only merge green + USER-verified.
- Keep the ESP32 relay verbatim (`@`-lines); new `@TRK<i>.*` just pass through.
- The subscription hub must stay repatch-free — no `loadVoice`/AudioConnection edit on a routing switch.
- Every firmware change: green-build opll + voice2 + drumvoice + 4voice; check DTCM **and** OCRAM (§2).
  Every app change: `npx tsc --noEmit`.
- `TDSP_SYNTH_VOICES<4` (voice2/drumvoice) must stay byte-identical; gate all new inventory code behind
  the count flags so existing single-backend builds are unaffected.
