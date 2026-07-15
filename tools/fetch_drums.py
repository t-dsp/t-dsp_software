#!/usr/bin/env python3
"""fetch_drums.py — assemble MIDI drum grooves for the T-DSP "Drums" feature.

The mix-kit firmware (projects/spike_esp32_bt_spdif_mix_kit_f32) scans an SD
folder /drums/*.mid and exposes each groove in the app/web "Drums" menu. A
groove is a short, LOOPABLE, channel-10-only Standard MIDI File that plays
through whichever General-MIDI engine is built (TSF / SF2 / OPL3 / OPLL), so
you get a drum backing while you jam a melody live on the keyboard.

Outputs a folder ready to drag onto the card:

    <out>/drums/<name>.mid          (channel 10, GM drum notes)

Modes
-----
sample   (default, offline)
    Generate a handful of ORIGINAL one-bar grooves (rock/funk/pop/hiphop/
    jazz/latin) with the tiny SMF writer below. These are trivially-authored
    public-domain patterns — the set that is checked into the repo under
    assets/drums/ so the feature works with no download.

dmp      (network)
    Fetch the full MIT-licensed dmp_midi pack (460 drum-machine grooves from
    René-Pierre Bardet's "200/260 Drum Machine Patterns", converted to GM
    channel-10 MIDI) from https://github.com/gvellut/dmp_midi releases,
    normalize every file to channel 10, and stage a curated genre spread.

all
    sample + dmp.

Usage
-----
    python tools/fetch_drums.py sample
    python tools/fetch_drums.py sample --assets     # (re)write repo assets/drums
    python tools/fetch_drums.py dmp --limit 48
    python tools/fetch_drums.py all --push          # stage + copy to the card

By default the script writes to a staging directory (default
c:/tmp/t-dsp-drums) and prints instructions for dragging <out>/drums onto the
SD card. With --push it invokes tools/push_to_teensy.ps1 (same auto-detect as
fetch_samples.py). --assets (re)generates the checked-in sample set in the
repo instead of the staging dir.

Licensing
---------
The generated `sample` grooves are original and public-domain (see
assets/drums/CREDITS.md). The `dmp` pack is MIT-licensed; its LICENSE +
attribution are written next to the fetched files. Keep those files with the
grooves if you redistribute them.
"""

from __future__ import annotations

import argparse
import io
import os
import shutil
import struct
import subprocess
import sys
import urllib.request
import zipfile

# --- General MIDI percussion (channel 10) note numbers we use ----------------
KICK, RIM, SNARE, CLAP = 36, 37, 38, 39
CLHAT, PDHAT, OPHAT = 42, 44, 46          # closed / pedal / open hi-hat
LOWTOM, HITOM = 45, 50
CRASH, RIDE = 49, 51
LOWCONGA, HICONGA, HICONGA_OPEN = 64, 63, 62
CLAVES, LOWTIMB, HITIMB = 75, 66, 65
COWBELL, TAMB, SHAKER = 56, 54, 82

PPQN = 480                                # ticks per quarter note in the output
GM_DRUM_CHANNEL = 9                       # 0-based channel index (MIDI ch 10)


# --- Minimal Standard MIDI File (type 0) writer ------------------------------
# Enough to emit a single-track, channel-10 groove. No external deps.
def _vlq(n: int) -> bytes:
    """MIDI variable-length quantity."""
    out = bytearray([n & 0x7F])
    n >>= 7
    while n:
        out.insert(0, (n & 0x7F) | 0x80)
        n >>= 7
    return bytes(out)


