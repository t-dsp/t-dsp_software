# Runtime drum-font swap — @DRUMFONT / @FONTS (contract)

Overnight feature. Lets the box switch which drum **SF2 file** is resident at runtime
(no reboot, no reflash), so the per-pack "…From Mars" fonts on the card
(`/sf2/mars_<pack>.sf2`, built by `tools/build_mars_kits.py --per-pack`) are testable live.

Today only the *kit within* the loaded font is runtime-selectable (ch10 program change via
`@DRUMKIT`/`setDrumKit`); the font FILE is fixed at boot in `DrumTsf.h::drumTsfBegin()`.
This adds file-level swap.

## SD layout (produced by the build tool — already on the card)
- `/sf2/drumkits.sf2` (+ `.tsv`) — boot default (unchanged path; DrumTsf still prefers it).
- `/sf2/mars_<pack>.sf2` (+ sibling `/sf2/mars_<pack>.tsv`) — one swappable font per pack.
- `/sf2/fonts.tsv` — manifest, cols: `path \t display \t kits \t bytes` (comment header `#`).
- Each `<font>.tsv` uses the existing drumkits.tsv columns: `program \t name \t license \t pieces \t display`.

## Firmware (firmware/mix-kit/src, guarded by `#if defined(TDSP_DRUM_TSF)`)
Env that compiles this path: `teensy41_dexed2_opll2_drums` (and `…_jaymint_serial`, the flash
target). Also green-build `teensy41_opll` (baseline fit floor). PSRAM board only — the whole
font is resident; ~6 MB ceiling on the 8 MB jay-mint board.

**`@DRUMFONT=/sf2/mars_909.sf2`** — swap the resident drum font. Runs in the command handler
(loop context, NOT the audio ISR). Implement as a FLASHMEM cold function `drumFontSwap(path)`:
1. `if (!SD.exists(path))` → print `[drumfont] not found` and return (keep current).
2. Silence + detach so the ISR can't touch a freed handle: `outL.gain(2,0); outR.gain(2,0);`
   then under `AudioNoInterrupts()` set the live handle pointer null (e.g. `g_drumTsfHandle=nullptr`
   — `TsfSink` derefs `*_t` each call, so this makes the sink a no-op) and `AudioInterrupts()`.
3. `tsf_close(old)` (capture old first), `tsf *nu = tsfLoadFromSD(path);`
   - on null: try to reload the previous path; print error; restore gains; return false.
4. Re-apply the exact setup `drumTsfBegin()` does: `tsf_set_output(nu, TSF_STEREO_UNWEAVED, rate, -4.0f)`,
   `tsf_set_max_voices(nu,24)`, per-ch `tsf_channel_set_presetnumber(nu,ch,0,ch==9?1:0)` +
   `tsf_channel_set_pitchrange(nu,ch,48)`, then publish: `g_drumTsfHandle=nu; g_drumTsf.begin(nu);`
   `g_drumTsf.setGain(1.0f);` restore `outL/outR.gain(2,0.62f)`.
5. Reload the sibling `.tsv`: generalize `loadDrumKitsTsv()` to take a path (derive `<font>.tsv`
   from the font path; fall back to GM names if absent). Reset `g_drumKit=0; drumApplyKit();`.
6. Track current font in a global `g_drumFontPath` / display; emit updated `@STATE` + a
   `[drumfont] loaded <path> (<n> presets, <k> kits)` line. Keep `g_drumFontIsKits` correct.

**`@FONTS`** — list available fonts for the UI. Read `/sf2/fonts.tsv` if present (else scan
`/sf2/*.sf2`). Emit a framed reply mirroring how `@DRUMS`/catalog lists are sent to the app
(same US=0x1f framing; include path, display, kits, current-flag). Also always include the
boot default `/sf2/drumkits.sf2`.

**`@STATE`** — add `drumfont`: current font path + display; add `caps.drumfontsel = (TDSP_DRUM_TSF
&& fontCount>1)`. ESP32 BLE relay: make sure the new @STATE field rides the existing chunked
@STATE/@APP relay (see [[project_ble_state_relay]]) so BLE clients see it too.

## App (app/tdsp-control)
- Parse the `@FONTS` reply + the `@STATE.drumfont` field (transport.* + state store).
- On the Drums card (gated by `caps.drumfontsel`), add a **Drum Font** picker listing the fonts;
  selecting one sends `@DRUMFONT=<path>`. After swap, the existing `@DRUMS`/`@STATE` refresh
  repopulates the kit list for the new font — no special-casing needed.
- Keep it a picker (switch, not layer), consistent with the two-persona UI.

## Constraints
- ISR-safety in the swap is the #1 risk — never `tsf_close` a handle the ISR may still read.
  The null-publish-under-AudioNoInterrupts step above is mandatory.
- Cold path → FLASHMEM (ITCM budget, see [[reference_itcm_boundary_cliff]]).
- Don't remove the A2DP/WiFi `#error` guard; don't touch unrelated in-progress edits.
- Do NOT run git (no commit/checkout/stash) — leave edits in the working tree on branch
  `mars-drumfonts`; the parent commits. Green-build only.
