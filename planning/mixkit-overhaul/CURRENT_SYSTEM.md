# mix-kit — Current System & UI (current-state reference)

A factual audit of **the mix-kit** as it exists today, written to inform a full
overhaul of how the system is organized in the UI. Two halves:

- **`firmware/mix-kit/`** — the device system (synths, players, loopers, drums,
  arp, tracks, master clock) exposing a line-based `@`-command protocol.
- **`app/tdsp-control/`** — the Expo/React-Native app (runs as web too) that
  drives it. This is the app you run on jay-mint at `localhost:19007`.

> **Not covered (deliberately):** `projects/t-dsp_tac5212_audio_shield_adaptor/tools/web_dev_surface/`
> — the older Vite/Electron **OSC mixer surface** (codec/mixer/spectrum over
> OSC, documented under `planning/ui-rebuild/`). That is a *different* frontend
> and a *different* protocol. It is **not** the mix-kit app and is out of scope here.

Line refs are `file:line` against the current tree; treat them as "as of this
audit," not eternal truth.

---

## 0. TL;DR for the overhaul

- The app is **one 2271-line `App.tsx`** with a home-grown string router
  (`home` → submenu → card), inline styles, fixed dark palette.
- **Three different things are all called "loop"** and they collide in the UI —
  this is the core confusion to fix. See §5.
- The user's loops (`/midi/loops`, 1,222 of them on jay-mint) currently surface
  **only** through the MIDI Player's `/midi` file browser (they play via the
  **song player**). The `@LOOPF` MIDI-looper path and the `loops` `catalog.tsv`
  manifest I added are **not wired into the active app** (see §6).
- `App.old.tsx`, the `@MANIFESTS`/`catalog.tsv` client code in `tdspBle.ts`, and
  several transport methods with no bound control are **dead/unused** (see §10).

---

## 1. Protocol & transports

Everything crosses the wire as **one `@`-command line per action** (`\n`-terminated).
The device answers/streams on the same link it heard the command on.

**Firmware dispatcher:** `handleControlLine(line, reply)` — `main.cpp:2554`–`3193`.
Called from the USB-CDC reader (`main.cpp:3798`) and the ESP32-UART/BLE relay
reader (`main.cpp:3884`). Two sub-dispatchers: `handleArpLine` (`main.cpp:2385`,
shared by `@ARP`/`@ARP2`/`@TRK<i>.ARP*`) and `handleTrkCmd` (`main.cpp:2466`, the
`@TRK<i>.*` family).

**App transports** — interface `Transport` in `src/transport.ts` (the de-facto
command catalog; every method has a one-line `@`-doc). Four implementations:

| Transport | Class | Link | When |
|---|---|---|---|
| `transport.web.ts` | `WebSerialTransport` ("USB") | Web Serial → Teensy CDC | **the web app uses this** |
| `transport.native.ts` | `BleTransport` ("BLE") | react-native-ble-plx → ESP32 GATT | native default |
| `transport.wifi.ts` | `WiFiTransport` ("WIFI") | `ws://tdsp.local:81/` → ESP32 | explicit `createTransport('wifi')` |
| `tdspBle.ts` | — | **legacy monolith** | dead except UUID/`CMD` exports consumed by native |

All four speak the identical `@`-line protocol; `send(line)` writes `line+'\n'`.

**File / browse / transfer primitives** (used for catalog + pickers + clips):

