#!/usr/bin/env python3
# Generates test_songs.h — baked MidiFileEvent[] MIDI/MPE test sequences.
# Event tuple: (deltaMs, kind, channel, data1, data2)
ON, OFF, CC, PB, CP = "ON", "OFF", "CC", "PB", "CP"

def cp_ramp(ch, p0, p1, total_ms, step_ms=15):
    # Smooth channel-pressure (MPE Z) ramp from p0 to p1 over total_ms, as fine steps.
    n = max(1, round(total_ms / step_ms))
    out = []
    for i in range(1, n + 1):
        p = round(p0 + (p1 - p0) * i / n)
        out.append((step_ms, CP, ch, max(0, min(127, p)), 0))
    return out

def bend(v):  # 14-bit bend value (center 8192) -> (LSB, MSB)
    v = max(0, min(16383, v))
    return (v & 0x7F, (v >> 7) & 0x7F)

CENTER = 8192

def glide(ch, v0, v1, total_ms, step_ms=10):
    # Emit a smooth pitch-bend ramp from 14-bit value v0 to v1 over total_ms, as many
    # small steps (step_ms apart). Fine steps => a continuous glide, not audible stairs.
    n = max(1, round(total_ms / step_ms))
    out = []
    for i in range(1, n + 1):
        v = round(v0 + (v1 - v0) * i / n)
        out.append((step_ms, PB, ch, *bend(v)))
    return out

def sweep():
    ev = []
    notes = list(range(48, 73)) + list(range(71, 47, -1))  # chromatic up 2 oct span then back
    first = True
    for n in notes:
        ev.append((0 if first else 15, ON, 0, n, 100)); first = False
        ev.append((95, OFF, 0, n, 0))
    return ev

def chord():
    ev = []
    # I - IV - V - I triads (C major), each held ~650ms with a short gap
    chords = [(60,64,67), (65,69,72), (67,71,74), (60,64,67,72)]
    first = True
    for ch in chords:
        for i, n in enumerate(ch):
            ev.append((0 if (first and i==0) else (0 if i else 40), ON, 0, n, 96)); first = False
        for i, n in enumerate(ch):
            ev.append((650 if i==0 else 0, OFF, 0, n, 0))
    return ev

def velocity():
    ev = []
    n = 60  # middle C at rising velocity: pp -> ff
    first = True
    for v in (16, 32, 48, 64, 80, 96, 112, 127):
        ev.append((0 if first else 120, ON, 0, n, v)); first = False
        ev.append((260, OFF, 0, n, 0))
    return ev

def arpeggio():
    ev = []
    # Cmaj7 arpeggio up and down, twice, fast
    pat = [60, 64, 67, 71, 72, 71, 67, 64]
    first = True
    for _ in range(3):
        for n in pat:
            ev.append((0 if first else 5, ON, 0, n, 100)); first = False
            ev.append((85, OFF, 0, n, 0))
    return ev

def pitchbend():
    n = 62  # hold a note, smoothly sweep bend up -> down -> back to center
    ev = [(0, ON, 0, n, 105)]
    ev.append((30, CC, 0, 101, 0)); ev.append((0, CC, 0, 100, 0)); ev.append((0, CC, 0, 6, 12))  # range 12
    ev += glide(0, CENTER, 16383, 700)          # up
    ev.append((300, PB, 0, *bend(16383)))       # hold high
    ev += glide(0, 16383, 0, 1300)              # down through center to min
    ev.append((300, PB, 0, *bend(0)))           # hold low
    ev += glide(0, 0, CENTER, 700)              # back to center
    ev.append((200, OFF, 0, n, 0))
    return ev

def sustain():
    ev = []
    # sustain pedal down; play staccato notes that should RING through the pedal; pedal up = stop
    ev.append((0, CC, 0, 64, 127))              # sustain ON
    first_off = True
    for n in (60, 64, 67, 72):
        ev.append((20, ON, 0, n, 100))
        ev.append((150, OFF, 0, n, 0))          # key released but pedal holds the voice
    ev.append((900, CC, 0, 64, 0))              # sustain OFF -> all held voices release
    return ev

def mpe_bend():
    # C-major triad across 3 MPE channels; bend ONLY the middle voice (E) down a full
    # semitone to Eb and back. Per-note MPE => the chord goes major -> minor -> major
    # (unmistakable). If the bend instead leaked to every note, the whole chord would
    # just slide down and STAY major -> so the quality change is the proof it's per-note.
    ev = []
    ev.append((0, ON, 1, 60, 100))              # C  (MIDI ch2) — steady anchor
    ev.append((0, ON, 2, 64, 100))              # E  (MIDI ch3) — the ONE note we bend
    ev.append((0, ON, 3, 67, 100))              # G  (MIDI ch4) — steady anchor
    # RPN 0,0 on ch3(idx2): bend range = 1 semitone, so a full bend is exactly a semitone
    ev.append((1000, CC, 2, 101, 0)); ev.append((0, CC, 2, 100, 0)); ev.append((0, CC, 2, 6, 1))
    ev += glide(2, CENTER, 0, 900)              # E -> Eb, smoothly
    ev.append((800, PB, 2, *bend(0)))           # hold Eb  (C minor)
    ev += glide(2, 0, CENTER, 900)              # Eb -> E, smoothly
    ev.append((800, PB, 2, *bend(CENTER)))      # hold E   (C major)
    ev.append((0, OFF, 1, 60, 0)); ev.append((0, OFF, 2, 64, 0)); ev.append((0, OFF, 3, 67, 0))
    return ev

