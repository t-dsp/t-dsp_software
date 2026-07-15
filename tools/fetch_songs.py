#!/usr/bin/env python3
"""fetch_songs.py — assemble a curated MIDI SONG library for the T-DSP "@" app.

The mix-kit firmware (projects/spike_esp32_bt_spdif_mix_kit_f32) scans /songs/*.mid
off the SD card and lists each file in the app/web/serial song picker with NO
firmware rebuild (buildSongList -> scanSongDir). Unlike the drum grooves, a song is
a full Standard MIDI File: on a General-MIDI engine (TSF / SF2 / OPL3) each channel
plays its own program, so multi-instrument arrangements render correctly; on a mono-
timbral engine (Dexed / ymfm) the whole arrangement collapses onto the active patch.

Firmware constraints this fetcher curates around
------------------------------------------------
* SONG-LIST CAP: g_songs[48] holds the ENTIRE picker (test seqs + baked fallbacks +
  SD files). Only ~30 SD songs ever show — so we CURATE a spread, we do not dump a
  whole corpus. Default --limit is 30 for that reason.
* MAX_EVENTS = 24000: loadSmfFile refuses a file with more events. We skip files
  whose parsed event count would blow the cap (a cheap pre-scan below).
* Songs list by FILENAME (there is no /songs/catalog.tsv — that manifest is drums-
  only), so we write tidy "Artist - Title.mid" names, clamped to fit r.name[48].
* Channel 10 (drums) is handled by the engine; we leave files byte-for-byte intact.

Outputs a folder ready to drag onto the card:

    <out>/songs/<Artist - Title>.mid

Modes
-----
lakh   (network, DEFAULT)
    Lakh MIDI Dataset "clean_midi" subset (~17k files already labelled
    <Artist>/<Title>, Colin Raffel, CC-BY 4.0). Best fit: real names make a
    browsable library. Curated round-robin across artists so a small --limit
    samples widely rather than taking 30 songs by one band.

tmom   (network)
    "The Magic of MIDI" v1 (~169k pop / rock / game / TV MIDIs, archive.org,
    public-domain compilation). One big tar; we stream it and curate a spread by
    first letter. Filenames are kept (trimmed) since the pack has no artist tree.

giant  (network)
    ByteDance GiantMIDI-Piano (~10k solo-piano transcriptions of classical works,
    CC-BY 4.0, filenames "Surname, Firstname, Title.mid"). Sounds excellent even
    on a single voice. The full set is distribution-gated on the upstream repo, so
    if the download URL is unreachable we print manual-download instructions.

all
    lakh + tmom + giant, splitting --limit evenly across the three.

Usage
-----
    python tools/fetch_songs.py                     # lakh, 30 curated songs
    python tools/fetch_songs.py lakh --limit 30
    python tools/fetch_songs.py tmom --limit 30
    python tools/fetch_songs.py giant --limit 24
    python tools/fetch_songs.py all --push          # stage + copy to the card

By default the script writes to a staging directory (default c:/tmp/t-dsp-songs)
and prints instructions for dragging <out>/songs onto the SD card. With --push it
invokes tools/push_to_teensy.ps1 (same auto-detect as fetch_drums.py / fetch_samples.py).

Licensing
---------
Each fetched pack carries its own license/attribution file written next to the
songs (LICENSE.lakh / ATTRIBUTION.tmom / LICENSE.giant). Lakh clean_midi and
GiantMIDI-Piano are CC-BY 4.0 — keep the attribution if you redistribute. The
Magic of MIDI is a public-domain community compilation. These are for personal /
demo use on the device.
"""

from __future__ import annotations

import argparse
import os
import re
import struct
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile

# --- Corpus sources ----------------------------------------------------------
LAKH_URL = "http://hog.ee.columbia.edu/craffel/lmd/clean_midi.tar.gz"
TMOM_URL = "https://archive.org/download/themagicofmidiv1/The_Magic_of_MIDI.tar"
# GiantMIDI's full set is gated upstream; this is the evaluation drop that IS
# directly downloadable. Override with --url for the full midis_v1.2.zip if you
# have the link from bytedance/GiantMIDI-Piano.
GIANT_URL = ("https://github.com/bytedance/GiantMIDI-Piano/releases/download/"
             "v1.2/midis_for_evaluation.zip")

MAX_EVENTS = 24000          # mirror the firmware cap (loadSmfFile refuses more)
MIN_NOTES = 120             # skip near-empty / fragment files
NAME_MAX = 44               # keep the display stem inside r.name[48] with margin


