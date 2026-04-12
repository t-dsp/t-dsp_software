# lib/TAC5212/

Register-level driver library for the TI TAC5212 pro-audio codec.

This library exposes **chip-specific primitives** including hardware gain
controls (ADC DVOL, DAC volume). Mixer-level concerns (fader, mute, EQ,
dynamics, metering) live in `lib/TDspMixer/`. See
`planning/osc-mixer-foundation/02-osc-protocol.md` for the canonical
`/codec/tac5212/...` OSC subtree that this class mirrors 1:1.

## Design rules

### Per-channel codec leaves must not have chip-global side effects

If a control affects more than one channel, it's exposed as a chip-global
method on `TAC5212` itself, not as a method on the `Adc` / `Out` accessor.
Example: MICBIAS power is one register bit (`PWR_CFG[5]`) that affects both
channels, so it lives on `TAC5212::setMicbiasEnable`, not on `AdcChannel`.

## API overview

```cpp
#include <TAC5212.h>

using namespace tac5212;

TAC5212 codec(Wire);               // construct, attach to a TwoWire

void setup() {
    Wire.begin();

    Result r = codec.begin(0x51);  // probe, reset, wake
    if (r.isError()) {
        Serial.println(r.message);
        return;
    }

    // Chip-global config
    codec.setVrefFscale(VrefFscale::V2p75);
    codec.setMicbiasLevel(MicbiasLevel::SameAsVref);

    // Boot-time audio interface configuration
    TAC5212::SerialFormat fmt;
    fmt.format    = TAC5212::Format::Tdm;
    fmt.wordLen   = TAC5212::WordLen::Bits32;
    fmt.bclkPol   = TAC5212::Polarity::Inverted;
    codec.setSerialFormat(fmt);
    codec.setRxChannelSlot(1, 0);    // DAC L1 <- slot 0
    codec.setRxChannelSlot(2, 1);    // DAC L2 <- slot 1
    codec.setTxChannelSlot(1, 0);    // ADC CH1 -> slot 0
    codec.setTxChannelSlot(2, 1);    // ADC CH2 -> slot 1

    // Per-channel ADC config
    codec.adc(1).setMode(AdcMode::SingleEndedInp);
    codec.adc(1).setImpedance(AdcImpedance::K5);
    codec.adc(2).setMode(AdcMode::SingleEndedInp);

    // Output drivers — HP mode for stereo TRS headphone on OUTxP pins
    codec.out(1).setMode(OutMode::HpDriver);
    codec.out(2).setMode(OutMode::HpDriver);

    // Power up
    codec.setChannelEnable(0xF, 0xC);  // IN1..4 + OUT1..2
    codec.powerAdc(true);
    codec.powerDac(true);

    // Report status
    codec.dumpStatus(Serial);
}
```

## Return-value contract

Every setter returns `tac5212::Result`:

```cpp
struct Result {
    ResultStatus status;  // Ok / Warning / Error
    const char  *message; // static C string or nullptr
    bool isOk()      const;
    bool isWarning() const;
    bool isError()   const;
};
```

- `Ok` — operation committed, no caveats.
- `Warning` — operation committed, but there's a caveat worth surfacing
  (e.g. `setMicbiasEnable` returns Warning when `EN_MBIAS_GPIO != 0`
  because the I2C bit is shadowed by the pin state).
- `Error` — operation did **not** commit. Invalid combination, I2C NACK,
  or out-of-range argument. The codec register state is unchanged.

`message` is a static C string. Callers can pass it verbatim into an OSC
reply bundle or `Serial.println` it — no allocation, no ownership concerns.

## Combo validation

The library validates compound register conditions that the datasheet calls
out as reserved or required:

### VREF_MICBIAS_CFG (register 0x4D)

Holds `VREF_FSCALE[1:0]`, `MICBIAS_VAL[3:2]`, and `EN_MBIAS_GPIO[7:6]` in
one byte. `setVrefFscale` and `setMicbiasLevel` both read-modify-write this
register and validate against datasheet Table 7-16. Reserved combinations
return `Result::error`:

- `VREF_FSCALE = 11b` — always reserved
- `MICBIAS_VAL = 10b` — always reserved
- `MICBIAS_VAL = 01b` combined with `VREF_FSCALE >= 10b` — reserved

### ADC_CH*_CFG0 combo rules

Validated on every `Adc::setImpedance` / `setFullscale` / `setCoupling` /
`setBw` call:

- `fullscale 4vrms` requires `coupling dc_rail_to_rail` + `bw 24khz` +
  chip-global `vref/fscale 2.75v`.