class SmfTrack:
    """Accumulates (absolute-tick, event-bytes) and renders a type-0 SMF."""

    def __init__(self) -> None:
        self._events: list[tuple[int, bytes]] = []

    def note(self, tick: int, note: int, vel: int, dur: int) -> None:
        ch = GM_DRUM_CHANNEL
        self._events.append((tick, bytes([0x90 | ch, note & 0x7F, vel & 0x7F])))
        # Drums are one-shots; a short note-off keeps the file valid + tidy.
        self._events.append((tick + dur, bytes([0x80 | ch, note & 0x7F, 0])))

    def render(self, bpm: float = 120.0, loop_ticks: int | None = None) -> bytes:
        # loop_ticks defaults to one 4/4 bar (BAR is defined after this class, so we
        # compute it from PPQN here rather than as a default-arg expression).
        if loop_ticks is None:
            loop_ticks = PPQN * 4
        # Track chunk: tempo meta first, then time-sorted events, then End-of-Track.
        # Note-offs must sort before note-ons at the same tick (retrigger safety).
        def rank(ev: bytes) -> int:
            return 0 if (ev[0] & 0xF0) == 0x80 else 1

        # SEAMLESS LOOP: the firmware loops this file back-to-back, and the SMF parser
        # only keeps time up to the LAST note — trailing silence to the barline is lost,
        # so beat 1 of the next pass comes in early. Fix it here: clamp every event into
        # [0, loop_ticks] (drum tails past the barline are one-shots — safe to clamp) and
        # append a SILENT barline marker (a note-off on note 0, unmapped in GM percussion)
        # exactly at loop_ticks. That marker becomes the last event, so its lead-in wait
        # fills the bar and the loop period is exactly one bar.
        events = [(min(tick, loop_ticks), ev) for (tick, ev) in self._events]
        events.append((loop_ticks, bytes([0x80 | GM_DRUM_CHANNEL, 0, 0])))
        evs = sorted(events, key=lambda e: (e[0], rank(e[1])))
        body = bytearray()
        # tempo meta at tick 0
        usec = int(round(60_000_000 / bpm))
        body += _vlq(0) + bytes([0xFF, 0x51, 0x03]) + usec.to_bytes(3, "big")
        prev = 0
        for tick, ev in evs:
            body += _vlq(tick - prev) + ev
            prev = tick
        body += _vlq(0) + bytes([0xFF, 0x2F, 0x00])   # End of Track

        track = b"MTrk" + struct.pack(">I", len(body)) + bytes(body)
        header = b"MThd" + struct.pack(">IHHH", 6, 0, 1, PPQN)
        return header + track


# --- Original grooves --------------------------------------------------------
# Each builder lays down ONE bar (4/4). The firmware loops it seamlessly, so one
# tight bar is all a backing groove needs. Sixteenth = PPQN/4, eighth = PPQN/2.
S16 = PPQN // 4
S8 = PPQN // 2
BAR = PPQN * 4


