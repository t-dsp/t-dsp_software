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

## Patches

`OpmVoice.h` defines a plain-data patch struct (per-channel algorithm/feedback +
4 operators) and two presets:

- `kAdditiveOrgan` — algorithm 7 (all operators are carriers), chosen as the
  default because it sounds regardless of operator-order conventions.
- `kElectricPiano` — algorithm 5, a true modulated FM patch (timbre approximate
  until validated on hardware).

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
