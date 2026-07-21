#!/usr/bin/env python3
"""fetch_loops.py — assemble short, LOOPABLE melody & bass MIDI loops for T-DSP.

Sibling to fetch_drums.py (channel-10 grooves) and fetch_songs.py (full-song
arrangements). Where a *groove* is a drum loop and a *song* is a whole
arrangement, a *loop* here is a SHORT, single-part, seamlessly-loopable melodic
or bass phrase — the kind of thing you drop under a live synth to jam over. The
firmware plays these through the song player (a loop IS just a tiny .mid), so
they land on the card and loop with @LOOP=1.

The whole point is melody + bass. Most MIDI in the wild is a full arrangement,
so the engine of this tool is extract_role_loops(): given ANY Standard MIDI
File it pulls out a MELODY part and a BASS part (by GM program family + pitch
register + monophony), slices an N-bar window past any count-in, and re-emits
each as a one-track loop with a silent barline marker so it loops seamlessly
(the same trick fetch_drums.py uses for grooves). Sources whose parts are
already separated (NES-MDB pulse/triangle, JSB SATB voices, POP909 MELODY
track) get an exact role map; everything else uses the generic classifier.

Output layout — a SUBFOLDER PER SOURCE so provenance is obvious, plus one
catalog.tsv carrying the genre/role axes so a track can browse by them:

    <out>/loops/<source>/<name> [melody].mid
    <out>/loops/<source>/<name> [bass].mid
    <out>/loops/catalog.tsv        # relpath  source  genre  role  bpm  display

Modes
-----
BUNDLE-SAFE (permissively licensed — fine to commit to assets/ and redistribute):
  cc0      m-malandro/CC0-midis (CC0). Full arrangements -> melody+bass loops.
  nesmdb   NES Music Database (MIT). Chiptune; P1/P2 = melody, triangle = bass.
           Hosted on Google Drive (confirm-token download handled below).
  jsb      JSB Chorales (public domain, Bach). 4 SATB voices -> soprano+bass.
  pop909   POP909 (research). Explicit MELODY track + accompaniment bass.

PERSONAL-USE-ONLY (copyrighted transcriptions — fetched to YOUR machine, never
committed; these mirror the fetch_songs.py "personal/demo use" stance):
  bitmidi  BitMidi search API. Default = a spread across genre-word queries into
           bitmidi/<genre>/ subfolders. --query "<term>" fetches a single bucket
           by artist/name instead. (FreeMIDI is NOT here: its pages are
           JavaScript-rendered, so there's nothing to scrape over plain HTTP.)
  local    Mine a folder of .mid files you already have into melody/bass loops
           (--local <dir>). This is the way to turn e.g. Phish transcriptions you
           downloaded yourself into loops — nothing is fetched or committed.

  all      the BUNDLE-SAFE set (cc0 + nesmdb + jsb + pop909). The personal-use
           modes are opt-in by name so nothing copyrighted is fetched by accident.

Usage
-----
    python tools/fetch_loops.py cc0 --limit 40
    python tools/fetch_loops.py nesmdb --limit 60
    python tools/fetch_loops.py bitmidi --genres rock,jazz,funk --limit 60
    python tools/fetch_loops.py bitmidi --query "beatles" --limit 20
    python tools/fetch_loops.py local --local c:/my-phish-midis --genre-tag jam
    python tools/fetch_loops.py all --push        # stage bundle-safe + copy to card

By default the script stages to c:/tmp/t-dsp-loops and prints how to drag
<out>/loops onto the SD card (lands as /midi/loops). --assets writes the
bundle-safe sources into firmware/mix-kit/assets/midi/loops instead. --push
invokes tools/push_to_teensy.ps1 like the sibling fetchers.

Licensing
---------
Each source writes its own LICENSE/ATTRIBUTION file into its subfolder. Keep it
if you redistribute. The personal-use modes (freemidi/bitmidi) fetch copyrighted
fan transcriptions — legal for personal noodling on your own device, but do NOT
commit them or `--assets` them into the repo. `all` never touches them.
"""

from __future__ import annotations

import argparse
import io
import json
import os
import re
import struct
import subprocess
import sys
import tarfile
import tempfile
import urllib.parse
import urllib.request
import zipfile

PPQN_OUT = 480                       # ticks/quarter in emitted loops (division kept from source, actually)
NAME_MAX = 40                        # keep display stems tidy (song player r.name[48])

# GM program families used to guess a part's musical role (0-based program #).
BASS_PROGRAMS = set(range(32, 40))                  # 32-39 acoustic..synth bass
LEAD_PROGRAMS = set(range(80, 88)) | set(range(56, 62)) | set(range(72, 80))  # synth lead, brass, pipe
DRUM_CH = 9                                          # 0-based ch 10 = GM drums (skip for melodic loops)


