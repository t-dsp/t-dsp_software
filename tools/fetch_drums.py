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

gmd      (network)
    Fetch Google Magenta's Groove MIDI Dataset (1,150 human-performed grooves,
    CC-BY 4.0) from the magentadata bucket. These are expressive multi-bar
    performances already on channel 10; the fetcher forces channel 10 (idempotent),
    makes each file SEAMLESSLY LOOPABLE (appends a silent barline marker at the
    next bar boundary after the last note — same trick as the sample writer), and
    curates a round-robin spread across the ~18 playing styles (prefers `beat`
    takes over `fill`s and 4/4 over odd meters).

all
    sample + dmp + gmd.

Usage
-----
    python tools/fetch_drums.py sample
    python tools/fetch_drums.py sample --assets     # (re)write repo assets/drums
    python tools/fetch_drums.py dmp --limit 48
    python tools/fetch_drums.py gmd --limit 64
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
attribution are written next to the fetched files. The `gmd` pack is CC-BY 4.0
(Google LLC / Magenta); an ATTRIBUTION.gmd file is written next to the fetched
files. Keep those license/attribution files with the grooves if you redistribute
them.
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


# --- Groove manifest (/drums/catalog.tsv) ------------------------------------
# The mix-kit firmware browses grooves along two axes (Genre, Pack). Rather than
# have the firmware guess a groove's genre/pack from its filename, each fetch mode
# records what it KNOWS here; write_manifest() emits one authoritative TSV whose
# line order defines each groove's global index:
#     <filename>\t<pack>\t<genre>\t<bpm>\t<display>
_MANIFEST: dict[str, dict] = {}       # filename -> {pack, genre, bpm, display}


def _record(filename: str, pack: str, genre: str, bpm: str, display: str) -> None:
    _MANIFEST[filename] = {"pack": pack, "genre": genre or "misc",
                           "bpm": str(bpm or ""), "display": display}


def _genre_from_name(base: str) -> str:
    """Fuzzy genre for a stray/legacy .mid: the alpha word after any leading index.

    "01 Rock Straight" -> "rock", "Disco 2" -> "disco", "gmd funk 95bpm" -> "funk".
    """
    stem = base[:-4] if base.lower().endswith(".mid") else base
    toks = stem.replace("_", " ").split()
    if toks and toks[0].isdigit():          # leading "01 " index
        toks = toks[1:]
    if toks and toks[0].lower() == "gmd":    # our gmd prefix
        toks = toks[1:]
    for t in toks:
        w = "".join(c for c in t if c.isalpha() or c == "-")
        if w:
            return w.lower()
    return "misc"


def write_manifest(out_drums: str) -> None:
    """Write /drums/catalog.tsv for EVERY *.mid in out_drums (this run + prior modes).

    Classification precedence per file: this run's authoritative record > a row
    already in catalog.tsv (so running modes separately keeps prior packs' labels)
    > fuzzy (pack=misc). Lines are sorted by (pack, genre, display) so the
    global-index order is deterministic.
    """
    prior: dict[str, dict] = {}
    cat = os.path.join(out_drums, "catalog.tsv")
    if os.path.exists(cat):
        with open(cat, encoding="utf-8") as f:
            for ln in f:
                if ln.startswith("#") or "\t" not in ln:
                    continue
                c = ln.rstrip("\n").split("\t")
                if len(c) >= 5:
                    prior[c[0]] = {"pack": c[1], "genre": c[2], "bpm": c[3], "display": c[4]}

    rows = []
    for fn in sorted(os.listdir(out_drums)):
        if not fn.lower().endswith(".mid"):
            continue
        meta = _MANIFEST.get(fn) or prior.get(fn)
        if meta is None:
            meta = {"pack": "misc", "genre": _genre_from_name(fn),
                    "bpm": "", "display": fn[:-4]}
        rows.append((meta["pack"], meta["genre"], meta["display"], fn, meta["bpm"]))
    rows.sort(key=lambda r: (r[0], r[1], r[2].lower()))
    path = os.path.join(out_drums, "catalog.tsv")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# filename\tpack\tgenre\tbpm\tdisplay\n")
        for pack, genre, display, fn, bpm in rows:
            f.write(f"{fn}\t{pack}\t{genre}\t{bpm}\t{display}\n")
    print(f"[manifest] {len(rows)} grooves -> {path}")


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
        _record(name + ".mid", "samples", _genre_from_name(name), "", name)
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
        _record(base, "dmp", _genre_from_name(base), "", base[:-4])
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