| Primitive | Request | Reply frames | Purpose |
|---|---|---|---|
| File read | `@READ=<path>` | `@FB`(begin+size) / `@FD`(b64 chunk, seq) / `@FE` / `@FERR` | fetch any SD file (one in flight) |
| Dir list | `@LS=<path>[\x1f<ext>]` | `@LB` / `@LD`(D/F rows) / `@LE` / `@LERR` | recursive folder browse (the FolderBrowser) |
| Dexed browse | `@DXLS=<rel>[\t<page>]`, `@DXVL=<cart>` | paged | live `/dexed` cart/voice browse |
| SD write | `@WB=<path>` + raw payload | `@WOK`/`@WERR`, verify `@CRC`→`@CRCR` | host→card push (USB only) |
| Clip out | `@RECDUMP=<v>` | `@FB/@FD/@FE` (raw bytes) | pull a looper clip |
| Clip in | `@RECLOAD=<v>\x1f<len>` → `@RD=` frames → `@RECEND=<v>` | `@RECOK`/`@RECE`/`@RECERR` | push an edited clip |

---

## 2. `@STATE` snapshot + `caps{}` (the app↔device contract)

`@STATE` returns one JSON line (builder `main.cpp:3039`–`3189`); the app parses it
in `hydrate(j)` (`App.tsx:576`). `@APP=<blob>` is an opaque app-owned store echoed
right after (`main.cpp:3189`). Live push feeds sit outside `@STATE`.

**`@STATE` top-level keys** → app state:

| key | contents | app var |
|---|---|---|
| `vol,hpf,bpm,quant,loop,metrolock` | scalars (`vol` = app *digital* master, distinct from codec `@VOL`) | `vol/hpf/bpm/quant/endMode` |
| `arp{on,pat,rate,oct,latch}` | main arp | `arp` |
| `song{playing,p,sync,vol,name}` | player 1 | `player`,`songVol` |
| `song2{…,loop}` | player 2 | `player2`,`endMode2` |
| `drums{kit,playing,sync,vol,map}` | drum track | `drums`,`drumVol` |
| `metro,metromuted,metrosig,metrovol` | metronome | `metro{…}` |
| `clock{beat,bpb,barp,run}` | transport/beat | via `@BEAT=`/`@BPM=` feeds |
| `rec{v,bars1,st1,p1,n1,max[,bars2,st2,p2,n2]}` | **MIDI looper** | `rec` |
| `aloop{sel,n,bars,mono,follow,st,p,cap}` | **audio looper** | `aloop` |
| `voice{cart,cv,name\|i,name}` / `voice2{…}` | selected instruments | `selVoice*`,`voice2` |
| `tracks[]{i,kind,eng?,playing,on,arp,src,srcch,name}` | per-track summary | `trkSubs/trkNames/synthCount` |
| `pool{preset,engines[4],active[4]}` | voice-pool split | `pool` |
| `caps{voice2,arp2,rec,recedit,tracks,audioloop}` | build-flag mirror | `caps` |

**Live push feeds** (not in `@STATE`): `@SONGP=`/`@SONG2P=` (position), `@TRK<i>.P=`,
`@ALP=` (audio-loop state+permille), `@RECP=` (MIDI-looper state+permille),
`@BEAT=i/n`, `@BPM=`, BT status JSON.

**`caps{}` gates** (from `@STATE.caps`, parsed `App.tsx:609`; default `App.tsx:512`
holds `{voice2,arp2,rec,recedit,audioloop}`):

| flag | type | gates in UI |
|---|---|---|
| `voice2` | bool | Synthesizer **B** card; MIDI Player 2 / Arp 2; keyboard-ownership glyphs |
| `arp2` | bool | Arpeggiator 2 child under Synth B |
| `rec` | bool | **MIDI LOOPER** tab in every MIDI Player |
| `recedit` | bool | **NOTE EDITOR** tab (needs `rec && recedit`) |
| `audioloop` | **int count** | **Audio Loop** card (`>0`); loop-selector pills (`>1`) |

Non-`caps` gates: `pool.has` → Voice Pool card + Synth C/D; `cat.hasDrums/hasBt/hasSf/hasDexed`
(from catalog meta) → Drums/Bluetooth cards + `/dexed` browser; `metro.cap` → Metronome parent.

---

## 3. App UI structure (`app/tdsp-control/App.tsx`)