- `bw 96khz` requires `impedance 40k`.

## `reset()` semantics

`TAC5212::reset()` does exactly:

1. Software reset (self-clearing bit 0 of `SW_RESET` register 0x01)
2. Wait 15 ms for the chip to re-initialize
3. Wake via `DEV_MISC_CFG` register 0x02

That's it. The library does not apply any board-specific configuration
after reset — no audio interface format, no slot map, no GPIO routing, no
channel enables. The caller (`main.cpp`'s `setupCodec()` today, the
future `Tac5212Panel`'s reset handler tomorrow) is responsible for
re-applying its own session configuration. This keeps the library free of
opinions about any specific board wiring.

## Known ambiguities / deferred work

### `Pdm::setEnable` / `setSource` / `setClkPin`

Stubbed — return `Result::error("... not yet implemented")`. Need to
verify `INTF_CFG4` bitfield layout and whether GPIO2 is a valid PDM clock
pin on the TAC5212 before implementation. Datasheet re-check required.

### Shelf filters take slope, not Q

Two filter-cookbook variables describe shelving-filter transition
abruptness: `slope` (what OpenAudio's `setLowShelf` / `setHighShelf`
accept) and `Q` (what the `/ch/NN/eq/B/q` OSC leaf carries). They're
related but not the same parameter. When `SignalGraphBinding` wires
up shelf EQ bands, it will need to either (a) translate the OSC `q`
into a slope value via the RBJ cookbook relation, or (b) grow a new
`setLowShelfQ` / `setHighShelfQ` pair in the vendored OpenAudio copy
that takes Q directly. Not a TAC5212-library concern — flagging for
the TDspMixer binding work.

## Decisions that shaped this library

These are the calls from the parallel design-chat sync on 2026-04-11
that ended up as hard rules enforced at the type-system level. Recorded
here so future sessions don't re-litigate them.

1. **Per-channel leaves never have cross-channel side effects.** ADC
   HPF lives on `TAC5212::setAdcHpf(bool)` at the chip-global level,
   not on `TAC5212::Adc`, because DSP_CFG0's HPF_SEL field applies to
   both channels simultaneously. The OSC spec leaf moved from
   `/codec/tac5212/adc/N/hpf` to `/codec/tac5212/adc/hpf` to match.
2. **`AdcMode` has no `Pdm` value.** ADC channels 1/2 on the TAC5212
   are analog-only. PDM lives entirely in the `/codec/tac5212/pdm/...`
   subtree and routes to codec channels 3/4, not 1/2. The OSC spec
   `/adc/N/mode` enum dropped `"pdm_input"` to match.
3. **`MicbiasLevel` stays relative-to-VREF, not absolute volts.** The
   hardware register encodes a ratio (same-as-VREF / half-VREF / AVDD),
   so the library mirrors the ratio. The codec panel handler translates
   user-facing absolute-voltage strings like `"1.375v"` into the right
   `(vref/fscale, micbias_val)` pair before calling the library.
4. **`reset()` is narrow: SW reset + wake, no board-specific config.**
   TDM format, slot map, PDM routing, channel enables, and power-up
   stay in the project's `setupCodec()` (today) or `Tac5212Panel`
   (future). Library stays chip-generic so the same driver can serve
   multiple boards.
5. **Accessor pattern, not flat setters.** `codec.adc(1).setMode(...)`
   mirrors the OSC URL shape `/codec/tac5212/adc/1/mode` literally.
   Lower cognitive load in handler code; scales naturally to boards
   with more channels.

## Files

```
lib/TAC5212/
├── library.json                  PlatformIO manifest (minimal)
├── README.md                     this file
└── src/
    ├── TAC5212.h                 Public class, enums, Result
    ├── TAC5212.cpp               Implementation
    └── TAC5212_Registers.h       Private — register addresses and bitfield constants
```

## Build notes

The library is discovered by PlatformIO via `lib_extra_dirs = ../../lib`
in the project's `platformio.ini`. No `lib_deps` entry is required — LDF
walks `#include <TAC5212.h>` automatically.

`TAC5212_Registers.h` is a private header; user code should not include it
directly. The public API intentionally hides register addresses behind
enum-based setters so users don't have to think about bit positions.
Callers who need raw access fall back on `TAC5212::writeRegister` /
`readRegister`, which take `(page, reg, value)` triples.

The library assumes `-fno-rtti` and `-fno-exceptions` are in effect (the
Teensy platform default). `Result` is a plain value type; no `dynamic_cast`,
no `try`/`catch`.
