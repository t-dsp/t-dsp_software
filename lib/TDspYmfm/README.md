# TDspYmfm

Yamaha FM chip emulation ([ymfm](https://github.com/aaronsgiles/ymfm) by Aaron
Giles) exposed as a Teensy Audio Library source. v1 wraps the **OPM (YM2151)** —
8 channels × 4 operators — as `AudioSynthYmfmOPM`, a MIDI-playable, patch-driven
stereo synth.

- **License:** ymfm is **BSD-3-Clause**. A ymfm-only firmware carries no GPL
  obligation (unlike `synth_dexed`, which is GPL v3).
- **Footprint:** ~87 KB flash, ~69 KB RAM for the whole OPM spike. CPU on a
  Teensy 4.1: ~10 % for one note, ~18 % for a 4-note chord.
- **Vendored scope:** only the pure-FM core (`ymfm.h`, `ymfm_fm.h/.ipp`,
  `ymfm_opm.h/.cpp`) lives under `src/ymfm/`. No ADPCM/PCM/OPL/OPN, so there is
  no external-ROM memory interface to implement and no build-time network fetch.

## Why a wrapper is needed

ymfm is a **register-level chip emulator** — it has no note or patch API. You
write chip registers and call `generate()`. `AudioSynthYmfmOPM` adds the missing
synth layer:

- an **8-voice allocator** with oldest-note stealing,
- a **MIDI-note → OPM key-code** map,
- **patch application** (`OpmVoice` → registers),
- a **resampler**: the chip runs at the real 3.579545 MHz clock (native rate
  ≈ 55.9 kHz) and `update()` linear-resamples to the audio graph rate, so pitch
  and timbre are authentic with no detune math.

Register writes are guarded by `AudioNoInterrupts()` so the audio update ISR
can't tear a multi-register sequence.

## Public API

```cpp
#include <AudioSynthYmfmOPM.h>

AudioSynthYmfmOPM opm;                 // stereo source: output 0 = L, 1 = R

void setup() {
    AudioMemory(40);                   // call BEFORE opm.begin()
    opm.begin();                       // reset chip, size resampler, load default voice
    opm.setVoice(tdsp::ymfmopm::kAdditiveOrgan);   // or kElectricPiano
    opm.setGain(2.5f);                 // optional; default is kDefaultGain (2.0)
}

// performance API — call from loop()/MIDI handlers, NOT the audio ISR:
opm.noteOn(note, velocity);            // velocity 1..127 (0 == noteOff)
opm.noteOff(note);
opm.allNotesOff();
```

Wire it into an int16 graph exactly like any Teensy synth:

```cpp
AudioConnection cL(opm, 0, mixerL, 0);
AudioConnection cR(opm, 1, mixerR, 0);
```

To bridge into an OpenAudio **F32** graph (e.g. the mix-kit), feed each output
through an `AudioConvert_I16toF32`, the same pattern the Dexed source uses.

## Multitimbral multi-bank

`YmfmOpmMulti` (header-only) manages several `AudioSynthYmfmOPM` **banks** — each
a full independent OPM chip with its own patch and 8 voices — routed by MIDI
channel. N banks give N distinct simultaneous timbres and N×8 total polyphony.
The banks are file-scope Teensy Audio objects; the manager borrows pointers to
them (like `MpeVaSink`) and adds channel→bank routing.

```cpp
AudioSynthYmfmOPM b0, b1, b2, b3;                       // file scope
AudioSynthYmfmOPM *banks[] = { &b0, &b1, &b2, &b3 };
tdsp::ymfmopm::YmfmOpmMulti multi(banks, 4);

multi.begin();                                          // begins every bank
multi.setBankVoice(0, tdsp::ymfmopm::kAdditiveOrgan);  // ch1 -> Organ
multi.setBankVoice(1, tdsp::ymfmopm::kFmBass);         // ch2 -> Bass  ...
multi.noteOn(channel, note, vel);                      // routed to that channel's bank
multi.setVoiceTable(voices, n);                        // optional: Program Change -> voice
```

**Idle cost is ~zero.** `AudioSynthYmfmOPM` has an idle gate: once a bank holds no
notes (after a short release-tail hold), `update()` stops calling the chip and
emits silence, so an instantiated-but-silent bank costs almost nothing. Measured:
4 banks idle ≈ **0.1 % CPU**; all 4 sounding at once ≈ **43 %**; one bank's 8-note
chord ≈ **53 %**. So 4 banks is comfortable and 6–8 is feasible on a Teensy 4.1.
`activeVoices()` (per engine, and summed on the manager) reports live voice count.

## Patches

`OpmVoice.h` defines a plain-data patch struct (per-channel algorithm/feedback +
4 operators) and four presets:

- `kAdditiveOrgan` — algorithm 7 (all operators are carriers), chosen as the
  default because it sounds regardless of operator-order conventions.
- `kElectricPiano` — algorithm 5, a true modulated FM patch.
- `kFmBass` — algorithm 4 with heavy feedback, a buzzy plucked low end.
- `kBellVibes` — algorithm 5 with high-multiple modulators, inharmonic bell/vibes.

(The modulated presets' exact timbres are approximate until validated by ear;
they are audibly distinct, which is what the multitimbral demo needs.)

## Importing VOPM `.opm` banks

`OpmBank.h` parses the standard VOPM/vgmrips `.opm` text bank format into
`OpmVoice[]` — so you can load the large public libraries of OPM patches instead
of hand-authoring them. `OpmVoice` owns its name inline and `setVoice()` keeps
its own copy, so parsed voices need no lifetime management.

```cpp
#include <OpmBank.h>
tdsp::ymfmopm::OpmVoice bank[128];
int n = tdsp::ymfmopm::parseOpmBank(text, len, bank, 128);  // text = a whole .opm file
opm.setVoice(bank[i]);
```

The parser maps `CON`→algorithm, `FL`→feedback, and each operator **by label**
(`M1`→slot 0, `M2`→1, `C1`→2, `C2`→3) so file line-order can't scramble a patch.
It's tolerant of CRLF, blank lines, comments, and whitespace, and does no I/O —
read a file into RAM and hand it the buffer. (`LFO`/`PAN`/`SLOT`/`NE`/`AMS`/`PMS`
are parsed-past for now.)

**Runtime loading from SD:** `spike_midi_ymfm_opm` scans `/ymfm/*.opm` on the
microSD at boot and appends every parsed voice to its instrument catalog (after a
small baked demo bank). Drop `.opm` files into `/ymfm` via a card reader (or MTP)
and they appear as instruments with no rebuild — the same pattern the mix-kit uses
for `/songs/*.mid`.

**Where to get banks:** VOPM ships factory patches; large community/game-rip
collections exist (e.g. vgmrips wiki + forums, linuxsynths, rekkerd). Patch
parameter data isn't itself the engine's concern — any standards-compliant `.opm`
file parses.

Add voices by declaring more `OpmVoice` constants. The VOPM `.opm` bank format
maps directly onto these fields if you want to import existing OPM patches.

## Using it as a MIDI-player backend

To drive it from `tdsp::MidiSink` (the synth-agnostic player in
`lib/TDspMidiPlayer` / `projects/spike_midi_player`), wrap it in a thin
`YmfmSink : public tdsp::MidiSink`. See
`projects/spike_midi_player/YMFM_INTEGRATION.md` for the build-flag integration
recipe and a ready-to-paste sink.

## Reference

- Standalone bring-up spike: `projects/spike_midi_ymfm_opm/`
- ymfm upstream: <https://github.com/aaronsgiles/ymfm>