# --- Networking --------------------------------------------------------------
def _download(url: str, dest: str) -> bool:
    """Stream url to dest with a coarse progress meter. Returns True on success."""
    print(f"[net] GET {url}")
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "t-dsp-fetch-songs"})
        with urllib.request.urlopen(req, timeout=120) as r, open(dest, "wb") as f:
            total = int(r.headers.get("Content-Length", 0))
            got = 0
            mark = 0
            while True:
                chunk = r.read(1 << 20)          # 1 MiB
                if not chunk:
                    break
                f.write(chunk)
                got += len(chunk)
                if got - mark >= (16 << 20):     # print every 16 MiB
                    mark = got
                    if total:
                        print(f"      {got >> 20} / {total >> 20} MiB")
                    else:
                        print(f"      {got >> 20} MiB")
        print(f"[net] saved {got >> 20} MiB -> {dest}")
        return True
    except Exception as e:                       # noqa: BLE001
        print(f"[net] download failed ({e}).")
        return False


# --- Cheap SMF pre-scan (validate + count, no full parse) --------------------
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


def _smf_stats(mid: bytes):
    """Return (note_ons, total_events, channels_set) or None if not a sane SMF.

    Defensive walk — every read is bounds-checked so a truncated/garbage file
    returns None instead of raising. We only need rough counts to (a) reject files
    that would exceed MAX_EVENTS in the firmware and (b) reject empty fragments.
    """
    if len(mid) < 14 or mid[:4] != b"MThd":
        return None
    ntrks = struct.unpack(">H", mid[10:12])[0]
    division = struct.unpack(">H", mid[12:14])[0]
    if division == 0 or (division & 0x8000):     # SMPTE / zero division: firmware skips it
        return None
    note_ons = 0
    total = 0
    channels: set[int] = set()
    i = 14
    for _ in range(ntrks):
        if i + 8 > len(mid) or mid[i:i + 4] != b"MTrk":
            break
        clen = struct.unpack(">I", mid[i + 4:i + 8])[0]
        j = i + 8
        end = min(j + clen, len(mid))
        running = 0
        while j < end:
            _, j = _read_vlq(mid, j, end)
            if j >= end:
                break
            b = mid[j]
            if b == 0xFF:                        # meta
                if j + 1 >= end:
                    break
                j += 2
                mlen, j = _read_vlq(mid, j, end)
                j += mlen
                running = 0
                continue
            if b in (0xF0, 0xF7):                # sysex
                j += 1
                slen, j = _read_vlq(mid, j, end)
                j += slen
                running = 0
                continue
            if b & 0x80:
                status = b
                running = b
                j += 1
            else:
                status = running
            hi = status & 0xF0
            if hi == 0x90:                       # note-on (vel>0 => a real strike)
                if j + 1 < end and mid[j + 1] > 0:
                    note_ons += 1
                channels.add(status & 0x0F)
            elif 0x80 <= hi <= 0xE0:
                channels.add(status & 0x0F)
            total += 1
            j += 1 if hi in (0xC0, 0xD0) else 2
        i = end
    return note_ons, total, channels


def _usable(mid: bytes) -> bool:
    st = _smf_stats(mid)
    if not st:
        return False
    note_ons, total, _channels = st
    # total events is a lower bound on what loadSmfFile allocates; leave headroom.
    return MIN_NOTES <= note_ons and total < MAX_EVENTS


# --- Filename hygiene --------------------------------------------------------
_BAD = re.compile(r"[^A-Za-z0-9 ()\-,'&.]+")


def _clean(name: str) -> str:
    name = _BAD.sub(" ", name)
    name = re.sub(r"\s+", " ", name).strip(" .-")
    return name[:NAME_MAX].strip()


def _dedup(name: str, seen: set[str]) -> str:
    base = name
    k = 2
    while name.lower() in seen:
        suffix = f" ({k})"
        name = base[:NAME_MAX - len(suffix)].strip() + suffix
        k += 1
    seen.add(name.lower())
    return name


# --- Curation: round-robin across groups so a small --limit samples widely ----
def _round_robin(groups: list[list], limit: int) -> list:
    picks: list = []
    pools = [list(g) for g in groups if g]
    while pools and (not limit or len(picks) < limit):
        pools = [p for p in pools if p]
        for p in list(pools):
            picks.append(p.pop(0))
            if limit and len(picks) >= limit:
                break
    return picks


def _write(out_songs: str, disp: str, data: bytes, seen: set[str]) -> bool:
    disp = _clean(disp)
    if not disp or not _usable(data):
        return False
    disp = _dedup(disp, seen)
    with open(os.path.join(out_songs, disp + ".mid"), "wb") as f:
        f.write(data)
    print(f"  + {disp}.mid ({len(data)} B)")
    return True


