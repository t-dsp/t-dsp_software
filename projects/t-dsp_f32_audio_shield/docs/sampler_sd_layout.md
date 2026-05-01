# Sampler slot — SD card layout

The Phase 2 multisample slot (slot 1 in the synth switcher) streams WAV
files directly off the Teensy 4.1's BUILTIN_SDCARD. This doc covers the
hardware setup and the bank format on the card.

## Card hardware

Use the **built-in SD slot on the Teensy 4.1** (4-bit SDIO). The audio
shield's SD slot is 1-bit SPI and too slow for polyphonic streaming —
PJRC users consistently report stutter under load.

Recommended cards (in order of how often they appear on the PJRC forum):

1. **SanDisk Industrial** — gold standard for embedded audio. Low
   random-read latency, which is what matters for polyphonic streaming.
2. **SanDisk Extreme PRO** UHS-I U3 V30 A2 (32–64 GB) — consumer
   alternative.
3. **Samsung EVO Select / EVO Plus** — okay backup.

Avoid generic "Class 10" cards with no UHS-I rating; the controller's
random-read latency stalls the audio queue.

Format: FAT32 is the safe default (most SD cards ship with this
already). exFAT also works on Teensy 4.1 SD.

## Directory layout

```
/samples/
  piano/
    A0.wav            # single-layer
    A0_v8.wav         # one velocity layer
    A0_v16.wav        # another velocity layer
    C4_v1.wav         # softest
    C4_v8.wav         # mf (medium-loud)
    C4_v16.wav        # fff (loudest)
    ...
  rhodes/
    C2.wav
    ...
  drums/
    ...
```

The slot scans `/samples/<bank>/` at boot. The default bank path the
firmware tries is `/samples/piano` — change it by editing the
`scanBank()` call in `main.cpp` (a runtime OSC selector lands in a
follow-up phase).

### Velocity layers

Filenames may include a `v<N>` tag (with or without an underscore) to
mark which velocity layer they are. `<N>` is 1–16 mapped linearly to
MIDI velocity 8–127:

| v-tag | MIDI velocity |
|-------|--------------|
| v1    | 8 (softest, pp) |
| v4    | 32 |
| v8    | 64 (mf) |
| v12   | 95 |
| v16   | 127 (loudest, fff) |

Files without a v-tag are treated as MIDI velocity 64 (mf).

The voice picker uses **hierarchical** selection: closest root note
first, then closest velocity layer among ties. So a played `(C4, v=100)`
prefers `C4_v12.wav` (velocity 95) over `C4_v8.wav` (velocity 64),
and falls back to `C#4_v16.wav` only if no `C4` sample exists at all.

`MultisampleSlot::kMaxSamples` is 1024 — covers a fully velocity-
layered piano (Salamander Grand at 16 layers ≈ 480 files in practice).

## File format requirements