# --- gmd pack (Groove MIDI Dataset, CC-BY 4.0) -------------------------------
GMD_ZIP_URL = ("https://storage.googleapis.com/magentadata/datasets/groove/"
               "groove-v1.0.0-midionly.zip")

GMD_ATTRIBUTION = """\
Groove MIDI Dataset (GMD)
Copyright 2019 Google LLC.
Licensed under the Creative Commons Attribution 4.0 International License
(CC BY 4.0): https://creativecommons.org/licenses/by/4.0/
Source: https://magenta.tensorflow.org/datasets/groove

The .mid grooves in this folder derived from GMD have been normalized to General
MIDI channel 10 and made seamlessly loopable (a silent barline marker appended at
the bar boundary after the last note). Attribution to Google LLC / Magenta is
required if you redistribute them.
"""


def _read_vlq(buf: bytes, i: int) -> tuple[int, int]:
    """Read a variable-length quantity at buf[i]; return (value, next_index)."""
    val = 0
    while True:
        b = buf[i]
        i += 1
        val = (val << 7) | (b & 0x7F)
        if not (b & 0x80):
            return val, i


def _parse_smf(mid: bytes):
    """Flatten any type-0/1 SMF into (division, (num, den), [(abs_tick, event)]).

    Every track is merged onto one absolute-tick timeline (GM playback ignores
    track boundaries). Running status is expanded so each returned event carries
    its own status byte. End-of-Track metas are dropped (re-added on render).
    Returns None on anything we don't confidently understand.
    """
    if mid[:4] != b"MThd" or len(mid) < 14:
        return None
    ntrks = struct.unpack(">H", mid[10:12])[0]
    division = struct.unpack(">H", mid[12:14])[0]
    if division & 0x8000:                      # SMPTE timing — not tick-based; skip
        return None
    timesig = (4, 4)
    events: list[tuple[int, bytes]] = []
    i = 14
    for _ in range(ntrks):
        if mid[i:i + 4] != b"MTrk":
            return None
        clen = struct.unpack(">I", mid[i + 4:i + 8])[0]
        j = i + 8
        end = j + clen
        if end > len(mid):
            return None
        abstick = 0
        running = 0
        while j < end:
            dt, j = _read_vlq(mid, j)
            abstick += dt
            if j >= end:
                break
            b = mid[j]
            if b == 0xFF:                      # meta event
                mtype = mid[j + 1]
                mlen, k = _read_vlq(mid, j + 2)
                if mtype == 0x58 and mlen >= 2:      # time signature
                    timesig = (mid[k], 1 << mid[k + 1])
                if mtype != 0x2F:                    # keep everything but EoT
                    events.append((abstick, mid[j:k + mlen]))
                j = k + mlen
                running = 0
            elif b in (0xF0, 0xF7):            # sysex
                slen, k = _read_vlq(mid, j + 1)
                events.append((abstick, mid[j:k + slen]))
                j = k + slen
                running = 0
            else:                              # channel-voice message
                if b & 0x80:
                    status = b
                    running = b
                    k = j + 1
                else:
                    status = running
                    k = j
                nd = 1 if (status & 0xF0) in (0xC0, 0xD0) else 2
                events.append((abstick, bytes([status]) + mid[k:k + nd]))
                j = k + nd
        i = end
    return division, timesig, events


