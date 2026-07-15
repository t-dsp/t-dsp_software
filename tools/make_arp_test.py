#!/usr/bin/env python3
"""make_arp_test.py — generate arp_test.mid: a small, VALID Standard MIDI File
that demos BOTH the arpeggiator and MPE per-note expression in one file.

@ 120 BPM, division 480 tpq. Written deliberately clean (explicit status,
correct delta VLQs) — the opposite of the malformed dmp_midi grooves.

  Section 1 — ARP  (channel 1, plain):
      4 single notes, one per beat (C4 D4 E4 F4), then 4 block chords (C F G C).
      Turn the arpeggiator on to hear each note/chord arpeggiated.

  Section 2 — MPE  (member channels 2..5, per-note X/Y/Z):
      4 single notes then 4 chords, each note on its OWN channel carrying its own
      X = pitch bend, Y = CC74 timbre, Z = channel pressure. In MPE mode every
      note bends / sweeps / swells independently — the chords show three voices
      each doing their own thing. (Play in MPE mode to hear true per-note MPE;
      in normal multitimbral mode it still demos per-channel X/Y/Z.)

Loadable from SD /songs/arp_test.mid (Rebuild catalog, then pick it in MIDI Player).
"""
import struct, sys, math

TPQ = 480
Q, H = TPQ, TPQ * 2                 # quarter / half note
BEND_CENTER = 8192

# Absolute-time event list: (tick, rank, status, d1, d2). rank orders same-tick
# events: expression/setup (0) -> note-off (1) -> note-on (2).
evs = []
def add(tick, status, d1, d2, rank):
    evs.append((tick, rank, status, d1, d2))

def note_plain(ch, note, t0, dur):
    add(t0, 0x90 | ch, note, 100, 2)
    add(t0 + dur, 0x80 | ch, note, 0, 1)

def bend(ch, t, value, rank=0):     # 14-bit, LSB then MSB
    v = max(0, min(16383, int(value)))
    add(t, 0xE0 | ch, v & 0x7F, (v >> 7) & 0x7F, rank)

def note_mpe(ch, note, t0, dur, phase=0.0):
    """A note with X/Y/Z automation over its life, on its own channel."""
    # setup at t0 (before the note-on): centre bend, timbre 0, pressure 0
    bend(ch, t0, BEND_CENTER, rank=0)
    add(t0, 0xB0 | ch, 74, 0, 0)                 # Y (CC74 timbre)
    add(t0, 0xD0 | ch, 0, 0, 0)                  # Z (channel pressure)
    add(t0, 0x90 | ch, note, 100, 2)             # note on
    N = 8
    for s in range(1, N + 1):
        tt = t0 + dur * s // N
        a = math.pi * (s / N) + phase
        bend(ch, tt, BEND_CENTER + math.sin(a) * 8191, rank=0)   # X: bend up & back
        add(tt, 0xB0 | ch, 74, round(127 * s / N), 0)            # Y: timbre opens 0->127
        add(tt, 0xD0 | ch, round(127 * abs(math.sin(a))), 0, 0)  # Z: pressure swell
    add(t0 + dur, 0x80 | ch, note, 0, 1)         # note off
    bend(ch, t0 + dur, BEND_CENTER, rank=0)      # recentre after release

# ---- Section 1: ARP (channel 0 = MIDI ch 1) --------------------------------
t = 0
for n in (60, 62, 64, 65):          # C4 D4 E4 F4, one per beat
    note_plain(0, n, t, Q); t += Q
for chord in [(60, 64, 67), (65, 69, 72), (67, 71, 74), (60, 64, 67)]:   # C F G C
    for n in chord: note_plain(0, n, t, H)
    t += H

# ---- Section 2: MPE (member channels 2..5 = 0-based 1..4) -------------------
t += Q                              # one beat of rest between sections
for k, n in enumerate((60, 62, 64, 65)):        # 4 notes, each own channel + X/Y/Z
    note_mpe(1 + k, n, t, Q, phase=k * 0.6); t += Q
for chord in [(60, 64, 67), (65, 69, 72), (67, 71, 74), (60, 64, 67)]:   # 4 MPE chords
    for j, n in enumerate(chord):               # each voice on its own channel, own phase
        note_mpe(1 + j, n, t, H, phase=j * 1.3)
    t += H

# ---- serialize to a single-track SMF ---------------------------------------
def vlq(n):
    b = [n & 0x7F]; n >>= 7
    while n: b.insert(0, (n & 0x7F) | 0x80); n >>= 7
    return bytes(b)

evs.sort(key=lambda e: (e[0], e[1]))
trk = bytearray()
trk += vlq(0) + b"\xFF\x51\x03" + (500000).to_bytes(3, "big")   # 120 BPM at tick 0
last = 0
for tick, _rank, status, d1, d2 in evs:
    trk += vlq(tick - last)
    # channel pressure (0xD0) and program change (0xC0) carry ONE data byte; all
    # other channel-voice messages carry two. Writing the wrong count corrupts every
    # following delta (this is exactly what the malformed dmp grooves suffer from).
    trk += bytes((status, d1)) if (status & 0xF0) in (0xC0, 0xD0) else bytes((status, d1, d2))
    last = tick
trk += vlq(0) + b"\xFF\x2F\x00"                                 # end of track

hdr = b"MThd" + struct.pack(">IHHH", 6, 0, 1, TPQ)
data = hdr + b"MTrk" + struct.pack(">I", len(trk)) + bytes(trk)
out = sys.argv[1] if len(sys.argv) > 1 else "arp_test.mid"
with open(out, "wb") as f: f.write(data)
print(f"wrote {out} ({len(data)} bytes, {len(evs)} events)")
