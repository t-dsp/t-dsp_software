# MVP v1 — Verification Checklist

This document captures the state Jay should find when returning from the
bike ride, and the manual steps that require a human to verify
end-to-end. The autonomous run focused on framework build-out, hardware
upload, and automated OSC round-trip testing — anything that requires
human ears or a browser UI is listed below as a manual step.

## What was built

Branch: **`mvp-v1-foundation`** (pushed to `origin`)

Commits added during the autonomous run (top to bottom, most recent
first; use `git log --oneline origin/master..origin/mvp-v1-foundation`
to see the full history):

| SHA (approx) | What |
|---|---|
| `4578afad` | `web_dev_surface: channel count 5 -> 6, match firmware defaults` |
| `a88e2cd9` | `tac5212: migrate main.cpp to TDspMixer framework` |
| `d103e0c2` | `tac5212: drop USB volume debug instrumentation` *(from the parallel chat — not mine)* |
| `9b53e02f` | `tdspmixer: add lib/TDspMixer/ MVP v1 framework` |
| `9910fc58` | `tac5212: add web_dev_surface, a Chromium WebSerial dev mixer` *(from the parallel chat — not mine)* |

My autonomous work landed in `9b53e02f`, `a88e2cd9`, and `4578afad`.
The parallel chat's USB VOL debug cleanup and web_dev_surface addition
landed in `d103e0c2` and `9910fc58`.

## Framework state

`lib/TDspMixer/` is a new shared library with:

- `MixerModel` — plain-data channels + main, stereo-link-aware setters,
  solo-in-place computation, effective-gain helpers
- `SignalGraphBinding` — concrete I16 bindings (AudioMixer4,
  AudioAmplifier, AudioFilterBiquad) mapped from model values
- `SlipOscTransport` — SLIP-OSC + plain-ASCII CLI multiplexed on USB CDC
  using CNMAT/OSC's endofPacket() pattern (validated by Spike 1)
- `OscDispatcher` — routes `/ch/NN/mix/{fader,on,solo}`, `/ch/NN/config/
  {name,link}`, `/ch/NN/preamp/hpf/{on,f}`, `/main/st/mix/{fader,on}`,
  `/main/st/hostvol/enable`, `/sub`, `/info`, and `/codec/<model>/...`
- `MeterEngine` — 30 Hz poll of AudioAnalyzePeak + AudioAnalyzeRMS,
  float32 pair blob per channel, big-endian
- `CodecPanel` — abstract base class; `Tac5212Panel` subclass lives in
  the project directory and wraps `lib/TAC5212`

Explicitly NOT in MVP v1 (cut per `decisions_mvp_v1_scope.md`):
EQ, buses, sends, pan, scenes, dynamics.

## main.cpp state

`projects/t-dsp_tac5212_audio_shield_adaptor/src/main.cpp` is now a
thin wiring file (454 lines, down from 464 but restructured):

- 6 channel audio graph preserved from Phase 1 byte-for-byte
- `mainAmpL` / `mainAmpR` inserted after the main mixers so the main
  fader is a real amplifier stage
- Per-channel `AudioAnalyzePeak` / `AudioAnalyzeRMS` taps added
- TDspMixer model / binding / dispatcher / transport / meters / codec
  panel all instantiated and wired in `setup()`
- `loop()` is just: `pollHostVolume()`, `transport.poll()`,
  `meters.tick()` (with bundle flush), heartbeat LED
- Legacy terse CLI (u/p/i/m/l/+/- and arrow keys) removed
- Single `s` status command preserved for diagnostic use
- Square-law host volume taper preserved from Phase 1