# --- lakh: clean_midi subset (CC-BY 4.0) -------------------------------------
LAKH_LICENSE = """\
Lakh MIDI Dataset — clean_midi subset
Copyright (c) Colin Raffel. Licensed CC-BY 4.0
(https://creativecommons.org/licenses/by/4.0/).
Source: https://colinraffel.com/projects/lmd/
Cite: Colin Raffel, "Learning-Based Methods for Comparing Sequences, with
Applications to Audio-to-MIDI Alignment and Matching", PhD Thesis, 2016.
Keep this attribution if you redistribute these files.
"""


def fetch_lakh(out_songs: str, limit: int, url: str, seen: set[str]) -> int:
    with tempfile.TemporaryDirectory() as td:
        tgz = os.path.join(td, "clean_midi.tar.gz")
        if not _download(url, tgz):
            print("      Get clean_midi.tar.gz from https://colinraffel.com/projects/lmd/")
            print(f"      and unpack its .mid files into: {out_songs}")
            return 0
        print("[lakh] indexing archive by artist...")
        try:
            tf = tarfile.open(tgz, "r:gz")
        except Exception as e:                   # noqa: BLE001
            print(f"[lakh] not a readable tar.gz ({e}).")
            return 0
        # Group members by artist (the directory under clean_midi/).
        by_artist: dict[str, list] = {}
        for m in tf.getmembers():
            if not (m.isfile() and m.name.lower().endswith(".mid")):
                continue
            parts = m.name.split("/")
            artist = parts[-2] if len(parts) >= 2 else "Unknown"
            by_artist.setdefault(artist, []).append(m)
        groups = [[(a, m) for m in ms] for a, ms in sorted(by_artist.items())]
        n = 0
        for artist, m in _round_robin(groups, limit * 3):   # overshoot; some get filtered
            if limit and n >= limit:
                break
            try:
                data = tf.extractfile(m).read()
            except Exception:                    # noqa: BLE001
                continue
            title = os.path.basename(m.name)[:-4]
            if _write(out_songs, f"{artist} - {title}", data, seen):
                n += 1
        tf.close()
    with open(os.path.join(out_songs, "LICENSE.lakh"), "w", encoding="utf-8") as f:
        f.write(LAKH_LICENSE)
    print(f"[lakh] staged {n} songs (CC-BY 4.0) into {out_songs}")
    return n


# --- tmom: The Magic of MIDI v1 (public-domain compilation) ------------------
TMOM_ATTRIBUTION = """\
The Magic of MIDI v1 — a community-compiled archive of ~169k MIDI files
(pop / rock / game / TV themes), hosted on the Internet Archive:
https://archive.org/details/themagicofmidiv1
The compilation is distributed for preservation; individual files are of varied
provenance. Use for personal / demo purposes on the device.
"""


def fetch_tmom(out_songs: str, limit: int, url: str, seen: set[str]) -> int:
    with tempfile.TemporaryDirectory() as td:
        tar = os.path.join(td, "tmom.tar")
        if not _download(url, tar):
            print(f"      Get the tar from https://archive.org/details/themagicofmidiv1")
            print(f"      and unpack its .mid files into: {out_songs}")
            return 0
        print("[tmom] indexing archive...")
        try:
            tf = tarfile.open(tar, "r:*")
        except Exception as e:                   # noqa: BLE001
            print(f"[tmom] not a readable tar ({e}).")
            return 0
        mids = [m for m in tf.getmembers()
                if m.isfile() and m.name.lower().endswith(".mid")]
        # Spread by leading character of the basename so a small limit isn't all "A".
        by_first: dict[str, list] = {}
        for m in mids:
            base = os.path.basename(m.name)
            key = (base[:1] or "?").upper()
            by_first.setdefault(key, []).append(m)
        groups = [ms for _, ms in sorted(by_first.items())]
        n = 0
        for m in _round_robin(groups, limit * 3):
            if limit and n >= limit:
                break
            try:
                data = tf.extractfile(m).read()
            except Exception:                    # noqa: BLE001
                continue
            title = os.path.basename(m.name)[:-4]
            if _write(out_songs, title, data, seen):
                n += 1
        tf.close()
    with open(os.path.join(out_songs, "ATTRIBUTION.tmom"), "w", encoding="utf-8") as f:
        f.write(TMOM_ATTRIBUTION)
    print(f"[tmom] staged {n} songs into {out_songs}")
    return n


# --- giant: GiantMIDI-Piano (CC-BY 4.0) --------------------------------------
GIANT_LICENSE = """\
GiantMIDI-Piano
Copyright (c) ByteDance. Licensed CC-BY 4.0
(https://creativecommons.org/licenses/by/4.0/).
Source: https://github.com/bytedance/GiantMIDI-Piano
Solo-piano transcriptions of classical works (filenames "Surname, First, Title").
Keep this attribution if you redistribute these files.
"""


