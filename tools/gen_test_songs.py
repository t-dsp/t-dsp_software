#!/usr/bin/env python3
# Generates test_songs.h — baked MidiFileEvent[] MIDI/MPE test sequences.
# Event tuple: (deltaMs, kind, channel, data1, data2)
ON, OFF, CC, PB, CP = "ON", "OFF", "CC", "PB", "CP"

import math

def cc_ramp(ch, cc, v0, v1, total_ms, step_ms=15):
    # Smooth CC ramp (used for CC74 timbre / MPE Y-axis).
    n = max(1, round(total_ms / step_ms)); out = []
    for i in range(1, n + 1):
        v = round(v0 + (v1 - v0) * i / n)
        out.append((step_ms, CC, ch, cc, max(0, min(127, v))))
    return out

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

def semis_to_bend(semi):
    # semitone offset (relative to note) -> 14-bit bend value, assuming per-note range = 12.
    return max(0, min(16383, round(CENTER + semi / 12.0 * 8192.0)))

def vib(ch, amp_semi, cycles, total_ms, center_semi=0.0, step_ms=20):
    # Per-note vibrato as a small sine pitch-bend around center_semi (per-note => forEachTarget).
    n = max(1, round(total_ms / step_ms)); out = []
    for i in range(1, n + 1):
        s = center_semi + amp_semi * math.sin(2 * math.pi * cycles * i / n)
        out.append((step_ms, PB, ch, *bend(semis_to_bend(s))))
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

def mpe_demo():
    # A guided tour of EVERY MPE function on one instrument (loop it, change the patch to
    # hear each). Member channels: MIDI ch2 = ev-channel 1, ch3 = ev-channel 2.
    n = 62; ch = 1
    rpn12 = lambda c: [(0, CC, c, 101, 0), (0, CC, c, 100, 0), (0, CC, c, 6, 12)]  # bend range 12
    ev = []
    # 1) PITCH BEND (X): up an octave and back, then down and back
    ev.append((0, ON, ch, n, 100)); ev += rpn12(ch)
    ev += glide(ch, CENTER, 16383, 700) + glide(ch, 16383, 0, 1200) + glide(ch, 0, CENTER, 700)
    ev.append((200, OFF, ch, n, 0))
    # 2) TIMBRE (Y / CC74): brightness sweeps up and down
    ev.append((350, ON, ch, n, 100))
    ev += cc_ramp(ch, 74, 64, 127, 700) + cc_ramp(ch, 74, 127, 0, 900) + cc_ramp(ch, 74, 0, 64, 500)
    ev.append((200, OFF, ch, n, 0))
    # 3) PRESSURE (Z): volume + brightness swell up and down
    ev.append((350, ON, ch, n, 100))
    ev += cp_ramp(ch, 0, 127, 800) + cp_ramp(ch, 127, 0, 800)
    ev.append((200, OFF, ch, n, 0))
    # 4) ALL THREE AT ONCE (one expressive note)
    ev.append((350, ON, ch, n, 100)); ev += rpn12(ch)
    for i in range(1, 49):
        p = i / 48.0
        lsb, msb = bend(round(CENTER + (16383 - CENTER) * math.sin(p * 2 * math.pi)))
        ev.append((25, PB, ch, lsb, msb))
        ev.append((0, CC, ch, 74, max(0, min(127, round(63 + 64 * (0.5 - 0.5 * math.cos(p * 2 * math.pi)))))))
        ev.append((0, CP, ch, max(0, min(127, round(127 * (0.5 - 0.5 * math.cos(p * 2 * math.pi))))), 0))
    ev.append((200, OFF, ch, n, 0))
    # 5) PER-NOTE INDEPENDENCE: two notes; bend + press ONLY the top one
    ev.append((400, ON, 1, 60, 100))                 # anchor (MIDI ch2), stays put
    ev.append((0, ON, 2, 64, 100)); ev += rpn12(2)   # expressive (MIDI ch3)
    for i in range(1, 41):
        p = i / 40.0
        lsb, msb = bend(round(CENTER + CENTER * 0.6 * math.sin(p * 2 * math.pi)))
        ev.append((30, PB, 2, lsb, msb))
        ev.append((0, CP, 2, max(0, min(127, round(127 * (0.5 - 0.5 * math.cos(p * 2 * math.pi))))), 0))
    ev.append((200, OFF, 1, 60, 0)); ev.append((0, OFF, 2, 64, 0))
    return ev