**Active entry confirmed:** `package.json main = index.ts` → imports `./App`
(`App.tsx`) via `registerRootComponent`. `App.old.tsx` is dead.

**Navigation model:** a home-grown string router — `const [route,setRoute] = useState('home')`
(`App.tsx:472`). Model = **home grid → (section page | submenu → child page)**. No
nav library, no URL router, no back-stack beyond one `parent` level (Back always →
`parent || 'home'`, `App.tsx:2116`).

- Every screen is a **`Section`** object (`App.tsx:1237`): `{id,title,show,value,status,
  subtitle,actions,body,fullHeight,accent,tint,topRight,parent}`. Single source of
  truth = the `sections` array (`App.tsx:1703`–`1964`), ordered by `SECTION_ORDER`
  (`App.tsx:1970`).
- **`SubMenu`** (`App.tsx:298`) renders a parent's children as cards; **`BodyTabs`**
  (`App.tsx:282`) splits one card body into local tabs (MIDI Player, arp editor).

**Home cards** (`show`-gated, in order):

| Card (id) | gate | opens |
|---|---|---|
| Synthesizer A (`synthesizer`) | always | submenu |
| Synthesizer B (`synthesizerB`) | `caps.voice2` | submenu |
| Synthesizer C/D (`synthX2/3`) | pool `synthCount>v` | submenu |
| Drums (`drumtrack`) | `cat.hasDrums` | submenu |
| Audio Loop (`audioloop`) | `caps.audioloop>0` | page |
| Tempo (`tempo`) | always | submenu |
| Bluetooth (`bt`) | `cat.hasBt` | page |
| Settings (`settings`) | always | submenu |
| Voice Pool (`voicepool`) | `pool.has` | page |

**Submenu tree:**

- **Synthesizer A** → MIDI Player (`player`) · Synth/Voices (`synth`) · Arpeggiator (`arp`)
- **Synthesizer B** → Synth/Voices 2 · MIDI Player 2 (`caps.voice2`) · Arp 2 (`caps.arp2`)
- **Synthesizer C/D** → Synth/Voices · MIDI Player · Arp (all wired `@TRK<v>.*`)
- **Drums** → Drum Loops (`drumloops`) · Kit (`drumkit`)
- **Tempo** → Tempo/BPM (`bpm`) · Metronome (`metro`)
- **Settings** → Connection (`conn`) · TAC5212 (`codec`)

**Global header** (every screen, `App.tsx:1985`): brand + connect dot;
Connect/Disconnect App; status line; **BeatStrip** lights; **master transport row**
(▶ `@METRO=1`, ■ `@METRO=0`, 🔇/🔊 `@METROMUTE=`, −/＋ `@BPM=`, 🔒 `@METROLOCK=`)
and a master **VOL** slider `@VOL=`.

### Cards → controls → commands (condensed)

**MIDI Player** (`player`, built from `makeSongDeck`, `App.tsx:1119`). Header transport
`playerActions` drives the **song** on all tabs: ‹/› `@SONGRESTART=`, ▶ `@SONGRESTART=`,
■ `@SONG=stop`, end-mode 🔀🔁➡◻ → Repeat sends `@LOOP=1` else `@LOOP=0` + app-side
advance (+`@APP=` persist). Deck 2 uses `@SONG2*`/`@LOOP2=`. Body = **`BodyTabs`**:
1. **MIDI PLAYER** — Volume `@SONGVOL=`; **FolderBrowser** root `/midi` ext `mid`, file
   tap → `@SONGRESTART=<fullpath>`; "When finished" pills → `@LOOP=` + `@APP=`.
2. **MIDI LOOPER** (`caps.rec`) — the recorder, see below.
3. **NOTE EDITOR** (`caps.rec&&recedit`) — `PianoRoll`, `@RECDUMP`/`@RECLOAD`.