- **48 kHz** sample rate (matches the firmware's `AUDIO_SAMPLE_RATE_EXACT`).
  44.1 kHz files play ~8.8% sharp because the underlying lib doesn't
  rate-correct.
- **16-bit signed PCM** (the `teensy-variable-playback` lib only
  supports 16-bit int).
- **Stereo** (the slot expects 2 channels). Mono works but only the
  left channel is heard.
- **Filename = note name**, no leading zeros, no spaces. Examples:
  `C4.wav`, `F#3.wav`, `Bb-1.wav`, `A0.wav`. Both `#` and `b`
  accidentals are accepted.
- File size — keep individual WAVs under a few MB each. SD bandwidth
  is the polyphony ceiling; longer/larger files mean fewer simultaneous
  voices.

## How notes get pitched

For each MIDI note coming in, the slot finds the **closest** sample by
root note and adjusts playback rate to pitch-shift. Example with the
piano layout above:

- MIDI 60 (C4) → plays `C4.wav` at rate 1.0
- MIDI 61 (C#4) → plays `C4.wav` at rate 2^(1/12) ≈ 1.059 (1 semitone up)
- MIDI 65 (F4) → plays `F#4.wav` at rate 2^(-1/12) ≈ 0.944 (1 semitone down)

A wider sample spacing (e.g., one sample per octave) saves SD space
but pitches notes further from their root → more aliasing, less natural.
Roughly one sample every 3–6 semitones is typical for a high-quality
piano patch.

## Polyphony

8 voices. Voice stealing prefers idle slots, then voices already in
their release phase, then the oldest active voice. Held release tails
keep streaming until the per-voice envelope decays (200 ms default), so
a chord released cleanly can use up to 8 voices' worth of SD bandwidth
during the tail.

If you hear glitches under polyphony stress: try a faster SD card,
shorten the envelope release, or reduce the number of simultaneous
notes.

## Salamander Grand Piano

Salamander Grand Piano V3 (CC-BY by Alexander Holm) is the canonical
"really good piano" content for embedded sampler projects. It ships
as 16 velocity layers per note in OGG format.

### Manual workflow (until salamander-trimmed auto-download lands)

1. **Download** Salamander Grand Piano V3 from
   <https://archive.org/details/SalamanderGrandPianoV3> (or the
   sfzinstruments mirror).
2. **Extract** the archive to a folder (e.g. `C:\salamander\`). Files
   look like `A0v8.ogg`, `C4v12.ogg` — note name then `v<1..16>` for
   the velocity layer.
3. **Install ffmpeg** if you don't already have it
   (<https://ffmpeg.org/download.html>) — required for OGG → WAV
   conversion.
4. **Convert + push** in one command:
   ```bash
   python tools/fetch_samples.py source-dir \
       --source-dir C:/salamander \
       --velocity 8 \
       --push
   ```
   `v8` is roughly mf (medium-loud) and tends to be the sweet spot
   for general-purpose piano playing. Try `v6` for softer / `v12` for
   brighter if v8 doesn't feel right.
5. **Re-flash** the audio firmware (the file_mode.sh orchestration
   script handles this; or run `tools/file_mode.sh --restore-only`
   manually).

### Why filtered velocity layers

The slot's filename-as-note convention only takes one sample per note
(no velocity layers in the bank). Filtering by `v8` selects exactly
one layer per note — `A0v8.ogg`, `A1v8.ogg`, ..., `C8v8.ogg`. The
script's `--max-samples 24` cap (default) thins ~88 piano notes down
to a sparse 24-sample set that fits the slot's `kMaxSamples`
budget.

### Trimmed sample list (for reference)

Roughly evenly distributed across the keyboard, ~24 samples works
well:

```
A0.wav, C2.wav, E2.wav, A2.wav, C3.wav, E3.wav, A3.wav,
C4.wav, E4.wav, A4.wav, C5.wav, E5.wav, A5.wav,
C6.wav, E6.wav, A6.wav, C7.wav, A7.wav, C8.wav
```

That's 19 samples ≈ 60–80 MB on a 48 kHz / 16-bit / stereo card,
sounds great, and stays well within the 24-sample-per-bank limit
in `MultisampleSlot::kMaxSamples`.

## Other piano sources

The `source-dir` mode is filename-driven, so any folder of WAV / OGG
/ FLAC / MP3 files where each filename contains a recognisable note
name will work. Examples:

- **Splendid Grand Piano** (smaller than Salamander)
- **Iowa Electronic Music Studios** piano samples
- **VS Chamber Orchestra** (has piano)
- **Free Wave Samples** piano keys
- Your own recordings of a real piano

The note-name parser in `tools/fetch_samples.py` looks for the first
`<letter>[#b]?<octave>` pattern in the basename (e.g. `piano_C4.wav`
extracts `C4`), so most naming conventions Just Work.

## Troubleshooting

- **Serial says "SD card init failed"** — card not inserted, wrong
  slot (use BUILTIN_SDCARD, not the audio shield's), or card is
  faulty. Run the `SDtest` example from PJRC to verify.
- **Serial says "bank not found on SD"** — `/samples/piano/` directory
  doesn't exist on the card. Create it and copy WAVs.
- **Serial says "loaded: 0 samples"** — files exist but none parse as
  note names. Check filenames: `C4.wav` not `c4.wav` (case-insensitive
  but the parser rejects spaces, hyphens before letters, etc.).
- **Notes play at wrong pitch** — WAV sample rate isn't 48 kHz, OR the
  filename doesn't match the actual note in the file.
- **Stuttering / dropouts** — SD card too slow. Move from the audio
  shield's slot to the Teensy's built-in slot, or try a SanDisk
  Industrial / Extreme PRO card.
- **Volume too loud / too quiet** — adjust `/synth/dexed/mix/fader`
  via OSC (the per-slot fader for slot 1 will get its own OSC handler
  in a follow-up phase). Master synth fader at `/synth/bus/mix/fader`
  works today.
