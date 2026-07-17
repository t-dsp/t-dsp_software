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

## How to do the ASRC — by cost, cheapest first

The A2DP async-reconciliation can live in three places. Pick by cost/risk.

### Tier 0 — **S3 does the ASRC in software** (≈ $0 extra silicon) — RECOMMENDED FIRST

This is the original Mode-A plan (`CLASSIC/sink → S3 as ASRC → Teensy`), and it's free: the
S3 is already on the board. Wiring: the A2DP sink's I2S feeds the S3's I2S0 (S3 slaves to
the sink's clock on input); the S3 runs a software ASRC and drives I2S1 out to the Teensy
slaved to the system MCLK. The ESP32-S3 has **two I2S peripherals**, so in + out is native.

Why the S3 can likely do what the classic couldn't (the alex6679 result): the classic
failed doing **A2DP + ASRC on one chip at once** — it was already saturated running the
Bluetooth stack. In this architecture the **S3 is NOT running Bluetooth at all** (the sink
does), so its cycles are free for DSP; and the S3 is a far stronger DSP part (dual-core
240 MHz, SIMD/PIE vector extensions, PSRAM). Real-time DSP headroom is demonstrated — e.g. a
[full FM-stereo + RDS 24-bit DSP pipeline runs live on the S3](https://blog.infrasonicaudio.com/real-time-audio-synthesis-on-esp-32).

- **Cost:** nothing beyond the S3 already in the design.
- **Risk / must-spike:** confirm alex6679's resampler (or an equivalent) fits in real time
  on the S3 alongside I2S-in, I2S-out, and control. Plausible but UNPROVEN — this is the
  spike that decides whether Tier 0 flies or we drop to Tier 1. It also needs the async
  resampler ported from Teensy/OpenAudio-F32 to ESP-IDF.

### Tier 1 — **cheap dedicated hardware ASRC: Cirrus CS8421** (≈ $5) — the fallback

If the S3 can't carry the ASRC in real time, this is the deterministic hardware answer at a
fraction of the SRC4392's price ([CS8421](https://www.cirrus.com/products/cs8421)):

- 32-bit / 192 kHz **stereo asynchronous** SRC; input and output can be **completely
  asynchronous** or synced to an external clock — exactly our case.
- **~$4.85 @ 10k** (vs the SRC4392's ~$8–13). It's cheaper because it's *just* the ASRC — no
  S/PDIF transceiver, which we don't need here.
- Small package, SPI/I2C config. A2DP sink (I2S master) → CS8421 (locks output to system
  MCLK) → Teensy. Zero software ASRC, guaranteed timing.
- Other cheap-ish dedicated ASRCs if sourcing pushes back: **ADI AD1896**, **TI SRC4192**
  (the SRC4392's ASRC core *without* the S/PDIF combo — cheaper than the 4392).

### Tier 2 — **TI SRC4392** (≈ $8–13) — only if you want the S/PDIF combo in hardware

Verified, premium, and overkill *for A2DP alone*. Its remaining edge is that its single ASRC
+ internal mux + built-in **S/PDIF receiver** can ingest **both** A2DP (I2S) *and* optical
S/PDIF, one at a time, and clock-lock the output — freeing **both** of the Teensy's
resamplers (see "Optical" below) with **zero S3 CPU load** and full determinism. That is what
you're paying the ~$6–10 premium for: not "an ASRC," but "optical-RX + mux + ASRC + zero DSP
risk in one part."

## Optical S/PDIF input — and the single spike that decides the whole front-end

The Teensy runs **two** resamplers today: `btIn` (~102 KB) and `spdifIn` (~85 KB) ≈ **187 KB
of RAM1** — the boot-loop pressure. Relieving *both* means the optical path needs a home too,
not just A2DP. Where optical can go:

- **The S3 can receive S/PDIF — in software, via the RMT peripheral.** There is no hardware
  S/PDIF RX in the S3's I2S (Standard/TDM/PDM only), but RMT captures the biphase-mark
  transitions and self-clocks from the stream (any sample rate), demonstrated on ESP32
  ([Hackaday, Oct 2025](https://hackaday.com/2025/10/06/esp32-decodes-s-pdif-like-a-boss-or-any-regular-piece-of-hi-fi-equipment/);
  [ESP32-S3 SW decode, diyAudio](https://www.diyaudio.com/community/threads/esp32-s3-rtp-sink-source-via-usb-uac-and-pdif-input-output-software-decode.432392/)).
  Cost in the author's words: it **"basically needs its own core."**
- So the S3 can be the WHOLE front-end with **no extra decode chip**: A2DP-sink I2S and the
  RMT-decoded optical are both **muxed via the S3's GPIO matrix** (route either into the
  input I2S at runtime — one source at a time = the mux), the S3 software-ASRCs whichever is
  live, and drives I2S out to the Teensy on the system clock.

### The decision gate — one spike

Because of the mux, only one source is ever active, so the **peak** S3 load is the
**optical mode: RMT S/PDIF decode + software ASRC running together** (A2DP mode is lighter —
ASRC only, no RMT). On a 240 MHz dual-core S3:

> **SPIKE: can the S3 run RMT S/PDIF-decode + software ASRC simultaneously, real-time?**
> (RMT ≈ one full core; alex6679's ASRC is heavy on a 240 MHz Xtensa — plausible but tight,
> and unmeasured. This one experiment picks the front-end.)

Escalation ladder by spike outcome:

| Outcome | Front-end | Extra silicon | Frees Teensy RAM |
|---|---|---|---|
| **S3 does RMT-decode + ASRC together** | S3 does everything (A2DP + optical mux + ASRC) | **$0** | both (~187 KB) |
| **S3 does ASRC, but not + RMT** | add a ~$4 S/PDIF-RX chip (WM8804 / CS8416 / DIR9001) → clean I2S → S3 ASRCs | **~$4** | both (~187 KB) |
| **S3 can't carry the ASRC reliably** | **SRC4392** (hardware RX + mux + ASRC) | **~$8–13** | both (~187 KB) |

All three free **both** resamplers — the difference is **$0 → ~$4 → ~$10** traded against
**S3 CPU risk → determinism**. (Note: A2DP-only, no optical, is the easy case — S3 ASRC alone,
Tier 0, frees ~102 KB and there's no optical resampler left to worry about.) The A2DP sink
chip and the optical TORX front-end are needed in **every** path, so they're not part of this
delta.

**Recommendation:** run the spike. If it passes, the S3 is the entire front-end for ~$0 and
the SRC4392/CS8421 are moot. If it fails, add the ~$4 S/PDIF-RX (keep the S3 ASRC) before
reaching for the SRC4392 — pay the ~$10 only to buy zero-S3-DSP determinism.

### The hero part — hardware ASRC: **TI SRC4392** (verified; premium — see Tier 2)

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

### Net: the concrete chains (cheapest first)

```
Tier 0 ($0):  [A2DP sink] ──I2S──▶ [ESP32-S3: SW ASRC, out slaved to MCLK] ──▶ Teensy TDM
Tier 1 (~$5): [A2DP sink] ──I2S──▶ [CS8421 ASRC, out slaved to MCLK]        ──▶ Teensy TDM
                                          ▲
                          system low-jitter audio oscillator (e.g. 24.576 / 22.5792 MHz)
```

Everything downstream of the ASRC (SW on the S3, or the CS8421) is synchronous to the
system clock. Zero software ASRC **on the Teensy** either way. Recommendation: **spike Tier 0
first** (it's free and it's the original plan); fall back to the CS8421 only if the S3 can't
carry the resampler in real time.

## Open questions before a board spin

0. **THE GATE — the S3 DSP spike** (see "Optical" above): can the S3 run RMT S/PDIF-decode +
   software ASRC together, real-time? Its outcome picks the front-end ($0 all-S3 / +$4
   S/PDIF-RX / +$10 SRC4392). Do this first — the others matter only after it's answered.
1. **Master-clock topology.** Who generates the system MCLK — the Teensy, or a dedicated
   low-jitter oscillator feeding Teensy + S3 (+ ASRC chip if used)? A shared clean oscillator
   is the audiophile-correct answer and makes the whole graph coherent. (Overlaps
   [[shared-clock]] / [[project_master_clock]] — but that's *musical* tempo; this is the
   *sample* clock. Don't conflate them.)
2. **Can the ESP32-S3 I2S output slave to the external MCLK** while (a) its streamer resamples
   network→MCLK internally (Mode B) and (b) it software-ASRCs an input to MCLK (Mode A)?
   Needs an IDF/driver check on the chosen streamer firmware.
3. **TDM slot assignment** — which slots carry the ASRC'd audio vs the S3 stream into the
   Teensy, and does the S3's I2S peripheral emit the right TDM framing.
4. **A2DP sink final selection** — CSR8675 (proven, config-tool friction) vs a cheaper
   JieLi/BlueTrum part (BOM win, doc risk). Prototype it; the downstream ASRC (S3 SW or a
   chip) makes the sink's own clock behavior largely irrelevant — it just needs clean I2S.

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