# --- Catalog (loops/catalog.tsv) --------------------------------------------
# One authoritative TSV across every source/run. Columns let a firmware/app
# browser pick loops along the axes the user cares about (genre, role, source):
#     <relpath>\t<source>\t<genre>\t<role>\t<bpm>\t<display>
# relpath is "<source>/<file>.mid" so it's unique and says where it came from.
_CATALOG: dict[str, dict] = {}


def _record(relpath: str, source: str, genre: str, role: str, bpm, display: str) -> None:
    _CATALOG[relpath] = {"source": source, "genre": genre or "misc",
                         "role": role, "bpm": str(bpm or ""), "display": display}


def write_catalog(out_loops: str) -> None:
    prior: dict[str, dict] = {}
    cat = os.path.join(out_loops, "catalog.tsv")
    if os.path.exists(cat):
        with open(cat, encoding="utf-8") as f:
            for ln in f:
                if ln.startswith("#") or "\t" not in ln:
                    continue
                c = ln.rstrip("\n").split("\t")
                if len(c) >= 6:
                    prior[c[0]] = {"source": c[1], "genre": c[2], "role": c[3],
                                   "bpm": c[4], "display": c[5]}
    # Re-scan the tree so stray files (or a prior run's) all end up indexed.
    rows = []
    for root, _dirs, names in os.walk(out_loops):
        for fn in sorted(names):
            if not fn.lower().endswith(".mid"):
                continue
            rel = os.path.relpath(os.path.join(root, fn), out_loops).replace(os.sep, "/")
            meta = _CATALOG.get(rel) or prior.get(rel)
            if meta is None:
                source = rel.split("/")[0] if "/" in rel else "misc"
                meta = {"source": source, "genre": "misc",
                        "role": _role_from_name(fn), "bpm": "", "display": fn[:-4]}
            rows.append((meta["source"], meta["genre"], meta["role"], meta["display"], rel, meta["bpm"]))
    rows.sort(key=lambda r: (r[0], r[1], r[2], r[3].lower()))
    with open(cat, "w", encoding="utf-8", newline="\n") as f:
        f.write("# relpath\tsource\tgenre\trole\tbpm\tdisplay\n")
        for source, genre, role, display, rel, bpm in rows:
            f.write(f"{rel}\t{source}\t{genre}\t{role}\t{bpm}\t{display}\n")
    print(f"[catalog] {len(rows)} loops -> {cat}")


def _role_from_name(base: str) -> str:
    b = base.lower()
    if "[bass]" in b or " bass" in b:
        return "bass"
    if "[melody]" in b or "[lead]" in b:
        return "melody"
    return "full"


# --- Minimal SMF read/write (ported from fetch_drums.py, proven) -------------
def _vlq(n: int) -> bytes:
    out = bytearray([n & 0x7F])
    n >>= 7
    while n:
        out.insert(0, (n & 0x7F) | 0x80)
        n >>= 7
    return bytes(out)


def _read_vlq(buf: bytes, i: int, end: int):
    val = 0
    for _ in range(4):
        if i >= end:
            return val, i
        b = buf[i]
        i += 1
        val = (val << 7) | (b & 0x7F)
        if not (b & 0x80):
            break
    return val, i


class Note:
    __slots__ = ("tick", "note", "vel", "dur")

    def __init__(self, tick, note, vel, dur):
        self.tick, self.note, self.vel, self.dur = tick, note, vel, dur