def _hat_every(t: SmfTrack, step: int, note: int, vel: int, accents=()):
    for i, tick in enumerate(range(0, BAR, step)):
        v = vel + (18 if i in accents else 0)
        t.note(tick, note, min(127, v), step // 2)


def groove_rock() -> tuple[str, bytes]:
    t = SmfTrack()
    _hat_every(t, S8, CLHAT, 92, accents=(0, 4))            # straight 8ths
    for tick in (0, BAR // 2):                              # kick on 1 and 3
        t.note(tick, KICK, 118, S8)
    t.note(BAR // 4, SNARE, 112, S8)                        # snare on 2
    t.note(3 * BAR // 4, SNARE, 112, S8)                    # and 4 (backbeat)
    t.note(0, CRASH, 100, PPQN)                             # crash on the downbeat
    return "01 Rock Straight", t.render(120)


def groove_funk() -> tuple[str, bytes]:
    t = SmfTrack()
    _hat_every(t, S16, CLHAT, 78, accents=(0, 2, 4, 6, 8, 10, 12, 14))
    for tick in (0, 3 * S16, 4 * S16, 7 * S16, 10 * S16, BAR // 2 + 3 * S16):
        t.note(tick, KICK, 116, S16)                        # syncopated kick
    t.note(BAR // 4, SNARE, 114, S8)
    t.note(3 * BAR // 4, SNARE, 114, S8)
    t.note(6 * S16, SNARE, 60, S16)                         # ghost snare
    return "02 Funk 16ths", t.render(104)


def groove_pop() -> tuple[str, bytes]:
    t = SmfTrack()
    _hat_every(t, S8, CLHAT, 88)
    t.note(0, KICK, 116, S8)
    t.note(3 * S8, KICK, 110, S8)
    t.note(BAR // 4, SNARE, 110, S8)
    t.note(3 * BAR // 4, SNARE, 110, S8)
    return "03 Pop Backbeat", t.render(118)


def groove_hiphop() -> tuple[str, bytes]:
    t = SmfTrack()
    _hat_every(t, S16, CLHAT, 74)
    t.note(0, KICK, 120, S16)
    t.note(5 * S16, KICK, 112, S16)
    t.note(8 * S16, KICK, 118, S16)
    t.note(BAR // 4, SNARE, 116, S8)
    t.note(3 * BAR // 4, SNARE, 116, S8)
    t.note(14 * S16, OPHAT, 90, S16)
    return "04 Hip-Hop Boom Bap", t.render(88)


def groove_jazz() -> tuple[str, bytes]:
    t = SmfTrack()
    # Swung ride: quarter + swung-and (triplet feel ~2/3 of the beat).
    for beat in range(4):
        base = beat * PPQN
        t.note(base, RIDE, 96, S8)
        t.note(base + (2 * PPQN) // 3, RIDE, 82, S8)
    t.note(PPQN, PDHAT, 78, S8)                             # foot hat on 2
    t.note(3 * PPQN, PDHAT, 78, S8)                         # and 4
    t.note(0, KICK, 70, S8)                                 # feathered kick
    return "05 Jazz Swing Ride", t.render(132)


def groove_latin() -> tuple[str, bytes]:
    t = SmfTrack()
    # Son-clave-ish 3-2 with congas + shaker.
    for tick in range(0, BAR, S16):
        t.note(tick, SHAKER, 66, S16)
    for tick in (0, 3 * S8, 4 * S8):                        # 3 side
        t.note(tick, HICONGA, 104, S16)
    for tick in (5 * S8 + S16, 6 * S8):                     # 2 side
        t.note(tick, LOWCONGA, 108, S16)
    t.note(0, KICK, 100, S8)
    t.note(BAR // 2, KICK, 100, S8)
    t.note(BAR // 4, RIM, 96, S16)
    t.note(3 * BAR // 4, RIM, 96, S16)
    return "06 Latin Clave", t.render(110)


SAMPLE_GROOVES = [
    groove_rock, groove_funk, groove_pop, groove_hiphop, groove_jazz, groove_latin,
]


def write_sample(out_drums: str) -> int:
    os.makedirs(out_drums, exist_ok=True)
    n = 0
    for build in SAMPLE_GROOVES:
        name, data = build()
        path = os.path.join(out_drums, name + ".mid")
        with open(path, "wb") as f:
            f.write(data)
        print(f"  + {name}.mid ({len(data)} B)")
        n += 1
    return n


SAMPLE_CREDITS = """\
# Drum grooves — credits & licensing

## Built-in sample grooves (this folder's *.mid, unless noted below)
Generated by tools/fetch_drums.py. These are ORIGINAL, trivially-authored
one-bar General-MIDI (channel 10) patterns placed in the PUBLIC DOMAIN
(CC0). Do whatever you like with them.

## Full pack (fetched with `python tools/fetch_drums.py dmp`)
The optional pack comes from gvellut/dmp_midi (https://github.com/gvellut/dmp_midi),
which is MIT-licensed. If you redistribute those files, keep the LICENSE.dmp_midi
file the fetcher writes next to them and credit the source.
"""

# --- dmp_midi pack (MIT) -----------------------------------------------------
DMP_RELEASES_API = "https://api.github.com/repos/gvellut/dmp_midi/releases/latest"
DMP_LICENSE_URL = "https://raw.githubusercontent.com/gvellut/dmp_midi/master/LICENSE"


def _http_get(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "t-dsp-fetch-drums"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def _force_channel10(mid: bytes) -> bytes | None:
    """Rewrite every channel-voice message's channel nibble to 9 (MIDI ch 10).

    A crude but effective normalizer: drum-machine transcriptions sometimes land
    on channel 0. We only touch running-status-free files (dmp_midi emits
    explicit status bytes), which is the common case; if parsing looks unsafe we
    return the file unchanged so a valid SMF is never corrupted.
    """
    if mid[:4] != b"MThd":
        return None
    out = bytearray(mid)
    i = 0
    # Walk chunks; only rewrite within MTrk bodies, event status bytes 0x80..0xE0.
    while i + 8 <= len(out):
        ctype = bytes(out[i:i + 4])
        clen = struct.unpack(">I", out[i + 4:i + 8])[0]
        start = i + 8
        end = start + clen
        if end > len(out):
            return None                        # malformed; leave original
        if ctype == b"MTrk":
            j = start
            running = 0
            while j < end:
                # skip delta-time VLQ
                while j < end and (out[j] & 0x80):
                    j += 1
                j += 1
                if j >= end:
                    break
                b = out[j]
                if b == 0xFF:                  # meta
                    j += 1
                    length_start = j
                    while j < end and (out[j] & 0x80):
                        j += 1
                    mlen = 0
                    for k in range(length_start, j + 1):
                        mlen = (mlen << 7) | (out[k] & 0x7F)
                    j += 1 + mlen
                    continue
                if b in (0xF0, 0xF7):          # sysex
                    j += 1
                    length_start = j
                    while j < end and (out[j] & 0x80):
                        j += 1
                    slen = 0
                    for k in range(length_start, j + 1):
                        slen = (slen << 7) | (out[k] & 0x7F)
                    j += 1 + slen
                    continue
                if b & 0x80:
                    running = b
                    status = b
                    j += 1
                else:
                    status = running           # running status; data byte
                hi = status & 0xF0
                if 0x80 <= hi <= 0xE0:
                    if status & 0x80:           # we advanced past an explicit status
                        out[j - 1] = hi | GM_DRUM_CHANNEL
                    ndata = 1 if hi in (0xC0, 0xD0) else 2
                    j += ndata
                else:
                    j += 1
        i = end
    return bytes(out)


def fetch_dmp(out_drums: str, limit: int) -> int:
    print("[dmp] querying latest release …")
    try:
        import json
        meta = json.loads(_http_get(DMP_RELEASES_API).decode("utf-8"))
    except Exception as e:                     # noqa: BLE001
        print(f"[dmp] could not reach GitHub ({e}).")
        print("      Download the MIDI zip from https://github.com/gvellut/dmp_midi/releases")
        print(f"      and unzip its .mid files into: {out_drums}")
        return 0
    assets = meta.get("assets", [])
    zip_asset = next((a for a in assets if a.get("name", "").lower().endswith(".zip")), None)
    if not zip_asset:
        print("[dmp] no .zip asset on the latest release; see the repo releases page.")
        return 0
    print(f"[dmp] downloading {zip_asset['name']} …")
    blob = _http_get(zip_asset["browser_download_url"])
    zf = zipfile.ZipFile(io.BytesIO(blob))
    mids = [n for n in zf.namelist() if n.lower().endswith(".mid")]
    mids.sort()
    # Curate a spread: take an even stride through the (genre-ordered) list so we
    # get variety rather than the first N of one style.
    if limit and len(mids) > limit:
        stride = len(mids) / float(limit)
        picks = [mids[int(k * stride)] for k in range(limit)]
    else:
        picks = mids

    os.makedirs(out_drums, exist_ok=True)
    n = 0
    for name in picks:
        raw = zf.read(name)
        norm = _force_channel10(raw) or raw
        base = os.path.basename(name)
        with open(os.path.join(out_drums, base), "wb") as f:
            f.write(norm)
        n += 1
    # Ship the MIT license text alongside the pack.
    try:
        lic = _http_get(DMP_LICENSE_URL)
        with open(os.path.join(out_drums, "LICENSE.dmp_midi"), "wb") as f:
            f.write(lic)
    except Exception:                          # noqa: BLE001
        pass
    print(f"[dmp] staged {n} grooves (MIT) into {out_drums}")
    return n


# --- push (reuse the sample pusher) ------------------------------------------
def push(out_drums: str) -> None:
    # push_to_teensy.ps1 lands the source folder under the card root using its
    # TOP-LEVEL name — so we pass <out>/drums (leaf "drums") to land as /drums.
    ps1 = os.path.join(os.path.dirname(__file__), "push_to_teensy.ps1")
    if not os.path.exists(ps1):
        print(f"[push] {ps1} not found; copy {out_drums} to the card as /drums manually.")
        return
    print("[push] invoking push_to_teensy.ps1 …")
    subprocess.run(
        ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", ps1,
         "-SourcePath", out_drums],
        check=False,
    )


def main() -> int:
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    assets_default = os.path.join(
        repo_root, "projects", "spike_esp32_bt_spdif_mix_kit_f32", "assets", "drums")

    ap = argparse.ArgumentParser(description="Assemble MIDI drum grooves for T-DSP.")
    ap.add_argument("mode", nargs="?", default="sample", choices=["sample", "dmp", "all"])
    ap.add_argument("--out", default="c:/tmp/t-dsp-drums",
                    help="staging dir; grooves land in <out>/drums")
    ap.add_argument("--limit", type=int, default=48,
                    help="max grooves to curate from the dmp pack")
    ap.add_argument("--assets", action="store_true",
                    help="write the sample set into the repo assets/drums instead of --out")
    ap.add_argument("--push", action="store_true", help="copy the staging dir to the card")
    args = ap.parse_args()

    if args.assets:
        out_drums = assets_default
        out_root = os.path.dirname(assets_default)
    else:
        out_root = args.out
        out_drums = os.path.join(args.out, "drums")

    total = 0
    if args.mode in ("sample", "all"):
        print(f"[sample] generating original grooves -> {out_drums}")
        total += write_sample(out_drums)
        with open(os.path.join(out_drums, "CREDITS.md"), "w", encoding="utf-8") as f:
            f.write(SAMPLE_CREDITS)
    if args.mode in ("dmp", "all"):
        total += fetch_dmp(out_drums, args.limit)

    print(f"\nDone: {total} groove(s) in {out_drums}")
    if args.push and not args.assets:
        push(out_drums)
    elif not args.assets:
        print(f"Drag {out_drums} onto the SD card as /drums (or run with --push).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
