# Audio-source MUX: S3 control/streaming + dedicated clock-slaved A2DP sink

**Status:** Decision locked (topology); chip selection + spikes pending
**Owner:** Jay
**Last updated:** 2026-07-17

## The decision in one line

Split the radios onto **separate chips** and treat the audio source as a **mux, not a
mixer**: one audio source is active at a time, and **every source is resampled into the
system clock domain in hardware** so the Teensy runs **no software ASRC**.

## Why (what forced this)

- **The classic ESP32 cannot do WiFi + A2DP at once.** Measured on hardware
  ([[project_wifi_control]]): with Bluetooth up, IDF forces WiFi modem sleep (disabling it
  boot-loops the chip), which parks the radio between beacons — ping avg jumps 7.9 ms →
  58.7 ms, jitter 4 ms → 42.6 ms, and bulk `@READ` transfers drop inbound commands. One
  2.4 GHz radio can't carry both. This is a hard, physical limit, not a code bug.
- **The classic ESP32 cannot ASRC its own A2DP output.** Tried by alex6679; it doesn't
  work, which is *why* the async resampler currently lives on the **Teensy** (see
  `lib/TDspAsyncI2S`, [[reference_async_i2s_resampler]]). So "keep the classic but
  clock-discipline it" is off the table.
- **That Teensy ASRC is expensive.** Its filter buffers (~160 KB) sit on Teensy RAM1 and
  are the direct cause of the over-RAM boot-loops we've fought ([[reference_resampler_ram]],
  [[project_jaymint_flash_host]]). Moving it OFF the Teensy is a real win, not just tidy.

## Target architecture

```
                              ┌──────────── system low-jitter master clock (MCLK) ───────────┐
                              │                        │                        │             │
phone ──A2DP/BT──▶ [A2DP sink chip] ──I2S──▶ [hardware ASRC] ──I2S/TDM (slaved to MCLK)──▶    │
                                                                                          Teensy 4.1
Spotify/AirPlay/radio ──WiFi──▶ [ESP32-S3] ──────────I2S/TDM (slaved to MCLK)────────────▶    │
                                    │                                                          │
app ──WiFi/BLE control──▶ [ESP32-S3] ──UART @-lines──────────────────────────────────────▶ (control)
```

- **ESP32-S3** owns ALL the "smart" radio: **BLE control + WiFi control + WiFi media
  streaming** (Spotify Connect / AirPlay / internet radio — see [[project_wifi_control]]
  for the firmware options). It has BLE *and* WiFi; it has **no** Bluetooth Classic, so it
  can never be the A2DP sink — which is exactly why A2DP moves to its own chip.
- **Dedicated A2DP sink chip** does Bluetooth *audio only* (a fixed-function appliance),
  emitting I2S at its own free-running rate.
- **Hardware ASRC** resamples that free-running A2DP stream to the **system master clock**.
  This is the async-boundary quarantine — the one place the phone-vs-local rate mismatch is
  reconciled, in silicon, on the system clock. **No software ASRC on the Teensy.**
- **The MUX is trivial because everything is clock-coherent.** Both audio sources (ASRC
  output, S3 streamer output) are slaved to the same MCLK, so they're synchronous TDM
  inputs. The Teensy just *selects* which TDM slot(s) to route in software — no physical
  mux chip, no resampling, no glitch beyond a clean source switch.

### The two operating modes (the "one radio stream" UX)

| | Mode A — Bluetooth audio | Mode B — WiFi streaming |
|---|---|---|
| Audio source | A2DP sink → hardware ASRC | S3 WiFi streamer |
| S3 WiFi | control only (low-rate) | control + media (full) |
| A2DP sink | active | powered down / idle |
| Teensy ASRC | none (hardware did it) | none (streamer buffers internally) |

Crucially, **the two radios are on different chips**, so there is no on-die coexistence
arbitration and no forced modem sleep — the entire problem from [[project_wifi_control]]
disappears. WiFi *control* stays live in both modes, so the app never loses the device;
only the audio *source* is muxed.

> Correction to keep honest: Mode B is not *literally* ASRC-free — a WiFi streamer always
> rate-adapts the (clockless, bursty) network stream in its output buffer. But that's
> inherent to the streamer and lands on the system clock if the S3 output is slaved, so
> there's no *separate* ASRC block. "No software ASRC on the Teensy" is the real claim, and
> it holds in both modes.

## Chips that do this out of the box

### The hero part — hardware ASRC: **TI SRC4392** (verified)

