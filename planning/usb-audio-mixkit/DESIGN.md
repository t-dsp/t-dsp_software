# USB Audio in mix-kit — design & plan

Add a build-flag-gated **24-bit / 48 kHz USB Audio interface** to `firmware/mix-kit`, so the
box appears to a host computer as a class-compliant sound card: **host audio flows into the
mix bus** (DAW/Spotify/backing tracks → TAC5212 out) and **the full mix streams back to the
host** for recording — plus **native USB-device MIDI** (a DAW can play the synths) as a free
bonus of the required USB descriptor.

**Status:** proposal — not yet implemented
**Owner:** Jay
**Last updated:** 2026-07-21

---

## 1. The one-line pitch

We already did the hard part. The full-precision USB-audio path — the F32-native
reimplementation of alex6679's 24-bit UAC2 idea — is a finished, loopback-validated library
(`lib/USB_Audio_F32_24/`) sitting on a patched Teensy core (`lib/teensy_cores/teensy4/`).
And because mix-kit's `[common]` build already runs `cores_overlay.py`, **that patched core
is already copied into every mix-kit build today** — the 24-bit USB descriptor, the ISR
routing, and the Q31 ring buffers are all compiled in, just **dormant** (gated on
`AUDIO_SUBSLOT_SIZE==3 && AUDIO_INTERFACE`, neither of which mix-kit currently sets).

> **So this is not a port. It's flipping two flags and adding two audio objects.**

## 2. The state of the art (what "we did a lot of work" produced)

Confirmed by reading the tree:

- **`lib/USB_Audio_F32_24/`** — F32-native USB Audio Class endpoints:
  - `AudioInputUSB_F32` — host → Teensy (USB OUT endpoint), F32 output pins, `volume()`/`mute()`
    from UAC Feature Unit, `getStatus()` (rx_overruns / tx_underruns).
  - `AudioOutputUSB_F32` — Teensy → host (USB IN endpoint), F32 input pins.
  - Both are **no-ops unless `AUDIO_SUBSLOT_SIZE==3`**, so linking them costs nothing when off.
- **`lib/teensy_cores/teensy4/`** (overlaid by `cores_overlay.py` — the *same* mechanism
  mix-kit already uses):
  - `usb_audio_f32_buffers.{h,cpp}` — two lock-free SPSC stereo rings in DMAMEM. **24-bit
    samples stored left-justified in `int32` (Q31)** so `arm_q31_to_float` / `arm_float_to_q31`
    convert with no manual scaling — the whole point of the F32 path is *no int16 round-trip*.
  - `usb_audio.cpp` — stock PJRC ISR patched so that under `AUDIO_SUBSLOT_SIZE==3` the rx/tx
    events feed the Q31 rings instead of the int16 deinterleave. Below that flag it is
    **bit-identical stock int16** — zero risk to existing behaviour.
  - `usb_desc.{h,c}` — the descriptor patch: under `AUDIO_SUBSLOT_SIZE==3` it re-sizes the
    isochronous endpoints to `2ch × 3B × (48+1 jitter) = 294 B/ms`. Flag unset ⇒ stock sizing.
- **Provenance:** `vendored.json` pins PaulStoffregen/cores `7659e412…`; upstream inspiration
  is `references/teensy-4-usbAudio/` (alex6679 / Alex Walch's UAC2 24-bit/multichannel work).
- **Already proven end-to-end** in three projects: `projects/spike_f32_usb_loopback/` (env
  `teensy41_f32_24`, milestones M1–M5), `projects/teensy_usb_audio_tac5212/`, and
  `projects/t-dsp_f32_audio_shield/` — all TRUE 24-bit F32 USB↔TAC5212 loopbacks.

The precedent flag block (from `spike_f32_usb_loopback/platformio.ini`):

```ini
-D USB_MIDI_AUDIO_SERIAL      ; USB device = Serial(CDC) + native MIDI + Audio
-D AUDIO_SUBSLOT_SIZE=3       ; the single master switch for the 24-bit F32 path
-D AUDIO_SAMPLE_RATE_EXACT=48000.0f
-D AUDIO_BLOCK_SAMPLES=128
```

Sample rate (48 kHz) and block size (128) **already match mix-kit's `[common]`**. The library
is F32-native, so it plugs straight into mix-kit's F32 mix bus — no `AudioConvert` bridge like
the int16 BT/synth sources need.

## 3. The one real cost: USB descriptor type (MTP ⇄ Audio)

This is the only genuine trade-off, and it's forced by Teensy's USB descriptor design, not by us.

- mix-kit's default USB type is **`USB_MTPDISK_SERIAL`** (Serial + MTP, so the SD card mounts
  on the host). The `*_jaymint_serial` envs already drop that for plain **`USB_SERIAL`**.
- **No stock Teensy USB type exposes both MTP and Audio.** Verified in the vendored
  `usb_desc.h`: the only descriptor block carrying `AUDIO_INTERFACE` alongside an MTP interface
  is `USB_EVERYTHING`, and there **MTP is commented out** (endpoint exhaustion). So a USB-audio
  build **cannot also be an MTP build**.