**FolderBrowser** (`App.tsx:325`) — the `@LS` client. Root `/midi`; lists folders/files
via `@LS=<path>`; folder tap descends, file tap → `onSelectFile(fullpath, dispName)`.
Baked/built-in songs injected as a synthetic "built-ins" folder. Selected row shows
`♪ ` when playing; card title = `♪ ` + `player.name`. **This is the only place loops
surface today** (the `loops` folder appears alongside `songs/drums/tests`).

**MIDI LOOPER tab / `recRow`** (`App.tsx:1357`) — per-voice recorder. Each button first
sends `@RECV=<v>` then: ● Record `@REC=1`, ＋ Overdub `@RECDUB=1`, ■ Stop `@REC=0`,
✕ Clear `@RECCLR`, length pills `@RECBARS=<n>`. State label from `REC_STATES`
`['Idle','Armed — play a note','Recording','Overdubbing','Looping']` (`App.tsx:78`),
fed by `@RECP=`. **No "load a loop file" control and no clip-name/title display here.**

**Audio Loop** (`audioloop`, `App.tsx:1824`, gated `caps.audioloop>0`): slot pills
`@ALSEL=`, bars `@ALBARS=`, Mono `@ALMONO=`, Follow `@ALFOLLOW=`, ● Record `@AL=1`,
⊕ Overdub `@ALDUB=1`, ■ Stop `@AL=0`, Save `@ALSAVE=` (→ `/loops/<name>.wav`), Clear
`@ALCLR`. States `AL_STATES` `['Empty','Armed…','Recording','Overdubbing','Looping']`,
fed by `@ALP=`. **Saved `.wav` loops are never listed/reloaded in the UI.**

**Synth/Voices** (`synth`, `App.tsx:1479`): ‹/› `pickVoice`; Volume `@SONGVOL=`;
`voiceBrowserBody` = a **different** browser over `@DXLS`/`@DXVL`; pick →
`@DXPICK=<cart>\t<v>` or `@DXVOICE=<i>`; keyboard glyph `@VOICE2=0/1`; appends the
**MIDI Input** selector (`@TRK<i>.SRC=`, `@TRK<i>.SRCCH=`). Voice 2 mirrors with
`@VOICE2VOL=`/`@DXPICK2`/`@DXVOICE2`. Synth C/D use `@TRK<v>.{VOL,DXPICK,DXVOICE}`.

**Arpeggiator** (`arp`, `App.tsx:1040`): ▶/■ `@ARPON=1/0` (`@ARPRESTART` if running),
Latch `@ARPLATCH=`; **Presets** tab → `@ARPPRESET=` (atomic); **Manual** tab → pattern
grid `@ARPPAT=` (+`@ARPSEQ=` for the user-sequence pattern), Rate `@ARPRATE=`, Octaves
`@ARPOCT=`. Arp 2 is the clone on `@ARP2*`.

**Drums** (`drumtrack`): reuses `makeSongDeck` over a `{kit,sel}` view. Drum Loops
child = FolderBrowser `/midi`, tap → `@DRUMF=<name>`; Volume `@DRUMVOL=`; always loops.
Kit child = kit pills `@DRUMKIT=<i>`.

**Tempo/BPM** (`bpm`): BPM `@BPM=`; Launch-quantize `@QUANTIZE=`.
**Metronome** (`metro`): ▶/■ master transport; Hear-click `@METROMUTE=`; Lock
`@METROLOCK=`; Click vol `@METROVOL=`; Time-sig pills `@METROSIG=<n>`.
**Voice Pool** (`voicepool`): four presets `@POOL=<0..3>`.
**Bluetooth** (`bt`): connect/pair/forget/disconnect ESP32 A2DP (`P/r/X/F` or `!wifi`).
**Settings → Connection** (`conn`): catalog stats; Rebuild `@REINDEX`.
**Settings → TAC5212** (`codec`): Output vol `@VOL=`; HPF enable + cutoff `@HPF=<0-3>`.

