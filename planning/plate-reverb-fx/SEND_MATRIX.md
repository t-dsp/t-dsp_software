# FX Send Matrix — plan (per-voice aux sends)

Convert the reverb from a master **INSERT** (whole mix reverbed) to a per-voice **aux SEND**
bus: each track keeps its **dry** path to the master and gains an independent **send** into the
reverb; the reverb's **wet** output returns to the master. `send=0` ⇒ dry (bypasses reverb),
`send>0` ⇒ reverb on that track.

## Decisions (locked)
- **Granularity: per-voice** — independent send for **Synth A, B, C, D, Drums, Audio Loop** (6).
- **Default: pure send** — every track starts dry; you dial reverb per track. The global reverb
  On/Off gates the whole bus; the current "Mix" slider becomes the wet **Return** level. (No
  INSERT mode kept — `@FX.ROUTE` not needed for now.)
- Effect-agnostic: this routes tracks→bus regardless of which effect is on the bus. Building the
  other hexefx effects is **not** required for the matrix; they're a later expansion (extra bus
  slots / additional buses). See DESIGN.md §1 for the catalog.

## Tap points (verified — no engine surgery needed)
| Track | Tap node (pre master-sum) |
|---|---|
| Synth A / B (Dexed) | `dxpTrim*` (per-voice pool trims, DexedPoolSink.h) |
| Synth C / D (OPLL)  | `g_hoOpllTrim[0]` / `[1]` (HeteroOpll.h, before `g_hoMixL/R`) |
| Drums               | the TSF drum-out node (DrumTsf) |
| Audio Loop          | the loop return (finalL/R feed) |

## Graph change
```
each track ──(dry, unchanged)──────────────────► outL/outR ─► final ─► post ─► DAC
each track ──(SEND gain)──► fxInL/fxInR ─► reverb(100% wet) ─► fxRet(return gain) ─► post/final ─► DAC
```
- `fxInL/fxInR` = a **cascaded** F32 mixer (6 sources > 4 slots → e.g. `fxIn1` 4-in + `fxIn2` = fxIn1 + 2 more).
- Reverb set `dry_level=0, wet_level=1` (dry lives on the master already).
- Return lands in the **post** mixer (a free slot) — downstream of the audio-loop record tap, so
  the wet tail isn't re-recorded. (Loop is still *sendable* via its own tap.)
- Gate everything behind **`TDSP_FX_SEND`** so INSERT-only builds stay byte-identical.

## Protocol
- **`@TRK<i>.FXSEND=<0..100>`** — per-track send level (in `handleTrkCmd`, beside `VOL=`; uniform,
  reuses the app's `trk()` path; works for any current/future track for free).
- **`@FX.RETURN=<0..100>`** — global wet return level (the old "Mix").
- `@STATE`: add `"fxsend":<0..100>` to each `tracks[]` entry so the app hydrates on connect.

## App (Send Matrix UI)
- On the Reverb card: a **matrix** — one **send slider per track** (rows: A, B, C, D, Drums,
  Audio Loop; label from `trkNames`/kind), `onCommit → trk(i,'FXSEND='+v)`, hydrated from
  `@STATE.tracks[].fxsend` into a `trkSend` map.
- Keep global On/Off + Return + reverb params (size/damp/etc). Structurally like the per-track
  volume rows already on the synth cards; throttled via the existing `ThrottledSlider`.

## Phases
1. **Firmware** — DONE. `TDSP_FX_SEND`: fxIn1+fxIn2 cascade + per-track send taps (dxpTrim/dxpTrimB
   → fxIn1[0/1], g_hoOpllTrim[0/1] → fxIn1[2/3], drum TSF/OPLL → fxIn2[1], g_aloop[0] → fxIn2[2]);
   reverb 100% wet → `post` slot 3 return. `@TRK<i>.FXSEND`, `@FX.RETURN` (+`@FX.MIX` alias),
   `@FX.LOOPSEND`; `@STATE.fx.{route,return,loopsend}` + `tracks[].fxsend`. Built green: opll canary,
   spring-send (TSF + OPLL drums / usbaudio local), plate-send PSRAM (jay-mint). Also fixed a
   pre-existing LDF over-inclusion (USB_Audio_F32_24 compiled into non-USB envs) via `lib_ldf_mode = chain+`.
2. **App** — DONE. Reverb card branches on `fx.route`: send mode renders a send slider per track
   (Synth A-D · instrument, Drums, Audio Loop) + wet Return + shared character params; insert mode
   unchanged. `trkSend` hydrated from `@STATE.tracks[].fxsend`; return/loopsend from `@STATE.fx`.
   Home tile quick-slider is Return in send mode, Mix in insert. tsc-clean.
3. **HW verify** — per-track sends audibly correct; dry tracks stay dry; loop send works (PSRAM board).

## Still open (pre-existing)
- jay-mint plate/PSRAM flash + verify.
- Audio-verify the metronome reroute (best on jay-mint — it has the audio looper).
- teensy41 default env pre-existing RAM1 overflow (SPDIF-in DTCM) — unrelated; use `teensy41_opll` as canary.