def fetch_giant(out_songs: str, limit: int, url: str, seen: set[str]) -> int:
    with tempfile.TemporaryDirectory() as td:
        zpath = os.path.join(td, "giant.zip")
        if not _download(url, zpath):
            print("      GiantMIDI-Piano's full set is gated; see the download link in")
            print("      https://github.com/bytedance/GiantMIDI-Piano (disk_files.md),")
            print(f"      then unpack its .mid files into: {out_songs}")
            print("      (or pass a direct URL with --url)")
            return 0
        print("[giant] indexing archive by composer...")
        try:
            zf = zipfile.ZipFile(zpath)
        except Exception as e:                   # noqa: BLE001
            print(f"[giant] not a readable zip ({e}).")
            return 0
        names = [nm for nm in zf.namelist() if nm.lower().endswith(".mid")]
        by_composer: dict[str, list] = {}
        for nm in names:
            base = os.path.basename(nm)
            composer = base.split(",")[0].strip() or "Unknown"
            by_composer.setdefault(composer, []).append(nm)
        groups = [ms for _, ms in sorted(by_composer.items())]
        n = 0
        for nm in _round_robin(groups, limit * 3):
            if limit and n >= limit:
                break
            try:
                data = zf.read(nm)
            except Exception:                    # noqa: BLE001
                continue
            base = os.path.basename(nm)[:-4]
            parts = [p.strip() for p in base.split(",")]
            # "Surname, First, Title" -> "Surname - Title"; else keep the stem.
            disp = f"{parts[0]} - {parts[-1]}" if len(parts) >= 3 else base
            if _write(out_songs, disp, data, seen):
                n += 1
        zf.close()
    with open(os.path.join(out_songs, "LICENSE.giant"), "w", encoding="utf-8") as f:
        f.write(GIANT_LICENSE)
    print(f"[giant] staged {n} songs (CC-BY 4.0) into {out_songs}")
    return n


# --- push (reuse the shared pusher) ------------------------------------------
def push(out_songs: str) -> None:
    ps1 = os.path.join(os.path.dirname(__file__), "push_to_teensy.ps1")
    if not os.path.exists(ps1):
        print(f"[push] {ps1} not found; copy {out_songs} to the card as /songs manually.")
        return
    print("[push] invoking push_to_teensy.ps1...")
    subprocess.run(
        ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", ps1,
         "-SourcePath", out_songs],
        check=False,
    )


def main() -> int:
    ap = argparse.ArgumentParser(description="Assemble a curated MIDI song library for T-DSP.")
    ap.add_argument("mode", nargs="?", default="lakh",
                    choices=["lakh", "tmom", "giant", "all"])
    ap.add_argument("--out", default="c:/tmp/t-dsp-songs",
                    help="staging dir; songs land in <out>/songs")
    ap.add_argument("--limit", type=int, default=30,
                    help="max songs to curate (firmware shows ~48 total; default 30)")
    ap.add_argument("--url", default=None,
                    help="override the download URL for the chosen mode")
    ap.add_argument("--push", action="store_true", help="copy the staging dir to the card")
    args = ap.parse_args()

    out_songs = os.path.join(args.out, "songs")
    os.makedirs(out_songs, exist_ok=True)
    seen: set[str] = set()
    for fn in os.listdir(out_songs):             # pre-seed de-dup with any prior run
        if fn.lower().endswith(".mid"):
            seen.add(fn[:-4].lower())

    total = 0
    if args.mode == "all":
        per = max(1, args.limit // 3)
        total += fetch_lakh(out_songs, per, args.url or LAKH_URL, seen)
        total += fetch_tmom(out_songs, per, args.url or TMOM_URL, seen)
        total += fetch_giant(out_songs, per, args.url or GIANT_URL, seen)
    elif args.mode == "lakh":
        total += fetch_lakh(out_songs, args.limit, args.url or LAKH_URL, seen)
    elif args.mode == "tmom":
        total += fetch_tmom(out_songs, args.limit, args.url or TMOM_URL, seen)
    elif args.mode == "giant":
        total += fetch_giant(out_songs, args.limit, args.url or GIANT_URL, seen)

    print(f"\nDone: {total} song(s) in {out_songs}")
    if total > 48:
        print("NOTE: the firmware picker caps at ~48 songs (g_songs[48], incl. test seqs);")
        print("      trim the folder or lower --limit so your favorites make the cut.")
    if args.push:
        push(out_songs)
    else:
        print(f"Drag {out_songs} onto the SD card as /songs (or run with --push).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