- **Verdict: acceptable, and barely a loss.** Bulk SD asset transfer already has a first-class
  non-MTP path — the `@WB`/`@WRITE` USB-CDC primitive + `push_file_serial.ps1` /
  `tools/sync_assets.py` (see [[project_fast_sd_transfer]], [[reference_copy_files_to_sd]]).
  We give up "SD mounts as a drive" and gain a 24-bit sound card. For the card-reader bulk case
  nothing changes.
- **Bonus, not cost:** the chosen type `USB_MIDI_AUDIO_SERIAL` also brings **native USB-device
  MIDI**, which mix-kit does *not* have today (its MIDI is USB-**host** + DIN only). A DAW
  plugged into the box can now drive the synths directly. We should wire that in under the same
  flag (see §4, step 4).

## 4. The build flag & wiring

**Umbrella flag: `TDSP_USB_AUDIO`.** Setting it in an env does four things:

1. **Swap the USB type.** In that env: `build_unflags = -D USB_MTPDISK_SERIAL` and
   `build_flags += -D USB_MIDI_AUDIO_SERIAL -D AUDIO_SUBSLOT_SIZE=3`. (The `cores_overlay.py`
   that ships the patched descriptor is already in `[common]` — nothing to add there.)
2. **Add two graph objects** in `main.cpp`'s declaration block, after `tdmOut` (which must stay
   the first-constructed audio object — [[project_f32_update_order]]):
   ```cpp
   #if TDSP_USB_AUDIO
   AudioInputUSB_F32  usbIn(g_audioSettings);   // host -> Teensy, F32-native
   AudioOutputUSB_F32 usbOut(g_audioSettings);  // Teensy -> host (records the mix)
   #endif
   ```