class Part:
    """One melodic part = a (track, channel) stream of notes + its GM program."""
    __slots__ = ("track", "channel", "program", "notes")

    def __init__(self, track, channel):
        self.track, self.channel, self.program, self.notes = track, channel, 0, []

    # --- cheap musical descriptors used to guess the part's role ---
    def median_pitch(self) -> float:
        if not self.notes:
            return 0.0
        ps = sorted(n.note for n in self.notes)
        return ps[len(ps) // 2]

    def mono_score(self) -> float:
        """Fraction of onsets that are a SINGLE note (1.0 = fully monophonic)."""
        if not self.notes:
            return 0.0
        by_tick: dict[int, int] = {}
        for n in self.notes:
            by_tick[n.tick] = by_tick.get(n.tick, 0) + 1
        singles = sum(1 for c in by_tick.values() if c == 1)
        return singles / len(by_tick)


def parse_parts(mid: bytes):
    """Parse an SMF into (division, (num,den), tempo_bytes|None, [Part]).

    Notes are on/off paired per (track, channel, note). Channel-10 drums are
    dropped (these are melodic loops). Returns None on anything unparseable.
    """
    if mid[:4] != b"MThd" or len(mid) < 14:
        return None
    ntrks = struct.unpack(">H", mid[10:12])[0]
    division = struct.unpack(">H", mid[12:14])[0]
    if division == 0 or (division & 0x8000):          # SMPTE / zero division: skip
        return None
    timesig = (4, 4)
    tempo = None
    parts: dict[tuple, Part] = {}
    i = 14
    for tk in range(ntrks):
        if i + 8 > len(mid) or mid[i:i + 4] != b"MTrk":
            break
        clen = struct.unpack(">I", mid[i + 4:i + 8])[0]
        j = i + 8
        end = min(j + clen, len(mid))
        abstick = 0
        running = 0
        program: dict[int, int] = {}                  # channel -> current program
        pending: dict[tuple, tuple] = {}              # (ch,note) -> (ontick, vel)
        while j < end:
            dt, j = _read_vlq(mid, j, end)
            abstick += dt
            if j >= end:
                break
            b = mid[j]
            if b == 0xFF:                             # meta
                if j + 1 >= end:
                    break
                mtype = mid[j + 1]
                mlen, k = _read_vlq(mid, j + 2, end)
                if mtype == 0x58 and mlen >= 2 and k + 1 < end:
                    timesig = (mid[k], 1 << mid[k + 1])
                if mtype == 0x51 and mlen == 3 and tempo is None and k + 3 <= end:
                    tempo = bytes(mid[k:k + 3])
                j = k + mlen
                running = 0
                continue
            if b in (0xF0, 0xF7):                     # sysex
                slen, k = _read_vlq(mid, j + 1, end)
                j = k + slen
                running = 0
                continue
            if b & 0x80:
                status = b
                running = b
                j += 1
            else:
                status = running
            hi = status & 0xF0
            ch = status & 0x0F
            if hi in (0xC0, 0xD0):                    # program change / channel pressure (1 data byte)
                d0 = mid[j] if j < end else 0
                if hi == 0xC0:
                    program[ch] = d0
                j += 1
                continue
            d0 = mid[j] if j < end else 0
            d1 = mid[j + 1] if j + 1 < end else 0
            j += 2
            if ch == DRUM_CH:                         # drums are not melodic loop material
                continue
            key = (tk, ch)
            if hi == 0x90 and d1 > 0:                 # note-on
                pending[(ch, d0)] = (abstick, d1)
                parts.setdefault(key, Part(tk, ch))
            elif hi == 0x80 or (hi == 0x90 and d1 == 0):   # note-off
                st = pending.pop((ch, d0), None)
                if st is not None:
                    ontick, vel = st
                    p = parts.setdefault(key, Part(tk, ch))
                    p.notes.append(Note(ontick, d0, vel, max(1, abstick - ontick)))
        # flush parts' programs for this track
        for (t, c), p in parts.items():
            if t == tk and c in program:
                p.program = program[c]
        i = end
    plist = [p for p in parts.values() if p.notes]
    if not plist:
        return None
    return division, timesig, tempo, plist


def _emit_loop(notes: list, division: int, timesig, tempo, bars: int, out_channel: int = 0) -> bytes | None:
    """Emit a single-track type-0 SMF of `notes`, sliced to `bars` and seamlessly
    loopable. Skips a leading bar (count-in) when the part is long enough, preserves
    the source tempo, and appends a silent barline marker so the loop period is
    an exact bar count (same seam fix as fetch_drums.py)."""
    if not notes:
        return None
    num, den = timesig
    bar = int(round(division * 4 * num / den))
    if bar <= 0:
        return None
    notes = sorted(notes, key=lambda n: n.tick)
    first = notes[0].tick
    last = notes[-1].tick
    total_bars = (last - first) // bar + 1
    if total_bars <= 0:
        return None
    win = min(bars, total_bars)
    # start one bar in to skip a pickup when there's room, then clamp into range
    start_bar = 1 if total_bars > win + 1 else 0
    w0 = first + start_bar * bar
    loop_end = win * bar
    sel = [(n.tick - w0, n.note, n.vel, min(n.dur, loop_end)) for n in notes
           if w0 <= n.tick < w0 + loop_end]
    if len(sel) < 3:                 # skip near-empty windows (silence / 1-2 stray notes)
        return None

    evlist: list[tuple[int, bytes]] = []
    for tick, note, vel, dur in sel:
        end_t = min(tick + dur, loop_end)
        evlist.append((tick, bytes([0x90 | out_channel, note & 0x7F, vel & 0x7F])))
        evlist.append((end_t, bytes([0x80 | out_channel, note & 0x7F, 0])))
    evlist.append((loop_end, bytes([0x80 | out_channel, 0, 0])))   # silent barline marker

    def rank(ev: bytes) -> int:
        return 0 if (ev[0] & 0xF0) == 0x80 else 1

    evlist.sort(key=lambda e: (e[0], rank(e[1])))
    body = bytearray()
    if tempo:
        body += _vlq(0) + bytes([0xFF, 0x51, 0x03]) + tempo
    body += _vlq(0) + bytes([0xFF, 0x58, 0x04, num & 0x7F, (den.bit_length() - 1) & 0x7F, 24, 8])
    prev = 0
    for tick, ev in evlist:
        body += _vlq(tick - prev) + ev
        prev = tick
    body += _vlq(0) + bytes([0xFF, 0x2F, 0x00])
    track = b"MTrk" + struct.pack(">I", len(body)) + bytes(body)
    header = b"MThd" + struct.pack(">IHHH", 6, 0, 1, division)
    return header + track


def _tempo_bpm(tempo: bytes | None) -> str:
    if not tempo or len(tempo) != 3:
        return ""
    usec = (tempo[0] << 16) | (tempo[1] << 8) | tempo[2]
    return str(round(60_000_000 / usec)) if usec else ""


def classify_roles(parts: list) -> dict:
    """Pick the best MELODY and BASS part from a parsed arrangement.

    bass  = lowest-register part, preferring a GM bass program (32-39).
    melody= highest-register, mostly-monophonic part, preferring lead families.
    Returns {"melody": Part|None, "bass": Part|None} (may reuse none/one).
    """
    cand = [p for p in parts if len(p.notes) >= 8]
    if not cand:
        return {"melody": None, "bass": None}

    def bass_score(p: Part) -> float:
        s = 0.0
        if p.program in BASS_PROGRAMS:
            s += 100
        s += (127 - p.median_pitch())               # lower = better
        return s

    def melody_score(p: Part) -> float:
        s = 0.0
        if p.program in LEAD_PROGRAMS:
            s += 60
        s += p.mono_score() * 50                     # prefer single-line parts
        s += p.median_pitch() * 0.5                  # prefer higher register
        return s

    bass = max(cand, key=bass_score)
    mel_cand = [p for p in cand if p is not bass] or cand
    melody = max(mel_cand, key=melody_score)
    # A part that's clearly low-register shouldn't be tagged melody, and vice-versa.
    if melody.median_pitch() < 48:
        melody = None
    if bass.median_pitch() > 60 and bass.program not in BASS_PROGRAMS:
        bass = None
    return {"melody": melody, "bass": bass}


# --- Output helper -----------------------------------------------------------
_BAD = re.compile(r"[^A-Za-z0-9 ()\-,'&.]+")


def _clean(name: str) -> str:
    name = _BAD.sub(" ", name)
    name = re.sub(r"\s+", " ", name).strip(" .-")
    return name[:NAME_MAX].strip()


def _write_role_loops(out_root: str, subdir: str, source: str, genre: str, disp: str,
                      mid: bytes, bars: int, seen: set, roles_override: dict | None = None) -> int:
    """Parse an arrangement, extract melody+bass (or use an explicit role map),
    and write each as a seamless loop under <out_root>/<subdir>. `source` is the
    provenance label recorded in the catalog; `subdir` is where the files land
    (e.g. "cc0" or "bitmidi/jazz"). Returns how many loops were written."""
    parsed = parse_parts(mid)
    if not parsed:
        return 0
    division, timesig, tempo, parts = parsed
    bpm = _tempo_bpm(tempo)
    roles = roles_override if roles_override is not None else classify_roles(parts)
    disp = _clean(re.sub(r"\.midi?$", "", disp, flags=re.I))    # source name often ends ".mid"
    if not disp:
        return 0
    tgt = os.path.join(out_root, subdir)
    os.makedirs(tgt, exist_ok=True)
    n = 0
    for role in ("melody", "bass"):
        part = roles.get(role)
        if not part or len(part.notes) < 8:
            continue
        loop = _emit_loop(part.notes, division, timesig, tempo, bars)
        if not loop:
            continue
        stem = f"{disp} [{role}]"
        rel = f"{subdir}/{stem}.mid"
        if rel.lower() in seen:
            continue
        seen.add(rel.lower())
        with open(os.path.join(tgt, stem + ".mid"), "wb") as f:
            f.write(loop)
        _record(rel, source, genre, role, bpm, stem)
        print(f"  + {rel} ({len(loop)} B, {bpm or '?'} bpm)")
        n += 1
    return n


def _round_robin(groups: list, limit: int) -> list:
    picks: list = []
    pools = [list(g) for g in groups if g]
    while pools and (not limit or len(picks) < limit):
        pools = [p for p in pools if p]
        for p in list(pools):
            picks.append(p.pop(0))
            if limit and len(picks) >= limit:
                break
    return picks


# --- Networking --------------------------------------------------------------
UA = {"User-Agent": "Mozilla/5.0 (t-dsp-fetch-loops)"}


def _http_get(url: str, timeout: int = 90) -> bytes:
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


# ============================================================================
# BUNDLE-SAFE SOURCES
# ============================================================================

# --- cc0: m-malandro/CC0-midis (CC0) ----------------------------------------
CC0_ZIP = "https://codeload.github.com/m-malandro/CC0-midis/zip/refs/heads/main"
CC0_LICENSE = """\
CC0-midis by m-malandro — dedicated to the public domain under CC0 1.0
(https://creativecommons.org/publicdomain/zero/1.0/).
Source: https://github.com/m-malandro/CC0-midis
The .mid loops in this folder were sliced from those arrangements. No rights
reserved; attribution appreciated but not required.
"""


def fetch_cc0(out_loops: str, limit: int, bars: int, seen: set) -> int:
    d = os.path.join(out_loops, "cc0")
    os.makedirs(d, exist_ok=True)
    print("[cc0] downloading CC0-midis …")
    try:
        blob = _http_get(CC0_ZIP)
    except Exception as e:                            # noqa: BLE001
        print(f"[cc0] download failed ({e}). Get it from github.com/m-malandro/CC0-midis")
        return 0
    zf = zipfile.ZipFile(io.BytesIO(blob))
    mids = sorted(n for n in zf.namelist() if n.lower().endswith((".mid", ".midi")))
    n = 0
    for nm in mids:
        if limit and n >= limit:
            break
        base = os.path.splitext(os.path.basename(nm))[0]
        n += _write_role_loops(out_loops, "cc0", "cc0", "misc", base, zf.read(nm), bars, seen)
    with open(os.path.join(d, "LICENSE.cc0"), "w", encoding="utf-8") as f:
        f.write(CC0_LICENSE)
    print(f"[cc0] {n} loops (CC0) -> {d}")
    return n


# --- nesmdb: NES Music Database (MIT), chiptune melody+bass ------------------
# All artifacts live on Google Drive; the MIDI-format tarball is file id below.
NESMDB_DRIVE_ID = "1w2uo1Cmio4gz6nGUhZOtzF54kPkoKyo7"
NESMDB_LICENSE = """\
NES Music Database (NES-MDB) — MIT License.
Source: https://github.com/chrisdonahue/nesmdb
Each NES tune has four monophonic voices: P1/P2 (pulse) carry melody/harmony and
the triangle (TR) carries the bassline; noise (percussion) is dropped. The loops
here take P1 as melody and TR as bass. MIT-licensed dataset packaging.
"""


def _gdrive_download(file_id: str, timeout: int = 180) -> bytes | None:
    """Download a Google-Drive file, clearing the large-file confirm interstitial
    (no gdown dependency). Returns the bytes, or None on failure."""
    base = "https://drive.google.com/uc?export=download"
    cj = urllib.request.HTTPCookieProcessor()
    opener = urllib.request.build_opener(cj)
    opener.addheaders = list(UA.items())
    try:
        r = opener.open(f"{base}&id={file_id}", timeout=timeout)
        data = r.read()
    except Exception as e:                            # noqa: BLE001
        print(f"[nesmdb] Google Drive open failed ({e}).")
        return None
    # If Drive returned the HTML "can't scan for viruses" page, find the confirm token.
    head = data[:4096].lstrip()
    if head[:1] in (b"<", b"\xef") and b"html" in data[:512].lower():
        m = re.search(rb'confirm=([0-9A-Za-z_\-]+)', data)
        token = m.group(1).decode() if m else "t"
        try:
            r = opener.open(f"{base}&confirm={token}&id={file_id}", timeout=timeout)
            data = r.read()
        except Exception as e:                        # noqa: BLE001
            print(f"[nesmdb] confirm-token fetch failed ({e}).")
            return None
    return data


def fetch_nesmdb(out_loops: str, limit: int, bars: int, seen: set) -> int:
    d = os.path.join(out_loops, "nesmdb")
    os.makedirs(d, exist_ok=True)
    print("[nesmdb] downloading NES-MDB MIDI tarball from Google Drive (~12 MB) …")
    blob = _gdrive_download(NESMDB_DRIVE_ID)
    if not blob:
        print("      Manual: download the MIDI artifact from")
        print("      https://github.com/chrisdonahue/nesmdb#download-links")
        print(f"      and unpack the .mid files, then re-run with --local <dir>.")
        return 0
    try:
        tf = tarfile.open(fileobj=io.BytesIO(blob), mode="r:*")
    except Exception as e:                            # noqa: BLE001
        print(f"[nesmdb] not a readable tar ({e}); the Drive download may have changed.")
        return 0
    members = sorted((m for m in tf.getmembers()
                      if m.isfile() and m.name.lower().endswith((".mid", ".midi"))),
                     key=lambda m: m.name)
    n = 0
    for m in members:
        if limit and n >= limit:
            break
        try:
            mid = tf.extractfile(m).read()
        except Exception:                             # noqa: BLE001
            continue
        parsed = parse_parts(mid)
        if not parsed:
            continue
        _div, _ts, _tempo, parts = parsed
        # NES voices are separate tracks in order P1, P2, TR, (NO). Map by track.
        by_track = sorted(parts, key=lambda p: (p.track, p.channel))
        roles = {"melody": by_track[0] if by_track else None,
                 "bass": next((p for p in by_track if p.median_pitch() ==
                               min(q.median_pitch() for q in by_track)), None)}
        base = os.path.splitext(os.path.basename(m.name))[0]
        n += _write_role_loops(out_loops, "nesmdb", "nesmdb", "chiptune", base, mid, bars, seen, roles)
    with open(os.path.join(d, "LICENSE.nesmdb"), "w", encoding="utf-8") as f:
        f.write(NESMDB_LICENSE)
    print(f"[nesmdb] {n} loops (MIT) -> {d}")
    return n


# --- jsb: JSB Chorales (public domain) --------------------------------------
JSB_ZIP = "https://codeload.github.com/czhuang/JSB-Chorales-dataset/zip/refs/heads/master"
JSB_LICENSE = """\
JSB Chorales — J.S. Bach's four-part chorales (public domain composition).
Source: https://github.com/czhuang/JSB-Chorales-dataset (MIT code).
Each chorale has four voices SATB; these loops take Soprano as melody and Bass
as bass. The Bach music is public domain worldwide.
"""


def fetch_jsb(out_loops: str, limit: int, bars: int, seen: set) -> int:
    d = os.path.join(out_loops, "jsb")
    os.makedirs(d, exist_ok=True)
    print("[jsb] downloading JSB-Chorales-dataset …")
    try:
        blob = _http_get(JSB_ZIP)
    except Exception as e:                            # noqa: BLE001
        print(f"[jsb] download failed ({e}).")
        return 0
    zf = zipfile.ZipFile(io.BytesIO(blob))
    mids = sorted(n for n in zf.namelist() if n.lower().endswith((".mid", ".midi")))
    if not mids:
        print("[jsb] no .mid files in the archive (it may ship .npz only); skipping.")
        return 0
    n = 0
    for nm in mids:
        if limit and n >= limit:
            break
        parsed = parse_parts(zf.read(nm))
        if not parsed:
            continue
        _div, _ts, _tempo, parts = parsed
        ordered = sorted(parts, key=lambda p: -p.median_pitch())   # SATB by register
        roles = {"melody": ordered[0] if ordered else None,
                 "bass": ordered[-1] if ordered else None}
        base = os.path.splitext(os.path.basename(nm))[0]
        n += _write_role_loops(out_loops, "jsb", "jsb", "chorale", f"Bach {base}", zf.read(nm), bars, seen, roles)
    with open(os.path.join(d, "LICENSE.jsb"), "w", encoding="utf-8") as f:
        f.write(JSB_LICENSE)
    print(f"[jsb] {n} loops (public domain) -> {d}")
    return n


# --- pop909: POP909 (research) ----------------------------------------------
POP909_ZIP = "https://codeload.github.com/music-x-lab/POP909-Dataset/zip/refs/heads/master"
POP909_LICENSE = """\
POP909 Dataset — 909 pop songs, each with separated MELODY / BRIDGE / PIANO
tracks. Source: https://github.com/music-x-lab/POP909-Dataset
Provided for academic/research use (see the repo terms). The loops here take the
MELODY track as melody and the lowest PIANO register as bass. Personal/demo use.
"""


def fetch_pop909(out_loops: str, limit: int, bars: int, seen: set) -> int:
    d = os.path.join(out_loops, "pop909")
    os.makedirs(d, exist_ok=True)
    print("[pop909] downloading POP909-Dataset (~large) …")
    try:
        blob = _http_get(POP909_ZIP, timeout=180)
    except Exception as e:                            # noqa: BLE001
        print(f"[pop909] download failed ({e}).")
        return 0
    zf = zipfile.ZipFile(io.BytesIO(blob))
    mids = sorted(n for n in zf.namelist()
                  if n.lower().endswith(".mid") and "/POP909/" in n.replace("\\", "/"))
    n = 0
    for nm in mids:
        if limit and n >= limit:
            break
        base = os.path.splitext(os.path.basename(nm))[0]
        # POP909 files are named by a numeric id; generic classify picks melody/bass.
        n += _write_role_loops(out_loops, "pop909", "pop909", "pop", f"POP909 {base}", zf.read(nm), bars, seen)
    with open(os.path.join(d, "LICENSE.pop909"), "w", encoding="utf-8") as f:
        f.write(POP909_LICENSE)
    print(f"[pop909] {n} loops (research) -> {d}")
    return n


# ============================================================================
# PERSONAL-USE-ONLY SOURCES (copyrighted transcriptions — never bundle)
# ============================================================================
PERSONAL_WARN = """\
NOTE: {src} produces loops from copyrighted transcriptions. Legal to load onto
YOUR OWN device to play with, but do NOT commit them or run --assets with this
mode. The `all` mode never touches these.
"""

# --- bitmidi: search API (genre spread + free-text query) -------------------
# BitMidi has no genre facet, but a genre WORD as the query is a decent bucket.
# These terms all return hits; the spread lands in bitmidi/<genre>/ subfolders.
BITMIDI_GENRES = ["rock", "jazz", "blues", "funk", "reggae", "country",
                  "disco", "metal", "soul", "latin", "techno", "pop"]


def _bitmidi_mine(out_loops: str, term: str, subdir: str, genre: str,
                  want: int, bars: int, seen: set) -> int:
    """Fetch up to `want` melody/bass loop-pairs for a BitMidi search term."""
    got = 0
    page = 0
    while got < want and page < 12:
        api = f"https://bitmidi.com/api/midi/search?q={urllib.parse.quote(term)}&page={page}"
        try:
            js = json.loads(_http_get(api, timeout=60).decode("utf-8"))
        except Exception as e:                        # noqa: BLE001
            print(f"[bitmidi] search '{term}' failed ({e}).")
            break
        res = js.get("result", js)
        items = res.get("results") or res.get("midis") or []
        if not items:
            break
        for it in items:
            if got >= want:
                break
            durl = it.get("downloadUrl") or it.get("download_url") or ""
            if durl.startswith("/"):
                durl = "https://bitmidi.com" + durl
            if not durl:
                continue
            try:
                mid = _http_get(durl, timeout=60)
            except Exception:                         # noqa: BLE001
                continue
            if mid[:4] != b"MThd":
                continue
            disp = it.get("name") or f"{genre} {got}"
            # one search hit yields a melody+bass pair; count a hit that produced ANY loop
            if _write_role_loops(out_loops, subdir, "bitmidi", genre, disp, mid, bars, seen):
                got += 1
        page += 1
    return got


def fetch_bitmidi(out_loops: str, query: str | None, genres: list | None,
                  limit: int, bars: int, seen: set) -> int:
    print(PERSONAL_WARN.format(src="bitmidi"))
    total = 0
    if query:                                         # single free-text bucket (e.g. an artist)
        safe = re.sub(r"[^a-z0-9]+", "_", query.lower()).strip("_") or "query"
        n = _bitmidi_mine(out_loops, query, f"bitmidi/{safe}", safe, limit or 20, bars, seen)
        print(f"[bitmidi] '{query}': {n} source song(s) mined -> bitmidi/{safe}")
        total += n
    else:                                             # genre spread
        gl = genres or BITMIDI_GENRES
        per = max(1, (limit // len(gl)) if limit else 6)
        for g in gl:
            n = _bitmidi_mine(out_loops, g, f"bitmidi/{g}", g, per, bars, seen)
            print(f"[bitmidi] {g}: {n} source song(s) mined")
            total += n
    print(f"[bitmidi] {total} source song(s) -> melody/bass loops (PERSONAL USE)")
    return total


# --- local: ingest a folder of .mid you already have (the real Phish path) --
def fetch_local(out_loops: str, src_dir: str, genre: str, limit: int, bars: int, seen: set) -> int:
    print(PERSONAL_WARN.format(src="local"))
    if not src_dir or not os.path.isdir(src_dir):
        print(f"[local] --local dir not found: {src_dir!r}. Point it at a folder of .mid files.")
        return 0
    files = []
    for root, _dirs, names in os.walk(src_dir):
        for fn in sorted(names):
            if fn.lower().endswith((".mid", ".midi")):
                files.append(os.path.join(root, fn))
    files.sort()
    n = 0
    for path in files:
        if limit and n >= limit:
            break
        try:
            with open(path, "rb") as f:
                mid = f.read()
        except Exception:                             # noqa: BLE001
            continue
        base = os.path.splitext(os.path.basename(path))[0]
        n += _write_role_loops(out_loops, "local", "local", genre, base, mid, bars, seen)
    print(f"[local] {n} loops from {src_dir} (PERSONAL USE) -> {os.path.join(out_loops, 'local')}")
    return n


# --- push --------------------------------------------------------------------
def push(out_loops: str) -> None:
    # Push via sync_assets.py --loops-src: the canonical @WB serial path that lands the
    # tree (and catalog.tsv) at /midi/loops with CRC verify — the SD path the firmware
    # loops manifest + @LOOPF expect (push_to_teensy's top-level-folder rule would land
    # a bare /loops, which the firmware does not scan).
    sync = os.path.join(os.path.dirname(__file__), "sync_assets.py")
    if not os.path.exists(sync):
        print(f"[push] {sync} not found; copy {out_loops}/* onto the card as /midi/loops manually.")
        return
    print("[push] invoking sync_assets.py --loops-src (serial @WB -> /midi/loops) …")
    subprocess.run([sys.executable, sync, "--loops-src", out_loops], check=False)


BUNDLE_SAFE = ("cc0", "nesmdb", "jsb", "pop909")


def main() -> int:
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    assets_default = os.path.join(repo_root, "firmware", "mix-kit", "assets", "midi", "loops")

    ap = argparse.ArgumentParser(description="Assemble melody & bass MIDI loops for T-DSP.")
    ap.add_argument("mode", nargs="?", default="cc0",
                    choices=["cc0", "nesmdb", "jsb", "pop909", "bitmidi", "local", "all"])
    ap.add_argument("--out", default="c:/tmp/t-dsp-loops", help="staging dir; loops land in <out>/loops")
    ap.add_argument("--limit", type=int, default=40, help="max source files to mine per mode (0 = no cap)")
    ap.add_argument("--bars", type=int, default=4, choices=[1, 2, 4, 8],
                    help="loop length in bars; a MidiLooper-legal count (1/2/4/8) so a loop"
                         " loads straight into a track's looper")
    ap.add_argument("--genres", default=None,
                    help="bitmidi: comma genre-word list for the spread (default a broad set)")
    ap.add_argument("--query", default=None,
                    help="bitmidi: free-text search (e.g. an artist). Omit for a genre spread.")
    ap.add_argument("--local", default=None, help="local: folder of .mid to mine into loops")
    ap.add_argument("--genre-tag", default="misc", help="local: genre label for the catalog")
    ap.add_argument("--assets", action="store_true",
                    help="write BUNDLE-SAFE sources into firmware/mix-kit/assets/midi/loops")
    ap.add_argument("--push", action="store_true", help="copy the staging dir to the card")
    args = ap.parse_args()

    if args.assets and args.mode not in (*BUNDLE_SAFE, "all"):
        print(f"[assets] refusing: '{args.mode}' is personal-use-only and must not be committed.",
              file=sys.stderr)
        return 2

    out_loops = assets_default if args.assets else os.path.join(args.out, "loops")
    os.makedirs(out_loops, exist_ok=True)
    seen: set = set()
    # pre-seed de-dup from any prior run
    for root, _dirs, names in os.walk(out_loops):
        for fn in names:
            if fn.lower().endswith(".mid"):
                rel = os.path.relpath(os.path.join(root, fn), out_loops).replace(os.sep, "/")
                seen.add(rel.lower())

    total = 0
    modes = BUNDLE_SAFE if args.mode == "all" else (args.mode,)
    per = (args.limit // len(modes)) if (args.mode == "all" and args.limit) else args.limit
    for m in modes:
        if m == "cc0":
            total += fetch_cc0(out_loops, per, args.bars, seen)
        elif m == "nesmdb":
            total += fetch_nesmdb(out_loops, per, args.bars, seen)
        elif m == "jsb":
            total += fetch_jsb(out_loops, per, args.bars, seen)
        elif m == "pop909":
            total += fetch_pop909(out_loops, per, args.bars, seen)
        elif m == "bitmidi":
            genres = [g.strip() for g in args.genres.split(",") if g.strip()] if args.genres else None
            total += fetch_bitmidi(out_loops, args.query, genres, args.limit, args.bars, seen)
        elif m == "local":
            total += fetch_local(out_loops, args.local, args.genre_tag, args.limit, args.bars, seen)

    if total or os.path.isdir(out_loops):
        write_catalog(out_loops)
    print(f"\nDone: {total} loop(s) in {out_loops}")
    if args.push and not args.assets:
        push(out_loops)
    elif not args.assets:
        print(f"Drag {out_loops} onto the SD card as /midi/loops (or run with --push).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