def _loopify_gmd(mid: bytes, window_bars: int = 2) -> bytes | None:
    """Extract a short, seamlessly-loopable channel-10 groove from a GMD take.

    GMD takes are whole ~100-bar performances — far too long/heavy for the mix-kit
    (MAX_DRUM_EVENTS=4096, "a bar or two"). We slice a `window_bars`-bar window past
    any count-in, keep only the drum hits (channel-voice CC/PC on ch10 would fight the
    firmware's kit selection), re-key to channel 10, preserve the take's tempo, and
    append a silent barline marker so the slice loops on an exact bar boundary. Each
    hit gets a short synthesized note-off (drums are one-shots — original offs carry no
    musical info). window_bars <= 0 keeps the WHOLE take (may exceed the firmware cap).
    """
    parsed = _parse_smf(mid)
    if not parsed:
        return None
    division, (num, den), events = parsed
    bar = int(round(division * 4 * num / den))          # ticks per bar
    if bar <= 0 or not events:
        return None

    # GMD is click-locked (constant tempo): carry the take's first tempo meta.
    tempo = next((ev[3:6] for _, ev in events
                  if ev[0] == 0xFF and len(ev) >= 6 and ev[1] == 0x51), None)

    hits = [(t, ev[1] & 0x7F, ev[2] & 0x7F)             # (tick, note, vel)
            for t, ev in events
            if (ev[0] & 0xF0) == 0x90 and ev[2] > 0]
    if not hits:
        return None
    first_note, last_note = hits[0][0], hits[-1][0]
    total_bars = last_note // bar + 1

    if window_bars > 0 and window_bars < total_bars:
        # Start one bar past the first hit's bar to skip a count-in, clamped so the
        # window stays inside the take. `beat`-type takes are steady throughout, so
        # any interior window is representative.
        start_bar = first_note // bar + (1 if total_bars > window_bars + 1 else 0)
        start_bar = max(0, min(start_bar, total_bars - window_bars))
        w0 = start_bar * bar
        loop_end = window_bars * bar
        sel = [(t - w0, n, v) for (t, n, v) in hits if w0 <= t < w0 + loop_end]
    else:
        loop_end = total_bars * bar
        sel = [(t, n, v) for (t, n, v) in hits]
    if not sel:
        return None

    off = max(1, bar // 16)                              # short one-shot tail
    evlist: list[tuple[int, bytes]] = []
    for tick, note, vel in sel:
        evlist.append((tick, bytes([0x90 | GM_DRUM_CHANNEL, note, vel])))
        evlist.append((min(tick + off, loop_end), bytes([0x80 | GM_DRUM_CHANNEL, note, 0])))
    evlist.append((loop_end, bytes([0x80 | GM_DRUM_CHANNEL, 0, 0])))  # silent barline marker

    def rank(ev: bytes) -> int:
        return 0 if (ev[0] & 0xF0) == 0x80 else 1

    evlist.sort(key=lambda e: (e[0], rank(e[1])))
    body = bytearray()
    if tempo:
        body += _vlq(0) + bytes([0xFF, 0x51, 0x03]) + tempo
    prev = 0
    for tick, ev in evlist:
        body += _vlq(tick - prev) + ev
        prev = tick
    body += _vlq(0) + bytes([0xFF, 0x2F, 0x00])
    track = b"MTrk" + struct.pack(">I", len(body)) + bytes(body)
    header = b"MThd" + struct.pack(">IHHH", 6, 0, 1, division)
    return header + track


def _gmd_meta(path: str):
    """Parse a GMD filename `<id>_<style>_<bpm>_<type>_<timesig>.mid` -> dict."""
    base = os.path.basename(path)
    if not base.lower().endswith(".mid"):
        return None
    parts = base[:-4].split("_")
    if len(parts) < 5:
        return None
    tid, bpm, typ, timesig = parts[0], parts[-3], parts[-2], parts[-1]
    style = "-".join(parts[1:-3])
    if not bpm.isdigit():
        return None
    return {"id": tid, "style": style, "bpm": bpm, "type": typ, "timesig": timesig}


def fetch_gmd(out_drums: str, limit: int, window_bars: int = 2) -> int:
    print(f"[gmd] downloading Groove MIDI Dataset (midi-only, ~4 MB) …")
    try:
        blob = _http_get(GMD_ZIP_URL)
    except Exception as e:                     # noqa: BLE001
        print(f"[gmd] download failed ({e}).")
        print(f"      Get it manually from {GMD_ZIP_URL}")
        print(f"      and unzip the .mid files into: {out_drums}")
        return 0

    zf = zipfile.ZipFile(io.BytesIO(blob))
    cand = []
    for name in zf.namelist():
        meta = _gmd_meta(name)
        if meta:
            cand.append((name, meta))
    # Prefer full grooves in common time, then group by style for a round-robin
    # spread so a small --limit still samples the whole stylistic range.
    def pref(m: dict) -> tuple:
        return (0 if m["type"] == "beat" else 1,
                0 if m["timesig"] == "4-4" else 1)

    by_style: dict[str, list] = {}
    for name, meta in sorted(cand, key=lambda c: (pref(c[1]), c[1]["style"], c[0])):
        by_style.setdefault(meta["style"], []).append((name, meta))

    pools = list(by_style.values())
    picks: list[tuple] = []
    while pools and (not limit or len(picks) < limit):
        pools = [p for p in pools if p]
        for p in pools:
            picks.append(p.pop(0))
            if limit and len(picks) >= limit:
                break

    os.makedirs(out_drums, exist_ok=True)
    seen: set[str] = set()
    n = 0
    for name, meta in picks:
        loop = _loopify_gmd(zf.read(name), window_bars)
        if not loop:
            continue
        tag = "" if meta["type"] == "beat" else f" {meta['type']}"
        base = f"gmd {meta['style']} {meta['bpm']}bpm{tag}"
        out_name = base
        if out_name in seen:                   # disambiguate same style+bpm
            out_name = f"{base} ({meta['id']})"
        seen.add(out_name)
        with open(os.path.join(out_drums, out_name + ".mid"), "wb") as f:
            f.write(loop)
        # Collapse the granular GMD style to its family for the Genre axis
        # ("funk-groove1" -> "funk", "afrocuban-bembe" -> "afrocuban"); the full
        # style stays in the display name.
        _record(out_name + ".mid", "gmd", meta["style"].split("-")[0], meta["bpm"], out_name)
        n += 1
    with open(os.path.join(out_drums, "ATTRIBUTION.gmd"), "w", encoding="utf-8") as f:
        f.write(GMD_ATTRIBUTION)
    print(f"[gmd] staged {n} loopable grooves (CC-BY 4.0) into {out_drums}")
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
    ap.add_argument("mode", nargs="?", default="sample",
                    choices=["sample", "dmp", "gmd", "all"])
    ap.add_argument("--out", default="c:/tmp/t-dsp-drums",
                    help="staging dir; grooves land in <out>/drums")
    ap.add_argument("--limit", type=int, default=48,
                    help="max grooves to curate from the dmp/gmd packs (0 = no cap)")
    ap.add_argument("--bars", type=int, default=2,
                    help="gmd: loop-window length in bars (0 = keep the whole take)")
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
    if args.mode in ("gmd", "all"):
        total += fetch_gmd(out_drums, args.limit, args.bars)

    if total or os.path.isdir(out_drums):
        write_manifest(out_drums)          # /drums/catalog.tsv drives the firmware browser
    print(f"\nDone: {total} groove(s) in {out_drums}")
    if args.push and not args.assets:
        push(out_drums)
    elif not args.assets:
        print(f"Drag {out_drums} onto the SD card as /drums (or run with --push).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