---

## 4. Firmware subsystems & signal path

**Per-voice arrays** (sized `kSynthVoices = TDSP_SYNTH_VOICES`):
- `g_playerV[]` (`MidiFilePlayer`) — song players; `g_player=[0]`, `g_player2=[1]`.
- `g_arpFilterV[]` (`ArpFilter`) — one arp/voice.
- `g_routerV[]` (`MidiRouter`) — one live-MIDI router/voice (MPE-normalizes).
- **Drums:** `g_drumPlayer` → `g_drumNoteMapper` (ch10 remap) → drum sink.
- **Loopers:** `g_loop1`/`g_loop2` (`MidiLooper`); `g_aloop[]` (`AudioLooper`).

**Master clock (the "master section"):** `tdsp::Conductor g_conductor` (`main.cpp:257`,
`lib/TDspTempo`). Owns THE BPM + transport + a free-running 24-PPQN `Clock`. Two
consumer models:
1. **Tempo followers** — `PlayerFollower` adapters wrap each song/drum player
   (`g_songFollow`, `g_drumFollow`, …) registered via `addFollower()`; they get
   `onBpm/onStart/onStop/onBarEdge`. `applyTempos()` (`main.cpp:1097`) is the single
   tempo write path.
2. **Tick consumers (24-PPQN)** — the arp and the MidiLoopers read the master Clock
   directly, so loop/arp downbeats line up with song+drums. External MIDI clock
   (0xF8) can slave the kit via `g_clockSink`.

**MIDI signal path** (`trackWireSetup`, `main.cpp:3303`):
```
live MIDI IN (DIN / USB-host / BT / Serial)
  → per-source callbacks → midihub:: fan-out (per Track: subscribed? channel ok? voice live?)
  → per-track MidiRouter (MPE-normalize)
  → [ArpFilter]            (bypass = forward verbatim)
  → synth sink
```
- The **song player** also feeds *through* the arp (`t.player->setSink(arp?arp:sink)`).
- The **MidiLooper taps DOWNSTREAM of the arp** (`arp->addDownstream(looper)`) so it
  captures the *baked* post-arp stream; playback goes straight to the synth sink so a
  loop never re-enters the arp (no double-arp).
- Source subscription is a pure field write (`midihub::setSources`) — switch a
  keyboard between voices with zero audio-graph repatch.

**Audio graph:** synth/drum sinks → F32 mix bus → `outL/outR` record bus →
audio-loop tap+return → `finalL/finalR` → TAC5212.

**Tracks (Phase 3):** `g_tracks[]` + `g_drumTrack` (`src/Track.h`). `tracksInit()`
binds each Track's player/arp/router/looper/sink pointers + a per-track `caps{}` of
behavior flags. This is what lets the app render N uniform cards from `@STATE.tracks[]`.

---

## 5. The THREE "loop" concepts (the crux of the confusion)

| | **Song-player loop** | **MIDI looper** | **Audio looper** |
|---|---|---|---|
| Command | `@LOOP=` / `@LOOP2=` | `@REC*` / `@LOOPF` | `@AL*` |
| Object | `MidiFilePlayer` | `tdsp::MidiLooper` (`g_loop1/2`) | `tdsp::AudioLooper` (`g_aloop[]`) |
| What it loops | a whole **song file** | a **per-voice MIDI clip** (24-PPQN, 1/2/4/8 bar) | recorded **device audio** (stereo/mono) |
| Filled by | pick a `.mid`, Repeat on | **record live playing** OR **`@LOOPF` load an SD loop file** | record the output bus |
| `@STATE` | `song.loop` | `rec{}` (caps.rec) | `aloop{}` (caps.audioloop) |
| UI today | MIDI PLAYER tab (Repeat) | **MIDI LOOPER tab** (recorder only; no file-load, no title) | **Audio Loop card** |
| lib | — | `lib/TDspMidiLoop` | `lib/TDspAudioLoop` |