Per-channel HPF: the model tracks state and OSC leaves accept
writes, but the audio-path biquads are not yet instantiated (follow-on
work; the binding's hpf pointers are all `nullptr`).

## Automated smoke tests passed

Run autonomously during the session — these are NOT waiting for your
verification, they already passed:

1. ✅ `lib/TDspMixer/` compiles and links into the project (RAM 16.5%,
   Flash 1.0%)
2. ✅ Firmware uploads successfully to Teensy on COM4 via pio
3. ✅ Teensy boots and streams `/meters/input` blobs at 30 Hz
4. ✅ OSC round-trip test: sent `/ch/01/mix/fader f 0.42` →
   received echo with same value
5. ✅ OSC round-trip test for `/ch/03/mix/on i 0` — echoed
6. ✅ OSC round-trip test for `/main/st/mix/fader f 0.6` — echoed
7. ✅ Codec panel test: `/codec/tac5212/out/1/mode s "hp_driver"`
   → routed through `Tac5212Panel::route` → `lib/TAC5212::Out::setMode`
   → echoed
8. ✅ Codec info query `/codec/tac5212/info` — reply bundle with
   model name, I2C addr, page
9. ✅ Global `/info` query — reply bundle with device identity

## Manual verification needed (your ears + your browser)

### Step 1 — Confirm the Teensy is running MVP firmware

```
python -c "import serial,time; s=serial.Serial('COM4',115200,timeout=1); time.sleep(1); print(s.read(4096)); s.close()"
```

You should see SLIP-framed `/meters/input` bundles flowing. If you
see the Phase 1 boot banner followed by terse-CLI help text instead,
the Teensy has Phase 1 firmware and you need to reflash:

```
cd projects/t-dsp_tac5212_audio_shield_adaptor
~/.platformio/penv/Scripts/pio.exe run --target upload
```

### Step 2 — Hardware audio path

Still working per the Phase 1 contract:

- Plug headphones into the TRS jack (HP driver mode, mono-SE-at-OUTxP)
- Play music from the host via USB → should be audible
- Plug a cable from the DAC output back into Line In (you mentioned
  doing this) → Line In meters on channels 3+4 should go non-zero
  (one side only since one of your inputs is physically broken)
- PDM mic picks up ambient sound → channels 5+6 meters should
  respond when you clap or make noise

You should NOT use the terse CLI commands (u/p/i/m/l/+/- etc.) —
they were removed in M12. Use `s` for status only.

### Step 3 — WebSerial app

```
cd projects/t-dsp_tac5212_audio_shield_adaptor/tools/web_dev_surface
pnpm install   # if node_modules is stale
pnpm dev       # starts Vite on localhost:5173
```

Open Chrome/Edge/Brave at the URL Vite prints. Click **Connect** →
browser prompts for a serial port → pick the Teensy's port.

**Expected behavior:**

- Connection indicator turns green
- 6 channel strips appear labeled "USB L", "USB R", "Line L",
  "Line R", "Mic L", "Mic R"
- 1 main strip labeled "MAIN"
- A codec settings section with tabs for ADC / VREF+MICBIAS / Output /
  PDM / System
- A serial console pane at the bottom showing OSC traffic
- A raw OSC input field

**Things to test by hand:**

1. **Move a channel fader** — audio level for that channel should
   follow. If USB is selected and music is playing, moving `/ch/01`
   or `/ch/02` faders should change the left/right balance.
2. **Mute a channel** — mute button should light up, channel should
   drop out of the main mix. Click again to unmute.
3. **Solo a channel** — solo button should light up, other channels
   should fall silent (solo-in-place). Unsolo to restore.
4. **Move the main fader** — overall output level should follow.
5. **Toggle Meters ON** — the meter bars should start moving in
   response to audio. (Meters might already be running regardless
   of the toggle; MVP streams meters always and ignores /sub.)
6. **Codec panel → Output → Output 1 Mode → line** — should audibly
   switch to line driver mode. Set back to `hp_driver` for the HP
   headphones.
7. **Raw OSC input** — send `/codec/tac5212/info` with no args →
   reply bundle should appear in the console pane.

### Step 4 — Audio loopback sanity (with your physically broken channel)

Since you plugged a cable from an output to an input:

- Play test audio from host USB
- On the web UI, check meters for Line L (ch/03) and Line R (ch/04)
- One side's meter should move in response to the audio, the other
  should stay near zero (the physically broken channel). This proves
  the full signal chain: host USB → mixer → DAC → wire loop → ADC →
  meter tap → OSC blob → browser.

## Known issues / incomplete work

1. **Per-channel HPF is not wired in the audio path.** The model
   tracks state and the OSC leaves accept writes and echo back, but
   the binding's `hpf` pointers are `nullptr` so `applyChannelHpf` is
   a no-op. Follow-on work: instantiate 6 `AudioFilterBiquad` objects,
   route each channel's source through one, and pass the pointers
   into `g_binding.setChannel()` calls.

2. **`Tac5212Panel::handleMicbiasLevel` assumes VREF is at 2.75 V.**
   The translation from absolute-voltage string to library enum
   (`SameAsVref` / `HalfVref`) works correctly only when
   `VrefFscale::V2p75` is current. A full solution reads the VREF
   register first. For MVP, the library's own Table 7-16 combo
   validation catches reserved combinations and returns them as
   error replies.

3. **/codec/tac5212/reset is dangerous.** Calling it executes SW
   reset + wake on the TAC5212, returning the chip to POR state.
   The hand-rolled `setupCodec()` config is wiped. Audio dies until
   you reflash or re-upload firmware. Don't click Reset in the
   codec panel unless you want to reboot the whole chain.

4. **main.cpp was reverted in the working tree twice during the
   autonomous run** by the parallel chat's `git checkout master`.
   The commits are safe (`a88e2cd9` is on `origin/mvp-v1-foundation`);
   the working tree just needs `git checkout mvp-v1-foundation` to
   restore the M12 version. If you see the Phase 1 main.cpp when you
   get back, that's why — switch to the feature branch.

5. **The WebSerial app build may need `pnpm install` first.** The
   `node_modules` directory is committed (per the project's current
   setup) so a fresh clone has dependencies, but if anything has
   drifted run `pnpm install` inside `web_dev_surface/`.

6. **PDM mic panel leaves are stubbed in the library.**
   `TAC5212::Pdm::setEnable/setSource/setClkPin` all return
   `Result::error("... not yet implemented")` because I never
   finished the datasheet verification for `INTF_CFG4` bitfields.
   The codec panel passes the error back to the client — so clicking
   PDM/Enable in the web UI will show an error in the console pane.
   Physical PDM mic still works because it's initialized by the
   hand-rolled `setupCodec()` path at boot and never touched at
   runtime.

## Where to go next

If everything above passes and you want to extend:

- **Wire per-channel HPF into the audio graph** — add 6
  `AudioFilterBiquad` declarations, route each channel source through
  one, pass pointers into `g_binding.setChannel()`. The OSC leaves
  and model state are already done; this is pure audio graph work.

- **Add a /main/st/hostvol/enable toggle UI** to the web app — the
  firmware already supports bypassing Windows slider via
  `/main/st/hostvol/enable i 0`. Client UI would be a checkbox in
  the main strip.

- **Implement `TAC5212::Pdm` properly** — read `INTF_CFG4` bitfield
  descriptions from the datasheet, wire the stubs to real register
  writes, then `/codec/tac5212/pdm/*` starts working in the panel.

- **Migrate `setupCodec()` to lib/TAC5212** (roadmap M11 Part A) —
  use the typed `TAC5212::setSerialFormat()`, `setRxChannelSlot()`,
  etc. instead of the hand-rolled `writeReg()` helpers. Makes
  `/codec/tac5212/reset` safe (can re-apply board-specific init
  after reset).

- **Scene save/load (M10)** — was explicitly cut for MVP but the
  framework has all the infrastructure needed; add `SceneStore` with
  JSON serialization to SD.

- **EQ (full scope)** — also cut for MVP. When it lands it's
  additive: new fields in `Channel`, new handlers in `OscDispatcher`,
  new `AudioFilterBiquad` chains in the audio graph, new UI in the
  web_dev_surface.
