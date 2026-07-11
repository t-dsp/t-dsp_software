# TDspMidiPlayer

Synth-agnostic MIDI file playback for the T-DSP platform. Parses a Standard
MIDI File at runtime (or plays a baked event array) and fans the events into a
`tdsp::MidiSink` — so the **same player drives Dexed today and ymfm (or any
future engine) tomorrow**. The synth choice lives entirely on the sink side.

```
 .mid on SD ──parseSmf()──▶ MidiFileEvent[] ──▶ MidiFilePlayer ──▶ tdsp::MidiSink ──▶ [ synth ]
 baked SongEv[] ─expandLegacyNotes()─▶                (tick())        (DexedSink /
                                                                       YmfmSink / …)
```

## Why this exists

The old mix-kit player (`sd_midi.h` + `songTick`) collapsed every track to
note+velocity on one channel, and **dropped** MIDI channel, program-change,
CC, and pitch-bend — and stripped channel 10 (drums) at parse time. That is
fine for "one Dexed patch plays all the notes," but it throws away everything a
multi-timbral or drum-capable backend needs.

This library keeps all of it, behind the existing `tdsp::MidiSink` abstraction,
so the playback layer never has to change when the synth engine does.

## Pieces

| Header | Role | Dependencies |
|---|---|---|
| `MidiFileEvent.h` | Packed event: deltaMs + kind + channel + data1/data2 | none |
| `MidiSmfParser.h` | `parseSmf(buf,len,out,maxOut)` — pure SMF → events | none (unit-testable off-target) |
| `MidiSmfFile.h`   | `loadSmfFile(path,...)` — SD convenience loader | `<SD.h>` (pulled in itself) |
| `MidiFilePlayer.h`| Non-blocking `tick()` sequencer → `MidiSink` | `<MidiSink.h>` (TDspMidi) |

`parseSmf` preserves per-note **channel** (0..15), emits **program-change**,
**CC**, and **pitch-bend** inline in tick order, and keeps channel 10 (drums)
in the stream — filtering is a *playback policy*, not a parse decision.

## Channel policy (drums)

`MidiFilePlayer::setChannelMask()` chooses which channels are emitted:

- `kMaskNoDrums` (default) — all channels except index 9 (MIDI ch 10). Right
  for a single melodic engine so it does not try to play kick/snare notes.
- `kMaskAll` — everything. Use once a multi-timbral / drum backend exists.

## Usage

```cpp
#include <MidiFilePlayer.h>
#include <MidiSmfFile.h>          // for SD .mid loading

static DMAMEM tdsp::MidiFileEvent g_buf[24000];
tdsp::MidiFilePlayer g_player;
DexedSink            g_sink(&g_dexed);   // any tdsp::MidiSink

void setup() { g_player.setSink(&g_sink); }

void playSong(const char *path) {
    int n = tdsp::smf::loadSmfFile(path, g_buf, 24000);
    if (n > 0) g_player.play(g_buf, (uint32_t)n);
}

void loop() { g_player.tick(); }         // non-blocking
```

Baked legacy songs (`SongEv[]`) play through the same buffer:

```cpp
uint32_t n = tdsp::expandLegacyNotes(kWilliamTellSong, count, g_buf, 24000);
g_player.play(g_buf, n);
```

## Backend swap (the build flag)

`spike_midi_player` selects the synth at compile time via `-D TDSP_SYNTH_*`.
Adding ymfm is: provide a `YmfmSink : public tdsp::MidiSink`, instantiate it
under the ymfm branch, and point `g_player.setSink()` at it. The player, the
parser, and the song catalog do not change. To hear the drum track, that
backend also calls `g_player.setChannelMask(tdsp::MidiFilePlayer::kMaskAll)`.
