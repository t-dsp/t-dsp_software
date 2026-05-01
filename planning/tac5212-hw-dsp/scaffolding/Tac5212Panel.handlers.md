# Tac5212Panel handler additions

This file lists the new handler signatures that need to land in
`projects/.../src/Tac5212Panel.{h,cpp}`. Treat it as a checklist for
phase implementation. Each handler:

1. Parses the OSC message args.
2. Calls a `lib/TAC5212` method (typed accessor preferred over raw
   `writeRegister`).
3. Appends a confirmation echo to the reply bundle so the UI knows
   the firmware accepted the value.
4. Surfaces `Result::warning`/`Result::error` strings via the OSC reply
   so the dev surface can flash the affected control red on failure.

## Per-output DAC controls

```cpp
// New private members in Tac5212Panel.h
void handleDacDvol(int n, OSCMessage &msg, OSCBundle &reply);
void handleDacBiquadCoeffs(int n, int idx, OSCMessage &msg, OSCBundle &reply);
void handleDacBiquadDesign(int n, int idx, OSCMessage &msg, OSCBundle &reply); // optional convenience
void handleDacEqPreset(int n, OSCMessage &msg, OSCBundle &reply);

void handleDacDrcEnable(int n, OSCMessage &msg, OSCBundle &reply);
void handleDacDrcThreshold(int n, OSCMessage &msg, OSCBundle &reply);
void handleDacDrcRatio(int n, OSCMessage &msg, OSCBundle &reply);
void handleDacDrcAttack(int n, OSCMessage &msg, OSCBundle &reply);
void handleDacDrcRelease(int n, OSCMessage &msg, OSCBundle &reply);
void handleDacDrcMaxGain(int n, OSCMessage &msg, OSCBundle &reply);
void handleDacDrcKnee(int n, OSCMessage &msg, OSCBundle &reply);

void handleDacLimiterEnable(int n, OSCMessage &msg, OSCBundle &reply);
void handleDacLimiterThreshold(int n, OSCMessage &msg, OSCBundle &reply);
void handleDacLimiterAttack(int n, OSCMessage &msg, OSCBundle &reply);
void handleDacLimiterRelease(int n, OSCMessage &msg, OSCBundle &reply);
void handleDacLimiterKnee(int n, OSCMessage &msg, OSCBundle &reply);
```

## Chip-global DAC DSP

```cpp
void handleDacInterp(OSCMessage &msg, OSCBundle &reply);
void handleDacHpf(OSCMessage &msg, OSCBundle &reply);
void handleDacBiquadCount(OSCMessage &msg, OSCBundle &reply);
void handleDacDvolGang(OSCMessage &msg, OSCBundle &reply);
void handleDacSoftStep(OSCMessage &msg, OSCBundle &reply);
```

## Address dispatch table

The existing `route()` parses paths like `/adc/N/<leaf>` and `/out/N/mode`
by char-comparing prefixes. New entries follow the same pattern:

```cpp
// Inside Tac5212Panel::route()
if (matches(addr, "/dac/")) {
    // Parse channel index
    int n = parseChannelIndex(addr + 5, &leafStart);  // returns 1 or 2
    if (matches(leafStart, "/dvol"))     handleDacDvol(n, msg, reply);
    else if (matches(leafStart, "/bq/")) {
        int idx = parseBiquadIndex(leafStart + 4, &nextLeaf);  // 1..3
        if (matches(nextLeaf, "/coeffs")) handleDacBiquadCoeffs(n, idx, msg, reply);
        else if (matches(nextLeaf, "/design")) handleDacBiquadDesign(n, idx, msg, reply);
    }
    else if (matches(leafStart, "/eq/preset"))      handleDacEqPreset(n, msg, reply);
    else if (matches(leafStart, "/drc/enable"))     handleDacDrcEnable(n, msg, reply);
    else if (matches(leafStart, "/drc/threshold")) handleDacDrcThreshold(n, msg, reply);
    // ... etc
}
else if (exact(addr, "/dac/biquads"))     handleDacBiquadCount(msg, reply);
else if (exact(addr, "/dac/interp"))      handleDacInterp(msg, reply);
else if (exact(addr, "/dac/hpf"))         handleDacHpf(msg, reply);
else if (exact(addr, "/dac/dvol_gang"))   handleDacDvolGang(msg, reply);
else if (exact(addr, "/dac/soft_step"))   handleDacSoftStep(msg, reply);
```

## Snapshot expansion

`Tac5212Panel::snapshot()` reads back DVOL, biquad coefficients, DRC
state, limiter state for each output channel and emits the matching
echoes. Pseudo-code:

```cpp
void Tac5212Panel::snapshot(OSCBundle &reply) {
    // ... existing output mode + ADC dvol echoes ...

    // DAC DVOL (per channel)
    for (int n = 1; n <= 2; ++n) {
        float dB; if (_codec.out(n).getDvol(dB).isOk())
            replyAddFloat(reply, fmt("/codec/tac5212/dac/%d/dvol", n), dB);
    }

    // DAC biquad coefs (per channel × bands per channel)
    int bqPerCh = _codec.dacBiquadsPerChannel();
    for (int n = 1; n <= 2; ++n) {
        for (int idx = 1; idx <= bqPerCh; ++idx) {
            BiquadCoeffs c;
            if (_codec.out(n).getBiquad(idx, c).isOk()) {
                replyAddIntQuintet(reply, fmt("/codec/tac5212/dac/%d/bq/%d/coeffs", n, idx),
                                   c.n0, c.n1, c.n2, c.d1, c.d2);
            }
        }
    }

    // Chip-global DAC DSP
    InterpFilter interp; if (_codec.getDacInterpolationFilter(interp).isOk())
        replyAddString(reply, "/codec/tac5212/dac/interp", interpToOscStr(interp));
    DacHpf hpf; if (_codec.getDacHpf(hpf).isOk())
        replyAddString(reply, "/codec/tac5212/dac/hpf", hpfToOscStr(hpf));
    replyAddInt(reply, "/codec/tac5212/dac/biquads", bqPerCh);

    // ... DRC + limiter readbacks per output ...
}
```

## OSC arg-type conventions

| Leaf                              | Type    | Args                          |
|-----------------------------------|---------|-------------------------------|
| `/codec/tac5212/dac/N/dvol`       | `f`     | dB                            |
| `/codec/tac5212/dac/N/bq/I/coeffs`| `iiiii` | n0, n1, n2, d1, d2 (int32)    |
| `/codec/tac5212/dac/N/bq/I/design`| `sfff`  | type, freqHz, gainDb, q       |
| `/codec/tac5212/dac/biquads`      | `i`     | 0..3                          |
| `/codec/tac5212/dac/interp`       | `s`     | enum string                   |
| `/codec/tac5212/dac/hpf`          | `s`     | enum string                   |
| `/codec/tac5212/dac/dvol_gang`    | `i`     | 0/1                           |
| `/codec/tac5212/dac/soft_step`    | `i`     | 0/1                           |
| `/codec/tac5212/dac/N/drc/enable` | `i`     | 0/1                           |
| `/codec/tac5212/dac/N/drc/*`      | `f`     | engineering unit (dB / ms)    |
| `/codec/tac5212/dac/N/limiter/*`  | `f` / `i`| as above                     |
| `/codec/tac5212/dac/N/eq/preset`  | `s`     | "flat" \| "smooth" \| etc.    |