They share the word "loop" but are three unrelated subsystems. The overhaul needs to
decide how (or whether) each is surfaced, and stop them bleeding into each other. The
user's stated intent: **loops are accessed via the MIDI player**, shown with their
title; keep "looper" as a concept but not as its current recorder-only tab; the audio
looper is separate.

---

## 6. The loops feature (added this session) — exact status

**Tooling:** `tools/fetch_loops.py` extracts short **melody + bass** loops from any SMF
(sources: `cc0` CC0, `bitmidi` by genre, `nesmdb`/`jsb`/`pop909`, `local`), writes
`<out>/loops/<source>/…` + a `catalog.tsv` (`relpath,source,genre,role,bpm,display`).
`sync_assets.py --loops-src` (or `fetch_loops.py --push`) pushes them to `/midi/loops`.

**Firmware added:** `loadLoopFileIntoLooper()` (`main.cpp:1236`) + command
`@LOOPF=<v>\x1f<path>` (`main.cpp:2755`) — loads a `/midi/loops/*.mid` into voice v's
`MidiLooper` (24-PPQN resample, note on/off only) and plays it bar-locked. Also emits a
`loops` manifest row (`|loops\x1ffile:/midi/loops/catalog.tsv`, `main.cpp:1987`,
recorder builds only). Green on `teensy41_opll` + voice2 envs.

**On the device (jay-mint, SN 7681380):** flashed `teensy41_opll_jaymint_serial`
(new env: `teensy41_opll` + `USB_SERIAL` + `boards/jaymint.h`). **1,222 loops** live at
`/midi/loops/{cc0,bitmidi/<genre>,pop909}/…` + `catalog.tsv`, CRC-verified.

**What actually works / doesn't in the app:**
- ✅ Loops appear in the **MIDI Player's `/midi` FolderBrowser** (`@LS` lists the
  `loops` folder → sources → files). Tapping one plays via **`@SONGRESTART=`** (the
  **song player**) and shows the **title** in the player card; Repeat loops it.
- ⚠️ **`@LOOPF` (the MIDI-looper path) is NOT wired into the active app.** Loading a
  loop that way plays it (verified: `outPeak` up) but it only shows as "Looping" in the
  MIDI LOOPER tab with no title — which is why it felt invisible.
- ⚠️ **The `loops` `catalog.tsv` + `@MANIFESTS` are unused by `App.tsx`.** The active
  app browses via `@LS`, not manifests (manifest client is dead code in `tdspBle.ts`).
  The `catalog.tsv` (genre/role metadata) is on the card but nothing reads it yet.

**The confusion you hit:** you were on the **Audio Loop** card (empty, dead buttons —
different subsystem), while the loop I'd loaded went to the **MIDI looper** via
`@LOOPF`. The right home is the **MIDI Player** (song-player path), which already works.

---

## 7. Full `@`-command surface (grouped)

