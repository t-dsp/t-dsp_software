# PSS-140 OPLL (YM2413) voice patches

The 100 custom instrument voices of the **Yamaha PSS-140** PortaSound keyboard,
captured as **OPLL user-voice register sets** (8 bytes each = YM2413 registers
`$00`–`$07`).

## Why these exist / what they are

The PSS-140 is built around a Yamaha **YM2413 (OPLL)** chip. The OPLL's melodic
side has only **15 built-in ROM instruments** — so the keyboard's 100 "voices"
are *not* in the chip. They are custom 2-operator FM patches stored in the
**keyboard's firmware ROM**, which the PSS-140 writes into the chip's single
user-voice register bank at patch-select time.

`ymfm` contains only the 15 chip-ROM patches, so these 100 are the *only* way to
reproduce the actual PSS-140 timbres. Loading one into OPLL instrument slot 0
(the user voice) and selecting instrument 0 gives that patch on the chip.

## Provenance

- **Source:** plgDavid — <https://github.com/plgDavid/misc/tree/master/OPLL%20Synth%20Patches>
  (`pss140_patches.txt`, `pss140_patches_names.txt`).
- **How captured:** logic-analyzer (Saleae) trace of the register writes on real
  hardware, decoded to the 8-byte user-voice sets.
- **Verified:** independently confirmed patch-for-patch correct by Ben Boldt.
  Discussion: nesdev forum thread 23328 —
  <https://forums.nesdev.org/viewtopic.php?t=23328>.

## License / usage — IMPORTANT

plgDavid released these marked **"copyrighted, please only use for study."**
They are kept here for **preservation and study**, not redistribution. Do **not**
publish them as part of a shipped product or a public release. (This file lives in
a working repo purely so the data isn't lost.)

## Format

- `pss140_patches.txt` — 100 lines, one patch per line, 8 space-separated hex
  bytes each prefixed with `$` (e.g. `$13 $01 $18 $0F $9E $60 $00 $9F`). The bytes
  are OPLL registers `$00,$01,$02,$03,$04,$05,$06,$07` in order.
- `pss140_names.txt` — 100 lines, the patch name for the matching line index.

## OPLL user-voice byte layout (regs $00–$07)

| Byte | Reg | Meaning |
|-----:|-----|---------|
| 0 | $00 | modulator: AM, VIB, EG-type, KSR, multiple |
| 1 | $01 | carrier:   AM, VIB, EG-type, KSR, multiple |
| 2 | $02 | modulator: key-scale-level (2b) + total level (6b) |
| 3 | $03 | carrier KSL (2b), modulator/carrier waveform, feedback (3b) |
| 4 | $04 | modulator: attack (4b) / decay (4b) |
| 5 | $05 | carrier:   attack (4b) / decay (4b) |
| 6 | $06 | modulator: sustain-level (4b) / release (4b) |
| 7 | $07 | carrier:   sustain-level (4b) / release (4b) |

To sound a patch: write these 8 bytes to `$00`–`$07`, then play a note on a
channel whose instrument nibble (`$3x` high nibble) is `0` (the user voice).