def mpe_showcase():
    # A short MUSICAL piece (not a test sweep) that shows off the gestures a piano roll
    # physically cannot do — designed to loop and to sound good on ONE sustained, clearly-
    # pitched voice (analog strings / brass / a synth lead). Member channels: ev-ch 1..4
    # = MIDI ch 2..5. Everything expressive rides per-note pitch bend (our strongest axis)
    # and per-note channel pressure (volume swell); vibrato is a small sine bend (also
    # per-note). rpn12 sets each voice's bend range to 12 so the semitone math is exact.
    rpn12 = lambda c: [(0, CC, c, 101, 0), (0, CC, c, 100, 0), (0, CC, c, 6, 12)]
    ev = []

    # ---- I. FAN-OUT BLOOM -------------------------------------------------
    # Four voices strike in UNISON on C4, then glide outward into a C-major chord spread
    # over two octaves (C3 - E4 - G4 - C5). Impossible without per-note bend: on a normal
    # keyboard four notes on one pitch can only ever stay one pitch.
    for c in (1, 2, 3, 4):
        ev.append((0, ON, c, 60, 96)); ev += rpn12(c)      # all four at C4
    ev.append((250, PB, 1, *bend(CENTER)))                 # brief unison hold
    tgt = {1: -12, 2: +4, 3: +7, 4: +12}                   # -> C3, E4, G4, C5
    for c, s in tgt.items():
        ev += glide(c, CENTER, semis_to_bend(s), 2200, step_ms=20)
    # bloomed chord breathes (all four swell together, then settle)
    for c in (1, 2, 3, 4): ev += cp_ramp(c, 0, 110, 900)
    for c in (1, 2, 3, 4): ev += cp_ramp(c, 110, 40, 900)
    ev += [(0, OFF, 1, 60, 0), (0, OFF, 2, 60, 0), (0, OFF, 3, 60, 0), (0, OFF, 4, 60, 0)]

    # ---- II. BREATHING CHORD ----------------------------------------------
    # A Cmaj7 held as real notes; each voice swells on its OWN pressure envelope at a
    # different phase, and two voices get gentle vibrato at different rates -> the chord
    # shimmers and breathes like a live string section rather than a static pad.
    voices = [(1, 60), (2, 64), (3, 67), (4, 71)]          # C4 E4 G4 B4
    ev.append((350, ON, 1, 60, 88))
    for c, n in voices[1:]: ev.append((0, ON, c, n, 88)); ev += rpn12(c)
    ev += rpn12(1)
    # staggered pressure swells (phase-offset) — interleave as independent streams
    for c, delay in ((1, 0), (2, 500), (3, 1000), (4, 250)):
        seg = [(delay, CP, c, 0, 0)] if delay else [(0, CP, c, 0, 0)]
        seg += cp_ramp(c, 0, 120, 1400) + cp_ramp(c, 120, 30, 1400)
        ev += seg
    # per-note vibrato on the 3rd and top (different speeds), everyone else dead steady
    ev += vib(2, 0.18, 6, 1800) + vib(4, 0.12, 4, 1800)
    for c, n in voices: ev.append((0, OFF, c, n, 0))

    # ---- III. GUITAR BEND LEAD --------------------------------------------
    # A dead-steady power-chord drone (C3 + G3) while a lead voice plays a bluesy phrase
    # with classic string bends + vibrato. The drone NOT moving while the lead bends is the
    # per-note proof — on a mono-bend synth the whole chord would slide with the lead.
    ev.append((400, ON, 1, 48, 92)); ev += rpn12(1)        # C3 drone
    ev.append((0, ON, 2, 55, 92)); ev += rpn12(2)          # G3 drone
    ev += rpn12(3)                                          # lead channel armed
    def lead(note, bend_semi, hold_ms):
        s = []
        s.append((0, ON, 3, note, 108))
        s += glide(3, CENTER, semis_to_bend(bend_semi), 180, step_ms=18)   # bend up into pitch
        s += vib(3, 0.25, max(1, hold_ms // 300), hold_ms, center_semi=bend_semi)  # vibrato at target
        s.append((0, OFF, 3, note, 0))
        return s
    ev += lead(62, +2, 650)     # D4 bend up to E4
    ev += lead(67, +2, 650)     # G4 bend up to A4
    ev += lead(64, +3, 900)     # E4 bend up a minor third to G4, longer
    ev += [(150, OFF, 1, 48, 0), (0, OFF, 2, 55, 0)]

    # ---- IV. CHORD MORPH (voice leading) ----------------------------------
    # Strike Cmaj (C4 E4 G4 C5) once, then GLIDE the four voices through I - IV - V - I.
    # You hear each voice move independently to the next chord tone (per-note portamento) —
    # a slow pedal-steel voice-leading you cannot get from re-triggered block chords.
    struck = {1: 60, 2: 64, 3: 67, 4: 72}
    for c, n in struck.items(): ev.append((0, ON, c, n, 84)); ev += rpn12(c)
    ev.append((600, PB, 1, *bend(CENTER)))
    # target NOTES per chord; delta = target - struck (stays within +-12)
    prog = [
        {1: 60, 2: 65, 3: 69, 4: 72},   # F  (C  F  A  C)
        {1: 59, 2: 67, 3: 71, 4: 74},   # G  (B  G  B  D)
        {1: 60, 2: 64, 3: 67, 4: 72},   # C  (C  E  G  C)  resolve
    ]
    cur = {c: 0 for c in struck}
    for chord_targets in prog:
        for c in (1, 2, 3, 4):
            s0 = semis_to_bend(cur[c]); s1 = semis_to_bend(chord_targets[c] - struck[c])
            ev += glide(c, s0, s1, 1500, step_ms=22)
            cur[c] = chord_targets[c] - struck[c]
        ev.append((500, PB, 1, *bend(semis_to_bend(cur[1]))))   # let each chord ring
    # final swell + release on the resolved Cmaj
    for c in (1, 2, 3, 4): ev += cp_ramp(c, 30, 120, 1000)
    for c in (1, 2, 3, 4): ev += cp_ramp(c, 120, 0, 1200)
    for c, n in struck.items(): ev.append((0, OFF, c, n, 0))
    return ev

def mpe_timbre():
    # ISOLATED CC#74 (MPE Y / timbre) test. Nothing else moves — no pitch bend, no
    # pressure, constant velocity — so ANY change you hear is purely per-note brightness
    # (the pool maps CC#74 -> that note's modulator level / FM depth). Pick a SUSTAINED,
    # FM-rich voice first (Organ, or a PSS-140 brass/synth) to hear it clearly.
    # Member channels: ev-ch 1 = MIDI ch2, ev-ch 2 = MIDI ch3.
    ch = 1
    ev = []
    # 1) ONE held note: slow full brightness sweep dark->bright->dark, then faster.
    ev.append((0, ON, ch, 60, 100))
    ev.append((0, CC, ch, 74, 0))                       # start dark
    ev += cc_ramp(ch, 74, 0, 127, 1600) + cc_ramp(ch, 74, 127, 0, 1600)
    ev += cc_ramp(ch, 74, 0, 127, 800)  + cc_ramp(ch, 74, 127, 0, 800)
    ev.append((300, OFF, ch, 60, 0))
    # 2) STEPPED brightness: hold a note, JUMP CC#74 through discrete levels (~0.6 s each)
    #    so you hear distinct timbre steps, not a glide.
    ev.append((350, ON, ch, 62, 100))
    first = True
    for v in (0, 32, 64, 96, 127, 64, 0):
        ev.append((0 if first else 600, CC, ch, 74, v)); first = False
    ev.append((600, OFF, ch, 62, 0))
    # 3) PER-NOTE INDEPENDENCE (the MPE proof): two held notes a fifth apart. Sweep CC#74
    #    on the TOP note only while the BOTTOM stays dark; then swap. On a shared-timbre
    #    engine both would morph together — here only the swept note changes.
    ev.append((400, ON, 1, 55, 100)); ev.append((0, CC, 1, 74, 0))   # bottom (MIDI ch2), dark
    ev.append((0, ON, 2, 67, 100));   ev.append((0, CC, 2, 74, 0))   # top (MIDI ch3), dark
    ev += cc_ramp(2, 74, 0, 127, 1300) + cc_ramp(2, 74, 127, 0, 1300)  # sweep TOP only
    ev += cc_ramp(1, 74, 0, 127, 1300) + cc_ramp(1, 74, 127, 0, 1300)  # swap: sweep BOTTOM only
    ev.append((250, OFF, 1, 55, 0)); ev.append((0, OFF, 2, 67, 0))
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
    ("kMpeDemo",  "10 MPE Full Demo",        mpe_demo(),     "true"),
    ("kMpeShow",  "11 MPE Showcase",         mpe_showcase(), "true"),
    ("kMpeTimbre","12 MPE Timbre",           mpe_timbre(),   "true"),
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
        # PROGMEM keeps the array in (memory-mapped, directly-readable) flash instead of
        # letting the linker copy it into DTCM/RAM1 at boot — RAM1 is tight on this build.
        L.append(f"static const MidiFileEvent {var}[] PROGMEM = {{")
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