**Master/output:** `@VOL=` (codec master), `@HPF=`, `@RG=`/`@RG` (ReplayGain),
`@APP=`/`@APP`, `@STATE`.
**Song player:** `@SONGF=`, `@SONGRESTART=`, `@SONG=<idx|stop>`, `@LOOP=`, `@SONGVOL=`;
voice-2: `@SONG2F=`, `@SONG2RESTART=`, `@SONG2=stop`, `@LOOP2=`.
**MIDI looper:** `@RECV=`, `@RECBARS=`, `@RECSIG=`, `@REC=`, `@RECDUB=`, `@RECCLR`,
`@RECPLAY=`, **`@LOOPF=`**, `@RECDUMP=`, `@RECLOAD=`, `@RD=`, `@RECEND=`.
**Audio looper:** `@ALSEL=`, `@ALBARS=`, `@ALMONO=`, `@ALFOLLOW=`, `@ALLEVEL=`, `@AL=`,
`@ALDUB=`, `@ALCLR`, `@ALPLAY=`, `@ALSAVE=`.
**Drums:** `@DRUM=<idx|stop>`, `@DRUMF=`, `@DRUMKIT=`, `@DRUMMAP=`, `@DRUMVOL=`,
`@DRUMSYNCHRO=`, `@DRUMJIT=`. *(No `@DRUMSPEED` — drum tempo = master BPM.)*
**Arp:** `@ARPON=`, `@ARPRESTART`, `@ARPPAT=`, `@ARPSEQ=`, `@ARPRATE=`, `@ARPGATE=`,
`@ARPSWING=`, `@ARPOCT=`, `@ARPLATCH=`, `@ARPPRESET=` (+ `@ARP2*`, `@TRK<i>.ARP*`).
**Synth/voice:** `@DXVOICE=`, `@DXLS=`, `@DXVL=`, `@DXPICK=`, `@LFOMODE=`, `@POOL=`,
`@MIDIMODE=`, `@PRESSURE=`, `@MODWHEEL=`, `@TIMBRE=`; voice-2 `@VOICE2=`, `@VOICE2VOL=`,
`@DXVOICE2=`, `@DXPICK2=`.
**Tracks:** `@TRK<i>.{PLAY,SONGF,RESTART,STOP,VOL,LOOP,ARP*,SRC,SRCCH,INSTR,DXPICK,DXVOICE}`.
**Clock/metro:** `@BPM=`, `@METROLOCK=`, `@QUANTIZE=`, `@METRO=`, `@METROMUTE=`,
`@METROSIG=`, `@METROVOL=`.
**Catalog/transport:** `@GETCAT`, `@REINDEX`, `@READ=`, `@LS=`, `@WB=`, `@CRC=`, `@FXUP`.
**ESP32/programming (single-char):** `g` (flash), `r` (reset→app), `U` (Teensy program),
`@BOOTAPP@` (exit flash), `P/F/X` (pair/forget/disconnect A2DP).

---

## 8. SD card layout

```
/midi/songs/*.mid           songs        → /tdsp/songs.ndjson
/midi/drums/**.mid          grooves      → /tdsp/grooves.ndjson  + /midi/drums/catalog.tsv
/midi/loops/**.mid          melody/bass loops (this session)     + /midi/loops/catalog.tsv  (unused by app)
/midi/tests/                test MIDI
/tdsp/*.ndjson              on-device catalog DB (index/instruments/grooves/songs/soundfonts/drumkits/samples)
/sf2/*.sf2                  SoundFonts (e.g. gm_tsf.sf2)
/dexed/**.syx               ~3700 DX7 carts (browsed LIVE via @DXLS/@DXVL, never held in RAM)
/samples/<bank>/            sample banks
/loops/*.wav                audio-looper saves (@ALSAVE)   ← NOTE: /loops (audio) ≠ /midi/loops (MIDI)
```

> Naming trap for the overhaul: **`/loops/*.wav`** (audio looper output) is a *different
> folder* from **`/midi/loops/*.mid`** (MIDI loops). Another collision to clean up.

---

## 9. Build envs & feature flags (`platformio.ini`)

`[common]`: F32/48 kHz, `USB_MTPDISK_SERIAL`, `TDSP_HAS_I2C_MUX`.

**Synth backend (pick one):** `TDSP_SYNTH_{DEXED,DEXED_POOL,YMFM,PLAITS,RINGS,VA,OPL3,
OPLL,OPLL_POOL,SF2,SF2_TSF}`.

