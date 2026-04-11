# 02 — OSC Protocol

## Reference convention

The address tree is **X32-flavored**: it follows the shape of the Behringer X32's OSC protocol because that's the closest thing to an industry-standard remote control protocol for digital live mixers. Every existing X32-aware client tool — Open Stage Control templates, TouchOSC layouts, QLab cues, Reaper plugins, dozens of phone/iPad surfaces — can target our firmware with minimal adaptation if we match the convention.

We do not match X32 verbatim. We diverge where it makes sense (e.g. our codec panels are namespaced as `/codec/<model>/...`, which X32 doesn't need because its codec is fixed). But we match the shape, the naming idioms, and the value conventions wherever there's no compelling reason to differ.

## Subtree ownership

The full OSC tree is split into clearly-owned subtrees, each with a different audience and a different implementation source:

| Subtree | Audience | Owned by | Stability |
|---|---|---|---|
| `/ch/NN/...` | Live engineer | TDspMixer mixer-domain handlers | **Stable contract** |
| `/bus/NN/...` | Live engineer | TDspMixer mixer-domain handlers | **Stable contract** |
| `/main/st/...` | Live engineer | TDspMixer mixer-domain handlers | **Stable contract** |
| `/meters/...` | Live engineer (UI) | TDspMixer MeterEngine, streamed via subscription | **Stable contract** |
| `/codec/<model>/...` | Live engineer (input patching) | Per-board CodecPanel subclass | **Stable contract** (per board) |
| `/sub`, `/-snap/...`, `/info`, `/xremote` | Live engineer | TDspMixer global commands | **Stable contract** |
| `/teensy*/audio/...` | Firmware developer | OSCAudio (vendored), build-flag-gated | **Unstable, debug only** |
| `/teensy*/dynamic/...` | (not used) | OSCAudio's dynamic graph manipulation — **NOT used in T-DSP** | n/a |

The X32-style subtrees (top six) are the contract that clients target. The OSCAudio debug subtree is the internal back door for poking individual Audio Library objects during development; it's compiled out of production builds via the absence of `OSCAUDIO_DEBUG_SURFACE`.

## The address tree (mixer surface)

```
/ch/NN/mix/fader        f    0..1 normalized fader position
/ch/NN/mix/on           i    0|1 — note: "on" = unmuted (X32 idiom)
/ch/NN/mix/pan          f    0..1 (0.5 = center)
/ch/NN/mix/solo         i    0|1
/ch/NN/mix/MM/level     f    send level to bus MM (0..1)
/ch/NN/config/name      s    user-settable channel name
/ch/NN/config/color     i    0..15 color index
/ch/NN/config/source    s    soft-patch input source
/ch/NN/config/link      i    0|1 stereo-link with adjacent odd/even pair
/ch/NN/preamp/hpf/on    i    0|1
/ch/NN/preamp/hpf/f     f    Hz
/ch/NN/eq/on            i    0|1
/ch/NN/eq/B/type        s    "lcut"|"lshelf"|"peq"|"hshelf"|"hcut"
/ch/NN/eq/B/f           f    Hz
/ch/NN/eq/B/g           f    dB
/ch/NN/eq/B/q           f
/ch/NN/dyn/...               (slot reserved for v2 — not implemented v1)

/bus/NN/mix/fader       f
/bus/NN/mix/on          i
/bus/NN/config/name     s
/bus/NN/eq/B/...             (same shape as channel EQ)

/main/st/mix/fader      f    headphone master
/main/st/mix/on         i

/meters/input           b    blob — per-channel peak/RMS
/meters/output          b    blob — per-bus peak/RMS
/meters/gr              b    blob — per-channel gain reduction (future)

/sub                         (subscription routes — see below)
/-snap/save             i    store scene N
/-snap/load             i    recall scene N
/-snap/list             -    reply: scene names blob
/info                   -    reply: device name, FW version, channel counts
/xremote                -    legacy X32 idiom — alias for /sub addSub
```

`NN` is a zero-padded two-digit channel/bus number. `B` is a one-digit EQ band index. Patterns like `/ch/*/mix/fader` match all channels and are supported by the dispatcher.

## The address tree (codec settings panel)

The TAC5212-specific subtree, owned by `Tac5212Panel` in the project directory.

**Two design rules** that determine what is and isn't allowed in this subtree:

1. **Gain/level/fader concerns belong to the mixer framework, not the codec panel.** Even if the codec hardware offers a way to set a digital volume via a register (e.g. TAC5212's `ADC_CH*_DVOL` or `DAC_*_DVOL`), that's a gain, gains live on the mixer surface (`/main/st/mix/fader`, `/ch/NN/mix/fader`, etc.), and exposing it from the codec panel creates two paths to the same audible result — a recipe for state desync. Per-codec hardware acceleration of mixer gains is a future binding-layer optimization, never a codec-panel leaf.
2. **A per-channel codec leaf must not have chip-global side effects.** If a control affects more than one channel of the codec, it must be a chip-global leaf, not embedded inside a per-channel address. Hidden cross-channel coupling is a footgun.

These rules eliminated four speculative leaves from earlier drafts (`/out/N/level`, `/out/N/drive`, the `"se_mic_bias"` enum value of `/adc/N/mode`, and `/adc/N/pga`). See the [TAC5212 design notes](#tac5212-codec-panel-design-notes) below for the reasoning per leaf. The current subtree is grounded in the SLASF23A datasheet (Dec 2023, rev Jan 2025).

```
/codec/tac5212/info                 -    reply: model, rev, I2C addr, slot map
/codec/tac5212/reset                -    soft reset + re-init
/codec/tac5212/wake                 i    sleep/wake
/codec/tac5212/status               -    reply: DEV_STS0 + derived flags

# Per-channel ADC analog configuration (NOT gain — gain belongs on the mixer surface)
# Note: ADC ch1/ch2 are analog-only on the TAC5212; PDM lives entirely in /pdm/.
/codec/tac5212/adc/N/mode           s    "single_ended_inp" | "differential"
/codec/tac5212/adc/N/impedance      s    "5k" | "10k" | "40k"        ADC_CHn_IMP[5:4]
/codec/tac5212/adc/N/fullscale      s    "2vrms" | "4vrms"           ADC_CHn_FULLSCALE_VAL[1]
                                         (4vrms only valid in high-CMRR mode w/ 2.75V VREF +
                                         24kHz audio bandwidth; library rejects invalid combos
                                         with an error reply)
/codec/tac5212/adc/N/coupling       s    "ac" | "dc_low" | "dc_rail_to_rail"
/codec/tac5212/adc/N/bw             s    "24khz" | "96khz"

# ADC HPF is chip-global on the TAC5212 (DSP_CFG0), not per-channel.
# Exposed as a global leaf so there are no hidden cross-channel side effects.
/codec/tac5212/adc/hpf              i    0|1   codec-native HPF enable (affects both ADC channels)

# Chip-global VREF and MICBIAS (share VREF_MICBIAS_CFG register 0x4D —
# library does read-modify-write with combo validation on every leaf)
/codec/tac5212/vref/fscale          s    "2.75v" | "2.5v" | "1.375v" | "1.25v" | "avdd"
                                         VREF_FSCALE[1:0]; per-board decision typically set at
                                         boot based on AVDD
/codec/tac5212/micbias/enable       i    0|1   PWR_CFG[5] MICBIAS_PDZ — chip-global
                                         (reply carries warning when EN_MBIAS_GPIO != 0,
                                         since GPIO state shadows the I²C value silently)
/codec/tac5212/micbias/level        s    "2.75v" | "2.5v" | "1.375v" | "1.25v" | "avdd"
                                         MICBIAS_VAL[3:2]; library validates against the
                                         current vref/fscale and rejects reserved combos
# /codec/tac5212/micbias/gpio_control s   RESERVED: EN_MBIAS_GPIO[7:6] pin-driven enable

# PDM mic
/codec/tac5212/pdm/enable           i    0|1
/codec/tac5212/pdm/source           s    "gpi1" | "gpi2"
/codec/tac5212/pdm/clk_pin          s    "gpio1" | ...

# Output drivers (driver type only — level is mixer territory)
/codec/tac5212/out/N/mode           s    "diff_line" | "se_line" | "hp_driver" | "receiver"

# Reserved for future analog bypass support
# /codec/tac5212/bypass/enable      i    RESERVED — global analog bypass enable
# /codec/tac5212/bypass/N/level     s    RESERVED — OUT1x_CFG1[5:3] enum,
                                         only valid when bypass/enable=1

# Raw register access (debug)
/codec/tac5212/reg/set              ii   reg, value          (raw I²C poke)
/codec/tac5212/reg/get              i    reg                  (reply: /codec/tac5212/reg ii)
```

Future codec panels follow the same pattern under their own model name (`/codec/cs42448/...`, etc.). The `/codec/<model>/info` and `/codec/<model>/status` endpoints are conventional and any client can render them as a "codec info" panel without knowing what codec is in use.

### TAC5212 codec panel design notes

This sub-section captures *why* specific leaves were dropped, replaced, or reserved during design, to prevent the same speculative additions from creeping back in. Verified against datasheet SLASF23A (Dec 2023, rev Jan 2025).

**`/out/N/level` — dropped from v1, reserved as `/bypass/N/level`.** The TAC5212 has a real `OUT1x_CFG1[5:3] OUT1P_LVL_CTRL` enum field with +12 / +6 / 0 / -6 / -12 dB values, but the datasheet says non-0 dB values are valid only in analog bypass mode. In normal DAC playback the field must stay at `100b` (0 dB) or the device enters "Reserved; Don't use" territory. The small mixer v1 doesn't use analog bypass at all, so the leaf has no useful semantics in v1. When/if analog bypass is added, the correct address is namespaced under `/bypass/...` so the bypass-only scope is visible at the address level — there's no way to accidentally poke the field in DAC mode.

**`/out/N/drive` — dropped permanently.** There's no continuous drive-strength axis on the TAC5212. The two real axes are *driver type* (selected by `/out/N/mode` as an enum) and *digital level* (which is mixer territory, not codec territory). The original `/drive` leaf conflated two unrelated concerns. No future use is anticipated.

**`/adc/N/mode "se_mic_bias"` — dropped permanently.** MICBIAS on the TAC5212 is a chip-global power enable in `PWR_CFG[5]`, not a per-channel concern. Embedding it as an enum value of a per-channel leaf would create hidden cross-channel side effects (setting "se_mic_bias" on ch1 would silently affect ch2). Instead, `/micbias/enable` is exposed as a chip-global leaf. The `/adc/N/mode` enum stays pure: `single_ended_inp` / `differential` / `pdm_input`, none of which have cross-channel coupling.

**`/adc/N/pga` — dropped permanently.** Speculative leaf based on the wrong assumption that the TAC5212 has an analog PGA register the way many codecs do. It does not. The TAC5212's only dB-valued ADC-path gain is `ADC_CH*_DVOL`, which is *digital* volume post-ADC, and per Rule A digital gain belongs on the mixer surface (`/ch/NN/mix/fader` and friends), not on the codec panel. What the datasheet's electrical-characteristics tables call "channel gain" refers to this `ADC_CH*_DVOL` field, not analog gain. If a future framework feature wants to push mixer per-channel trim into `ADC_CH*_DVOL` as a hardware acceleration, that lives in the binding layer behind a flag, never as a codec-panel leaf.

**`/adc/N/impedance` + `/fullscale` + `/coupling` + `/bw` — added to replace the speculative `/range`.** The TAC5212 has no single "line vs mic" mode. The actual per-channel knobs that affect analog input sensitivity profile are: input impedance (`ADC_CHn_IMP[5:4]`: 5k/10k/40k), full-scale voltage (`ADC_CHn_FULLSCALE_VAL[1]`: 2 Vrms / 4 Vrms — with 4 Vrms only valid in high-CMRR + 2.75 V VREF + 24 kHz BW combinations), input coupling (AC / DC-low / DC-rail-to-rail), and audio bandwidth (24 kHz / 96 kHz). Each one is a real per-channel register field that doesn't violate either rule — they configure the analog front end without being gains and without affecting other channels. The library validates the (`fullscale`, `bw`, `vref/fscale`) combination on every write and rejects invalid combos with an error reply.

**`/vref/fscale` — added as chip-global.** `VREF_FSCALE[1:0]` in `VREF_MICBIAS_CFG (0x4D)` selects the codec's internal VREF voltage. This is per-board (different AVDD voltages require different VREF settings) and is typically set once at boot, but exposed as a writable leaf so engineers can adjust during bring-up.

**`/micbias/enable` — added as chip-global.** `PWR_CFG[5] MICBIAS_PDZ` is the one and only MICBIAS power-enable register; there is no per-channel MICBIAS bit. **Important caveat:** when `EN_MBIAS_GPIO[7:6]` in `VREF_MICBIAS_CFG` is non-zero, the GPIO/GPI pin state silently shadows the I²C `MICBIAS_PDZ` value. The library reads back `EN_MBIAS_GPIO` on every micbias write and surfaces a warning in the OSC reply when GPIO override is active, so the engineer doesn't think their write succeeded silently. The future leaf `/micbias/gpio_control` is reserved for the pin-driven case.

**`/micbias/level` — added as chip-global.** `MICBIAS_VAL[3:2]` in `VREF_MICBIAS_CFG` selects the bias output voltage. Combined with `VREF_FSCALE`, the achievable voltages per Table 7-16 are 2.75 V, 2.5 V, 1.375 V, 1.25 V, and AVDD (direct bypass). Some `(MICBIAS_VAL, VREF_FSCALE)` combinations are reserved — the library does a read-modify-write of `0x4D` and validates the combination before committing, returning an error reply on invalid combos.

**Library implementation note: VREF and MICBIAS share register 0x4D.** Because `vref/fscale`, `micbias/level`, and the future `micbias/gpio_control` all live in the same register, the library must do read-modify-write with cross-field validation on every leaf in this group. Treat the three fields as a single transaction internally.

## Conventions and idioms

These are the small choices that, if applied consistently, make the protocol feel coherent. Most are X32-derived.

### Faders are normalized 0..1 on the wire, not dB

The X32 convention is that fader OSC messages carry `0.0..1.0` and the client applies a logarithmic mapping for display (showing dB labels on a non-linear scale). We follow this. Reasons:

- Avoids round-trip rounding errors when a client moves a fader and sees its own value echoed back.
- Lets the server do simple arithmetic on the value (averaging, automation) without log conversions.
- Matches what existing X32-aware clients send and expect.

The dB mapping (where `0.0` = `-∞ dB`, `0.75` = `0 dB`, `1.0` = `+10 dB`) is documented but not encoded in the OSC payload.

### Mute is `mix/on` inverted

`mix/on 0` means muted; `mix/on 1` means unmuted. This is annoying — every newcomer trips on it — but it's the X32 idiom and matches what clients send. Diverging would break compatibility for almost no benefit.

### EQ band count is a compile-time constant per example

`constexpr int EqBandsPerChannel = 4;` in TDspMixer's config header, default 4 for input channels. The address tree uses bands `1`..`N`. Bus channels can have a different constant if we want. Future examples may override.

### HPF is a separate stage, not band 1 of the EQ

X32 treats the high-pass filter as a pre-EQ stage with its own subtree (`/ch/NN/preamp/hpf/...`). We do the same. This means the 4 EQ bands are all available for parametric work; HPF doesn't consume one. Implementation: a separate `AudioFilterBiquad_F32` instance dedicated to HPF, before the EQ chain.

### Stereo linking is explicit

Odd channels carry a `config/link` flag. When set, the odd channel and its even neighbor share fader, mute, solo, EQ, and sends; pan is independent (typically defaulted to hard L/R). The mixer model holds the link flag; the binding applies it. Clients that aren't link-aware see two channels and can use either; the firmware mirrors writes to both sides automatically.

### Solo is solo-in-place (SIP) in v1

When any `ch/NN/mix/solo = 1`, the main bus mixes only soloed channels; non-soloed channels are muted to main. Solo does not affect the capture bus. The mode is hardcoded for v1; `/config/solo/mode` is reserved for future expansion (AFL/PFL).

### Meters are blobs, not individual messages

Per-channel meter values are packed into a single OSC blob (an array of `float32`) and sent as one message. A 6-channel input meter blob is `6 channels × 2 values (peak, RMS) × 4 bytes = 48 bytes`. Compare to 12 individual OSC messages (~600 bytes with framing). Bandwidth and packet rate both win by 10×.

The blob layout per address is documented in the meter blob format spec (TBD: write this when MeterEngine is implemented). Order is fixed and never reordered without bumping a version field in `/info`.

### Subscriptions follow the X32 `/xremote` idiom

Clients send `/sub addSub i i s [args...]` (interval ms, lifetime ms, address pattern, optional extra args) to begin receiving periodic updates of an address. Server fires the address back periodically. Client renews via `/sub renew i s` before lifetime expires. Server stops streaming after lifetime expires without a renew.

This is implemented via OSCAudio's `OSCSubscribe` (which is standalone — depends only on `OSCUtils`, not `OSCAudioBase`). T-DSP wraps it in `lib/TDspMixer/SubscriptionMgr` for any TDspMixer-specific glue.

For backward compatibility with classic X32 clients, `/xremote` is aliased to `/sub addSub` with default interval and a 10-second lifetime.

### Reply / echo semantics

When a client writes a value (e.g. `/ch/01/mix/fader f 0.7`), the dispatcher:

1. Mutates the model.
2. Calls the binding to apply the change to the audio graph.
3. **Echoes** the new value on the same address to all subscribed clients (including the originator).

The echo is what keeps multiple clients in sync. If two phones have the same fader on screen, moving one moves the other.

For commands that don't have a "value" (like `/codec/tac5212/reset`), the dispatcher replies with `/codec/tac5212/reset/done` or similar, in an `OSCBundle` reply. This follows OSCAudio's `addReplyExecuted` pattern.

## The CLI escape hatch

In addition to binary SLIP-OSC frames, the firmware accepts a **dotted-path text CLI** on the same USB CDC stream. This is a development convenience and an emergency recovery channel for when OSC tooling isn't available.

### How the multiplexing discriminates

On every byte received from `Serial`:

- If the byte is `0xC0` (SLIP END), it begins a SLIP frame. Route subsequent bytes through the SLIP decoder until the closing `0xC0`. Hand the resulting payload to the OSC parser.
- Otherwise, accumulate into a line buffer until `\n`. Hand the buffered line to the CLI shim.

`0xC0` is not a printable ASCII character, not a common control character, and not produced by `Serial.print` from any normal code path. The discrimination is unambiguous.

### The CLI is a thin shim, not a separate protocol

The CLI does not have its own handler implementations. Its parser is ~50 lines:

1. Tokenize the line into address parts and arguments.
2. Convert dotted-path → slash-path (`codec.tac5212.adc.1.gain` → `/codec/tac5212/adc/1/gain`).
3. Build an `OSCMessage` with the address and typed args (parse `0.7` as float, `1` as int, quoted strings as string).
4. Hand the `OSCMessage` to the same dispatcher that handles real OSC frames.

This is the **single source of truth** principle: the address tree is defined once, in the dispatcher's handlers. The CLI reaches every feature automatically because the shim feeds the same dispatcher. CLI ↔ OSC parity is guaranteed by construction — there's no way for them to drift.

### CLI examples

```
codec.tac5212.adc.1.gain 6.0          → /codec/tac5212/adc/1/gain f 6.0
codec.tac5212.out.1.mode diff_line    → /codec/tac5212/out/1/mode s "diff_line"
codec.tac5212.reset                   → /codec/tac5212/reset
ch.01.mix.fader 0.75                  → /ch/01/mix/fader f 0.75
ch.01.mix.on 0                        → /ch/01/mix/on i 0
info                                  → /info
```

Replies come back as plain text — the dispatcher's reply bundle is rendered as one line per message in dotted-path form so the user can read it in a terminal.

### Why keep both

The OSC binary form is for real clients. The CLI form is for:

- A developer debugging at the bench with `picocom` open.
- Testing handlers without writing client code.
- Emergency recovery when OSC tooling is broken.
- Smoke-testing during firmware development.

It costs ~50 lines of code and gives a real safety net. Worth it.

## Reference material

- **X32 OSC protocol** (community-maintained, unofficial but widely used): the design model for the address tree shape.
- **Open Stage Control** docs: how clients are expected to map UIs to OSC trees. Good sanity check that the tree we design renders as a usable mixer in a generic OSC surface tool.
- **OSC 1.0 specification** (opensoundcontrol.org): pattern matching syntax, type tags.
- **OSCAudio README** (in `lib/OSCAudio/README.md` after vendoring): how OSCAudio's `/audio` and `/dynamic` namespaces work, for the debug surface coexistence.