def mpe_octave():
    # A held dead-steady; B (starts in unison) sweeps UP a full octave then back DOWN to
    # unison — on its OWN MPE channel. Per-note MPE => you hear B rise an octave above the
    # steady A and return. If the bend leaked to all notes, A would rise too and the two
    # would stay locked in unison the whole time -> so the separation is the proof.
    ev = []
    ev.append((0, ON, 1, 60, 100))              # A (MIDI ch2) — steady anchor, C4
    ev.append((0, ON, 2, 60, 100))              # B (MIDI ch3) — unison with A to start
    # RPN 0,0 on ch3(idx2): bend range = 12 semitones so a full up-bend is exactly 1 octave
    ev.append((600, CC, 2, 101, 0)); ev.append((0, CC, 2, 100, 0)); ev.append((0, CC, 2, 6, 12))
    ev += glide(2, CENTER, 16383, 1400)          # smoothly UP a full octave (0 -> +12)
    ev.append((700, PB, 2, *bend(16383)))        # hold: B an octave above A
    ev += glide(2, 16383, CENTER, 1400)          # smoothly back DOWN to unison
    ev.append((700, PB, 2, *bend(CENTER)))       # back to unison with A
    ev.append((0, OFF, 1, 60, 0)); ev.append((0, OFF, 2, 60, 0))
    return ev

def mpe_pressure():
    # ONE note; channel pressure (MPE Z-axis) swells up and down smoothly, driving the
    # sound via the pool's aftertouch->EG routing. FIRST 3 SLOW up/down cycles, THEN 6
    # FASTER up/down cycles — so pressure is heard both as a slow swell and quick pulses.
    ev = [(0, ON, 1, 60, 100), (40, CP, 1, 0, 0)]
    for _ in range(2):                      # 2 slow up/down swells (~1.8 s each)
        ev += cp_ramp(1, 0, 127, 900)
        ev += cp_ramp(1, 127, 0, 900)
    for _ in range(4):                      # 4 quick up/down swells (~0.6 s each)
        ev += cp_ramp(1, 0, 127, 300)
        ev += cp_ramp(1, 127, 0, 300)
    for _ in range(2):                      # 2 more slow up/down swells
        ev += cp_ramp(1, 0, 127, 900)
        ev += cp_ramp(1, 127, 0, 900)
    ev.append((60, OFF, 1, 60, 0))
    return ev

TESTS = [
    ("kSweep",    "01 Midi Test Sweep",      sweep(),        "false"),
    ("kChord",    "02 Midi Test Chord",      chord(),        "false"),
    ("kVelocity", "03 Midi Test Velocity",   velocity(),     "false"),
    ("kArp",      "04 Midi Test Arpeggio",   arpeggio(),     "false"),
    ("kPB",       "05 Midi Test Pitch Bend", pitchbend(),    "false"),
    ("kSustain",  "06 Midi Test Sustain",    sustain(),      "false"),
    ("kMpeBend",  "07 MPE Test Bend",        mpe_bend(),     "true"),
    ("kMpeOct",   "08 MPE Test Octave",      mpe_octave(),   "true"),
    ("kMpePress", "09 MPE Test Pressure",    mpe_pressure(), "true"),
]

def emit():
    L = []
    L.append("// test_songs.h — built-in MIDI + MPE test sequences, baked as MidiFileEvent[]")
    L.append("// (the player's rich format: note on/off, CC, pitch bend, channel pressure).")
    L.append("// GENERATED by tools/gen_test_songs.py — edit the generator, not this file.")
    L.append("//")
    L.append("// Registered at the FRONT of the song catalog (see buildSongList) so they show")
    L.append("// first in the app/serial picker as \"01 Midi Test ...\" .. \"08 MPE Test ...\".")
    L.append("// MPE entries set mpe=true so songStart flips the device into MPE mode first.")
    L.append("#pragma once")
    L.append('#include "MidiFileEvent.h"')
    L.append("")
    L.append("namespace testsong {")
    L.append("using tdsp::MidiFileEvent;")
    L.append("static constexpr uint8_t ON  = tdsp::kNoteOn;")
    L.append("static constexpr uint8_t OFF = tdsp::kNoteOff;")
    L.append("static constexpr uint8_t CC  = tdsp::kControlChange;")
    L.append("static constexpr uint8_t PB  = tdsp::kPitchBend;")
    L.append("static constexpr uint8_t CP  = tdsp::kChannelPressure;")
    L.append("")
    for var, name, ev, mpe in TESTS:
        L.append(f"// {name}  ({len(ev)} events)")
        L.append(f"static const MidiFileEvent {var}[] = {{")
        line = "  "
        for e in ev:
            tok = "{%d,%s,%d,%d,%d}," % e
            if len(line) + len(tok) > 96:
                L.append(line); line = "  "
            line += tok
        if line.strip():
            L.append(line)
        L.append("};")
        L.append("")
    L.append("struct TestSong { const char *name; const MidiFileEvent *ev; uint32_t count; bool mpe; };")
    L.append("static const TestSong kTestSongs[] = {")
    for var, name, ev, mpe in TESTS:
        L.append(f'  {{ "{name}", {var}, (uint32_t)(sizeof({var})/sizeof({var}[0])), {mpe} }},')
    L.append("};")
    L.append("static const int kNumTestSongs = (int)(sizeof(kTestSongs)/sizeof(kTestSongs[0]));")
    L.append("")
    L.append("} // namespace testsong")
    L.append("")
    return "\n".join(L)

import sys
open(sys.argv[1], "w", encoding="utf-8").write(emit())
print("wrote", sys.argv[1])
for var, name, ev, mpe in TESTS:
    print(f"  {name}: {len(ev)} events, mpe={mpe}")