This is the chip that "ties the stream to the clocks we use everywhere," confirmed against
the datasheet ([TI SRC4392](https://www.ti.com/product/SRC4392)):

- Two-channel **asynchronous** sample-rate converter (built on the well-regarded SRC4192
  core) with strong **jitter attenuation** — takes an async input and locks the output to a
  chosen master clock.
- **Two 4-wire audio serial ports (I2S / left- / right-justified), each independently
  Master or Slave.** In master mode it derives BCLK/LRCLK from the selected MCLK
  (128/256/384/512×fs). This is exactly the "input side floats with the A2DP chip, output
  side is nailed to the system MCLK" behavior we need.
- Bonus: integrated **S/PDIF receiver + transmitter**, so the same chip could also clock-
  correct an optical input (relevant to [[project_spdif_optical_ram_fit]]).
- SPI **or** I2C control; TQFP-32 (hand-solderable).
- Alternatives if sourcing/price is a problem:
  [**TI SRC4382**](https://www.ti.com/product/SRC4382) (sibling), **Cirrus CS8421**
  (32-bit/192k stereo ASRC, small pkg), **ADI AD1896** (24-bit/192k, older/proven),
  **AKM AK4137** (high-end/DSD — but AKM's 2020 fab fire left lingering supply/price
  caveats; verify stock).

### The A2DP sink: **Qualcomm/CSR CSR8675** (leading candidate)

- BT 5.0 audio SoC, aptX / aptX HD / aptX LL, **I2S output**, on-chip DSP; ready-made
  modules are cheap and common (e.g. Audiophonics "CSR8675 aptX-HD to I2S").
- **Caveat found in research:** the CSR8675's I2S is normally **master-only**, so it can't
  itself slave to the system clock — which is *fine*, because that's the SRC4392's job. The
  CSR8675 free-runs as I2S master into the SRC4392, and the SRC4392 hands the Teensy a
  system-locked stream.
- Configured with Qualcomm/CSR tools (some friction, but heavily used in DIY audio).
- Cheaper/simpler A2DP sinks exist — **JieLi AC69xx** (ubiquitous in $3 BT boards, I2S out),
  **BlueTrum AB532x**, Actions/Realtek parts — but their datasheets/clocking flexibility are
  thin/Chinese-only. **Do not assume their clocking; verify before committing.** The
  SRC4392 downstream makes the sink's own clock behavior mostly irrelevant anyway (it just
  needs a clean I2S master output), which is the beauty of the two-chip split.

### Net: the concrete, off-the-shelf chain

```
[CSR8675 A2DP module]  ──I2S(master)──▶  [SRC4392 ASRC]  ──I2S(slaved to system MCLK)──▶  Teensy TDM
                                              ▲
                              system low-jitter audio oscillator (e.g. 24.576 / 22.5792 MHz)
```

Everything downstream of the SRC4392 is synchronous. Zero software ASRC.

## Open questions before a board spin

1. **Master-clock topology.** Who generates the system MCLK — the Teensy, or a dedicated
   low-jitter oscillator feeding Teensy + SRC4392 + S3? A shared clean oscillator is the
   audiophile-correct answer and makes the whole graph coherent. (Overlaps [[shared-clock]]
   / [[project_master_clock]] — but that's *musical* tempo; this is the *sample* clock.
   Don't conflate them.)
2. **Can the ESP32-S3 I2S output slave to the external MCLK** while its streamer resamples
   network→MCLK internally? Needs an IDF/driver check on the chosen streamer firmware.
3. **TDM slot assignment** — which slots carry the A2DP/ASRC pair vs the S3 stream into the
   Teensy, and does the S3's I2S peripheral emit the right TDM framing.
4. **A2DP sink final selection** — CSR8675 (proven, config-tool friction) vs a cheaper
   JieLi/BlueTrum part (BOM win, doc risk). Prototype whichever behind the SRC4392.

## RF coexistence (two 2.4 GHz radios on one board)

The S3 (WiFi/BLE) and the A2DP sink (BT Classic) are both 2.4 GHz. The concern is
**receiver desense**, governed by **antenna-to-antenna isolation (dB)**, not chip spacing.

- Target **~20 dB min, ~30–40 dB comfortable** isolation.
- Distance alone can't get there on a hand-sized board (λ≈12.5 cm). Buy isolation with
  **opposite corners + orthogonal antenna orientation (polarization, ~90°) + pattern
  nulls**; **separate external antennas help precisely because they let you control this**.
  In-band 2.4 GHz filtering does **not** help (both signals share the band).
- **The mux keeps us out of the hard case:** Mode B has BT off (no contention at all);
  Mode A pairs continuous A2DP with only low-rate WiFi *control*, so the worst realistic
  symptom is an occasional A2DP tick during a control burst.
- Insurance if Mode A ever glitches: a **coexistence/PTA wire** between the radios
  (stuff-if-needed footprint), and/or **software deferral** — don't run heavy WiFi
  transfers while listening to Bluetooth (you wouldn't anyway).
- **Must be measured** on the real board (VNA S21 between antenna feeds, or an OTA desense
  test) before committing the layout — exact isolation depends on antennas/ground/enclosure.

## Confidence / provenance

- SRC4392 capabilities: **verified** against the TI datasheet.
- CSR8675 I2S master-only: from vendor docs + DIY-audio community reports — **treat as
  strong-but-verify** before layout.
- Cheaper A2DP sinks (JieLi/BlueTrum): **unverified clocking** — prototype first.
- RF numbers: standard multi-radio coexistence practice — **directionally right, measure to
  confirm** for this board.
