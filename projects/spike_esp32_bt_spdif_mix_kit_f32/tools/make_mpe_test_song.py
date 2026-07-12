#!/usr/bin/env python3
# make_mpe_test_song.py — generate an MPE demo MIDI for the mix-kit song player.
#
# MPE encodes each note on its own channel with per-channel pitch bend + pressure.
# This writes /songs/mpetest.mid: (1) a C-major chord where the MIDDLE note bends
# independently while the others hold, (2) pressure swells (volume follows finger
# pressure), (3) a phrase where each note scoops into pitch with a pressure swell.
# Copy the output to the SD card's /songs and play it (the player dispatches per-note
# bend via kPitchBend and pressure via kChannelPressure -> the synth sink onPressure).
#
#   python3 make_mpe_test_song.py [out.mid]
import struct, sys

OUT = sys.argv[1] if len(sys.argv) > 1 else "mpetest.mid"
DIV, TPS = 480, 960          # ticks/quarter, ticks/sec at 120 bpm
evs = []                     # (tick, midi-bytes)
def add(t, b): evs.append((int(t), bytes(b)))
def prog(t, ch, p):  add(t, [0xC0 | ch, p])
def non(t, ch, n, v): add(t, [0x90 | ch, n, v])
def noff(t, ch, n):  add(t, [0x80 | ch, n, 0])
def cc(t, ch, c, v): add(t, [0xB0 | ch, c, v])
def press(t, ch, v): add(t, [0xD0 | ch, max(0, min(127, int(v)))])
def bendv(t, ch, val):
    val = max(0, min(16383, int(val))); add(t, [0xE0 | ch, val & 0x7f, (val >> 7) & 0x7f])
def bend_semis(t, ch, semis, rng=12): bendv(t, ch, 8192 + (semis / rng) * 8191)
def ramp(t0, dur, fn):       # call fn(tick) ~every 15 ms over [t0, t0+dur]
    n = max(1, int(dur / (TPS * 0.015)))
    for k in range(n + 1): fn(t0 + dur * k / n)

CH = [2, 3, 4, 5]            # MPE member channels
# Only set the per-channel bend range (12 semis). Deliberately NO program change, so the
# song plays whatever instrument the picker/'V' has selected -> audition MPE with any sound.
# (Pressure swells are most expressive on SUSTAINED patches: strings, pads, organ, brass.)
for ch in CH:
    cc(0, ch, 101, 0); cc(0, ch, 100, 0); cc(0, ch, 6, 12); cc(0, ch, 38, 0)

t = TPS // 2
# Part 1 — chord, then bend the MIDDLE note independently (C E G, one note/channel)
non(t, 2, 60, 95); non(t, 3, 64, 95); non(t, 4, 67, 95); t += TPS
ramp(t, TPS, lambda tt, t=t: bend_semis(tt, 3,  2 * ((tt - t) / TPS)));      t += TPS   # E -> +2
t += TPS // 2
ramp(t, TPS, lambda tt, t=t: bend_semis(tt, 3,  2 * (1 - (tt - t) / TPS)));  t += TPS   # back
noff(t, 2, 60); noff(t, 3, 64); noff(t, 4, 67); t += TPS // 2
# Part 2 — pressure swells (volume follows pressure)
for ch, note in [(2, 62), (3, 65)]:
    press(t, ch, 15); non(t, ch, note, 80)
    ramp(t, int(TPS * 1.4), lambda tt, ch=ch, t=t: press(tt, ch, 15 + 112 * ((tt - t) / (TPS * 1.4)))); t += int(TPS * 1.4)
    ramp(t, int(TPS * 1.4), lambda tt, ch=ch, t=t: press(tt, ch, 127 - 112 * ((tt - t) / (TPS * 1.4)))); t += int(TPS * 1.4)
    noff(t, ch, note); t += TPS // 3
# Part 3 — expressive phrase: each note scoops into pitch with a pressure swell
for i, note in enumerate([60, 62, 64, 67, 72]):
    ch = CH[i % len(CH)]; dur = int(TPS * 0.55)
    press(t, ch, 20); bend_semis(t, ch, -1); non(t, ch, note, 90)
    ramp(t, dur, lambda tt, ch=ch, t=t, dur=dur: bend_semis(tt, ch, -1 + (tt - t) / dur))
    ramp(t, dur, lambda tt, ch=ch, t=t, dur=dur: press(tt, ch, 20 + 90 * ((tt - t) / dur)))
    t += dur; noff(t, ch, note); t += int(TPS * 0.12)
tEnd = t + TPS

evs.sort(key=lambda x: x[0])
def vlq(n):
    b = [n & 0x7f]; n >>= 7
    while n: b.append((n & 0x7f) | 0x80); n >>= 7
    return bytes(reversed(b))
trk = bytearray(vlq(0) + bytes([0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20]))   # tempo 120
prev = 0
for tk, b in evs:
    trk += vlq(tk - prev) + b; prev = tk
trk += vlq(max(0, tEnd - prev)) + bytes([0xFF, 0x2F, 0x00])
data = b'MThd' + struct.pack('>IHHH', 6, 0, 1, DIV) + b'MTrk' + struct.pack('>I', len(trk)) + bytes(trk)
open(OUT, 'wb').write(data)
print("wrote %s: %d bytes, %d events, ~%.1fs" % (OUT, len(data), len(evs), tEnd / TPS))