**Feature flags (mirrored into `@STATE.caps`):**
- `TDSP_RECORDER` → MIDI looper (`@REC*`/`@LOOPF`); `TDSP_RECORDER_EDIT` → clip editor.
- `TDSP_VOICE2` → second synth voice; `TDSP_ARP2` → keyboard arp.
- `TDSP_AUDIOLOOP` (+ `_N`, default 2) → audio looper.
- `TDSP_METRONOME` → master-transport click.
- `TDSP_DRUM_TSF` (PSRAM) vs `TDSP_DRUM_VOICE` (OPLL, no-PSRAM).
- `TDSP_SYNTH_VOICES=N` / `TDSP_HETERO` → fixed N-way / mixed-engine.
- `TDSP_NO_SPDIF_IN` → drop optical-in resampler (~87 KB DTCM).
- `TDSP_BOARD_HEADER` → board profile (`jaymint.h`, `digital_audio_board_nobt.h`, …).
- `USB_MTPDISK_SERIAL` vs `USB_SERIAL` → MTP+Serial composite vs plain serial (Linux
  flash hosts need `USB_SERIAL`; MTP call sites are `#ifdef`-guarded).

Representative envs: `teensy41` (Dexed); `teensy41_dexed_pool_nobt_voice2` (fullest:
recorder+audioloop+voice2+arp2); `teensy41_opll` ("fits anywhere" + recorder+audioloop+
metronome); `teensy41_opll_jaymint_serial` (this session — OPLL + USB_SERIAL + jaymint,
flashed on jay-mint).

---

## 10. Overhaul notes — sources of confusion & observations

**Naming collisions to resolve**
- Three "loops": song-player `@LOOP`, MIDI looper `@REC`/`@LOOPF`, audio looper `@AL`
  (§5). Plus **`/loops/*.wav` vs `/midi/loops/*.mid`** (§8).
- "MIDI LOOPER" tab is really a *recorder*; it neither loads loop files nor shows a
  title, so a loaded loop is invisible there.

**Player ↔ looper overlap (the real design question)**
- Loops currently play through the **song player** (via the `/midi` browser). That's
  what the user wants — title in the player card, loop with Repeat.
- The **MIDI looper** is genuinely different (per-track, records live playing, overdub,
  bar-tight seam). Decide whether loops should *ever* route through it, or whether the
  looper stays purely a live-capture tool.

**Dead / unwired code (safe to delete or ignore in the rebuild)**
- `App.old.tsx` — not imported.
- `@MANIFESTS` + `catalog.tsv` client in `tdspBle.ts` — the active app uses `@LS` +
  `/tdsp/*.ndjson`, never manifests.
- The firmware `loops` manifest + `/midi/loops/catalog.tsv` — emitted/present but
  **no app consumer** (genre/role metadata currently wasted).
- Unbound transport methods: `recSig`/`@RECSIG`, `recPlay`/`@RECPLAY`, `audioLoopLevel`/
  `@ALLEVEL`, `audioLoopPlay`/`@ALPLAY`, `arpGate`/`arpSwing` (preset-only). Saved audio
  `/loops/*.wav` are never browsed/reloaded.

**Structural observations**
- One 2271-line `App.tsx`, inline styles, single-string router, one-level back-stack.
- Heavy, *good* reuse seams to preserve: `makeSongDeck` (players/drums/extra voices),
  `ArpSlotT`, `FolderBrowser`, `voiceBrowserBody`.
- **Two browsers coexist:** `FolderBrowser`/`@LS` (songs, grooves, loops) vs
  `voiceBrowserBody`/`@DXLS`+`@DXVL` (instruments) — candidate for unification.
- The device already exposes rich per-track state (`@STATE.tracks[]` + Track `caps`),
  so a track-centric UI is well-supported by the firmware if the app reorganizes around it.

**If loops get a first-class home in the overhaul**
- Cheapest path (works today): keep them in the MIDI Player's `/midi` browser; maybe
  surface `loops` as a prominent shortcut and read `catalog.tsv` for genre/role filtering.
- The `catalog.tsv` (`source/genre/role/bpm`) is already on the card and ready to drive
  a real genre/role picker if the app ever reads a manifest again.
```