3. **Route it into the existing graph** (no new int16 convert — it's F32-native):
   - **USB in → mix bus.** `AudioMixer4_F32 outL/outR` has 4 slots, all assigned
     (`0=BT, 1=tone, 2=S/PDIF-in, 3=synth`). USB-in is another external digital line source
     exactly like S/PDIF-in, so **v1 reuses slot 2**: `TDSP_USB_AUDIO` implies `TDSP_NO_SPDIF_IN`
     (you pick optical *or* USB as the digital line-in — you can't feed both a 4-slot mixer
     without a sub-mixer). Connections mirror the `c_spL/c_spR` pair. A later revision can add a
     2-in sub-mixer to run USB-in and optical simultaneously if anyone ever wants it.
   - **USB out ← master.** Tap the master bus the same way the audio-loop / `peakOut` /
     `OutCaptureProbe_F32` taps do (post-mix, pre- or post-FX — recommend **post-FX**, tapping
     `finalL/finalR` when present else `outL/outR`, so the host records what you hear). The host
     gets a 24-bit stereo recording of the whole performance.
4. **Wire native USB-device MIDI** (comes with `USB_MIDI_AUDIO_SERIAL`): add a `usbMIDI`
   read-drain next to the existing USB-host / DIN readers in `loop()`, feeding the same
   `g_midiRouter` sink chain. Gate on `TDSP_USB_AUDIO` (or a sibling `TDSP_HAS_USB_DEVICE_MIDI`).
   Per-track source selection already exists (`@TRK<i>.SRC`, [[project_usb_midi_track_button]]),
   so USB-device MIDI slots in as one more selectable source.

**Defaults & caps** (follow the house pattern):
- `#ifndef TDSP_USB_AUDIO / #define TDSP_USB_AUDIO 0` near the other feature defaults in
  `main.cpp`, so every existing env stays byte-identical.
- Add `TDSP_HAS_USB_AUDIO` to the `@STATE` caps object so the app can show a "USB Audio"
  indicator / input-level control on builds that have it (hidden otherwise).

**Optional runtime control surface** (small): `@USBGAIN=<f>` to set the USB-in mixer-slot gain,
and surface `usbIn.getStatus()` overrun/underrun counters in `@STATE` for drift diagnostics.
Not required for v1 — the host's own volume maps through the UAC Feature Unit already.

### New envs (start on OPLL per [[feedback_test_with_opll]])

```ini
[env:teensy41_opll_usbaudio]           ; the green-anywhere canary, proves the path in mix-kit
extends = env:teensy41_opll            ; already sets TDSP_NO_SPDIF_IN
build_unflags = ${env:teensy41_opll.build_unflags} -D USB_MTPDISK_SERIAL
build_flags   = ${env:teensy41_opll.build_flags} -D USB_MIDI_AUDIO_SERIAL
                -D AUDIO_SUBSLOT_SIZE=3 -D TDSP_USB_AUDIO=1

[env:teensy41_usbaudio]                ; the real one: default Dexed synth as a 24-bit interface
extends = env:teensy41
build_unflags = -D USB_MTPDISK_SERIAL
build_flags   = ${env:teensy41.build_flags} -D USB_MIDI_AUDIO_SERIAL
                -D AUDIO_SUBSLOT_SIZE=3 -D TDSP_USB_AUDIO=1 -D TDSP_NO_SPDIF_IN
```

## 5. Cost / risk

- **RAM:** the two Q31 rings are `1024 × int32 × 2 = 8 KB` in **DMAMEM**, plus small F32 block
  scratch. **No async resampler** — unlike the BT (`btIn`, ~102 KB) and S/PDIF (`spdifIn`,
  ~85 KB) paths, USB audio uses the host clock + the UAC feedback endpoint, so there's **no
  160 KB filter buffer** and none of the RAM1 boot-loop pressure that dominates
  [[reference_resampler_ram]] / [[project_spdif_optical_ram_fit]]. This is the *cheap* input.
- **Flash/link:** the `USB_Audio_F32_24` objects are tiny; the patched core is already compiled
  in. Net code delta is ~two objects + a few connections + a MIDI drain.
- **Risk — low, and pre-retired:** the exact `USB_MIDI_AUDIO_SERIAL + AUDIO_SUBSLOT_SIZE=3`
  combination is what `spike_f32_usb_loopback` already validated on hardware end-to-end. The
  only *new* surface is the mix-bus routing (slot 2 reuse) and the USB-MIDI drain — both are
  copies of patterns already in `main.cpp`.
- **Watch item — clock coherence:** USB-in is slaved to the *host's* clock while the codec runs
  on the Teensy SAI1 master clock. USB audio's feedback endpoint rate-adapts, but sustained
  playback should be listened to for slow drift/glitching. The `getStatus()` overrun/underrun
  counters are exactly the probe for this — surface them early.
- **One-active-source honesty:** v1 muxes USB-in against optical-in (shared slot 2). BT-in
  (slot 0) and the synth (slot 3) are unaffected — you can still play a synth *over* incoming
  USB audio, and record the sum out USB. That mix-and-record case is the compelling one.

## 5b. App UI — BUILT AHEAD OF FIRMWARE (caps-gated, so inert until the flag ships)

A **USB Audio card** is already in `app/tdsp-control` (App.tsx + transport interface). It stays
hidden until the firmware advertises the capability, so it's harmless on today's builds. The
firmware must match this exact `@STATE` / command contract:

- **Capability (shows the card):** `@STATE.caps.usbaudio = true` on `TDSP_USB_AUDIO` builds.
  (Optionally list `usbaudio` in `@STATE.unavail` with a reason to render the card greyed instead
  of hidden — same pattern as `audioloop`/`fx`.)
- **Status block:** `@STATE.usb = { active, gain, rxover, txunder }` —
  `active` (bool) host has the stream open, `gain` (0..150 USB-in return level),
  `rxover`/`txunder` (ints, from `usbIn.getStatus()`) shown as a "clock drift" line when nonzero.
- **Command:** `@USBGAIN=<0..150>` sets the USB-in mixer-slot return level (the card's "USB In"
  slider). The host's own volume rides the UAC Feature Unit, independent of this.

Card contents (as built): status value line ("Streaming/Idle · 24-bit / 48 kHz"), a host-stream
indicator pill, the **USB In volume** slider (foot + body), and the drift counters. Themed light
blue (`THEME.usb`), ordered right after Audio Loop. Wired across all three transports
(USB/BLE/WiFi). This means step 4 below is *already done on the app side* — firmware just needs
to emit the caps/status and honour `@USBGAIN`.

## 6. Plan of record

1. **Add the flag + two envs** (`teensy41_opll_usbaudio` first) and the `main.cpp` graph gate.
   Green-build `teensy41_opll_usbaudio`. *(No hardware needed to prove it compiles + links + fits.)*
2. **Flash the OPLL env**, confirm the box enumerates as a sound card on the host; play host
   audio → hear it through TAC5212; record the OPLL out USB into a DAW. Watch `getStatus()`.
3. **Wire native USB-device MIDI**, confirm a DAW can play the synth over USB.
4. **Promote to a Dexed env** (`teensy41_usbaudio`) and emit the `@STATE.caps.usbaudio` +
   `@STATE.usb` status block + honour `@USBGAIN` (§5b). *The app card is already built and
   caps-gated, so this step just lights it up — no app work left.*
5. *(Later, optional)* sub-mixer so USB-in and optical-in coexist; `@USBGAIN`; multichannel
   (the alex6679 core already supports 2/4/6/8ch — a future per-track USB-out play).

## 7. Open questions

1. **Post-FX vs pre-FX record tap.** Recommend post-FX (record-what-you-hear); confirm no
   feedback path when the host also monitors.
2. **Drift over long sessions** — does the UAC feedback endpoint hold, or do we see periodic
   overruns? Decide by measurement in step 2 before promoting.
3. **Do we want a `USB_MTPDISK_AUDIO_SERIAL` combined descriptor** (author a new type in the
   vendored `usb_desc`) to keep SD-mount *and* audio? Higher effort/risk; deferred unless the
   `@WB` transfer path proves insufficient. Default answer: **no** — `@WB` already covers it.
