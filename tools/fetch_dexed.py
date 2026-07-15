#!/usr/bin/env python3
"""fetch_dexed.py — assemble a DX7 cart library for the T-DSP "Dexed" browser.

The mix-kit firmware (projects/spike_esp32_bt_spdif_mix_kit_f32) indexes an SD
folder /dexed/**/*.syx into /tdsp/dexed.ndjson (CatalogDb.h::walkDexed) so the
app/web UI can browse the whole library by folder -> cart -> voice, live, with
no reflash. Today the card ships only the 320 built-in voices; this fetches a
proper library to sit alongside them.

Outputs a folder ready to copy onto the card:

    <out>/dexed/<folder>/<cart>.syx      (32-voice DX7 bulk dumps, 4104 bytes)

The firmware is PICKY about what counts as a cart (see walkDexed):

  * file name ends in ".syx"
  * size is EXACTLY 4104 (sysex-wrapped 32-voice bulk dump) or 4096 (raw bank)
  * the 32 voice names must parse (sdCartVoiceNames > 0)

Anything else is silently dropped at index time. So this script does the
filtering up front: it keeps only 4096/4104-byte files, validates the 4104
ones as real DX7 32-voice bulk dumps (header F0 43 0n 09 20 00 ... F7),
SPLITS concatenated multi-bank files (4104*N) into individual carts, and
enforces the firmware's path limits (depth <= 5, name <= 63 chars). It prints
a full accounting of kept vs dropped and why.

Source
------
alltheweb   (default, network)
    Bobby Blues' "DX7 All The Web" collection: 468 soundbanks / 14,973 patches,
    every file a 4104-byte 32-voice .syx. ~26.7 MB.
      https://bobbyblues.recup.ch/yamaha_dx7/dx7_patches.html
    The author states: "As I downloaded all these soundbanks for free in the
    past, I may consider it as public domain material." An ATTRIBUTION.dexed
    file recording the source is written next to the staged carts.

local       (offline)
    Normalize an existing folder of .syx files (pass --src DIR) into the same
    validated /dexed layout — for adding your own carts or a different pack.

Usage
-----
    python tools/fetch_dexed.py                       # download + stage to c:/tmp/t-dsp-dexed
    python tools/fetch_dexed.py --zip DX7_AllTheWeb.zip   # use an already-downloaded archive
    python tools/fetch_dexed.py local --src C:/my_syx    # normalize a local folder
    python tools/fetch_dexed.py --push                 # stage + copy to the card (card reader / MTP)

By default it writes to a staging dir (c:/tmp/t-dsp-dexed) and does NOT touch
the card — copy <out>/dexed onto the SD later (fast @WB serial push, card
reader, or MTP). With --push it invokes tools/push_to_teensy.ps1.
"""

from __future__ import annotations

import argparse
import io
import os
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile

# --- source ------------------------------------------------------------------
ALLTHEWEB_URL = "https://bobbyblues.recup.ch/yamaha_dx7/patches/DX7_AllTheWeb.zip"
ALLTHEWEB_PAGE = "https://bobbyblues.recup.ch/yamaha_dx7/dx7_patches.html"

ATTRIBUTION = """\
DX7 cart library for T-DSP (/dexed)
===================================

Source: Bobby Blues' "DX7 All The Web" collection
  {page}
  468 soundbanks / 14,973 patches, 32-voice .syx bulk dumps (4104 bytes each).

License: the collection's author states -- "As I downloaded all these
soundbanks for free in the past, I may consider it as public domain material."
Keep this file with the carts if you redistribute them.

Prepared by tools/fetch_dexed.py: only valid 4096/4104-byte 32-voice bulk
dumps are kept; concatenated multi-bank files are split into individual carts;
paths are constrained to the firmware's limits (depth <= 5, name <= 63 chars).
"""

BANK_SIZE_SYSEX = 4104   # F0 43 0n 09 20 00 + 4096 data + checksum + F7
BANK_SIZE_RAW = 4096     # headerless 32-voice bank

# walkDexed limits (CatalogDb.h): kMaxDepth = 5 nesting, nm[64] name buffer.
MAX_DEPTH = 5
MAX_NAME = 59            # leaves room for ".syx" (4) inside the 63-char usable buffer


def _http_get(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "t-dsp-fetch-dexed"})
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.read()


def _is_dx7_bulk(buf: bytes) -> bool:
    """True if `buf` is a well-formed 4104-byte DX7 32-voice bulk dump.

    Header: F0 43 0n 09 20 00  (n = MIDI channel nibble); trailer: ... F7.
    Byte 3 == 0x09 is the "32 voices" format id; 0x20 0x00 is the 4096 count.
    This is exactly what the firmware's sdCartVoiceNames() expects, so a file
    that passes here will parse into 32 names on-device.
    """
    if len(buf) != BANK_SIZE_SYSEX:
        return False
    return (
        buf[0] == 0xF0
        and buf[1] == 0x43
        and (buf[2] & 0xF0) == 0x00
        and buf[3] == 0x09
        and buf[4] == 0x20
        and buf[5] == 0x00
        and buf[-1] == 0xF7
    )


def _sanitize_component(name: str) -> str:
    """FAT-safe, length-capped path component. Preserves readability."""
    name = name.strip().strip(".")
    name = re.sub(r'[<>:"/\\|?*\x00-\x1f]', "_", name)   # illegal on FAT/Win
    name = re.sub(r"\s+", " ", name).strip()
    return name or "_"


def _cap_name(base: str) -> str:
    base = _sanitize_component(base)
    if len(base) > MAX_NAME:
        base = base[:MAX_NAME].rstrip(" _")
    return base or "cart"


class Stager:
    """Writes validated carts into <out>/dexed, deduping names, capping depth."""

    def __init__(self, dexed_dir: str):
        self.dexed = dexed_dir
        self.used: set[str] = set()        # lowercased rel paths already written
        self.kept = 0
        self.split_extra = 0
        self.dropped_size = 0
        self.dropped_hdr = 0

    def _rel_dir(self, parts: list[str]) -> str:
        """Constrain a source subfolder chain to <= MAX_DEPTH-1 dirs, sanitized.

        walkDexed recurses at most MAX_DEPTH levels below /dexed, so flatten
        anything deeper by joining the tail with '_'.
        """
        parts = [_sanitize_component(p) for p in parts if p not in ("", ".", "..")]
        if len(parts) > MAX_DEPTH - 1:
            head = parts[: MAX_DEPTH - 2]
            tail = "_".join(parts[MAX_DEPTH - 2:])
            parts = head + [_cap_name(tail)]
        else:
            parts = [_cap_name(p) for p in parts]
        return os.path.join(*parts) if parts else ""

    def _unique(self, rel_dir: str, base: str) -> str:
        cand = os.path.join(rel_dir, base + ".syx")
        i = 2
        while cand.lower() in self.used:
            cand = os.path.join(rel_dir, f"{_cap_name(base + f'_{i}')}.syx")
            i += 1
        self.used.add(cand.lower())
        return cand

    def add(self, src_parts: list[str], base: str, buf: bytes) -> None:
        """Route one source .syx (any size) into 0+ validated carts."""
        rel_dir = self._rel_dir(src_parts)
        base = _cap_name(base)

        # Raw headerless bank: keep as-is (can't validate, size is the signal).
        if len(buf) == BANK_SIZE_RAW:
            self._write(rel_dir, base, buf)
            self.kept += 1
            return

        # One or more concatenated 32-voice bulk dumps.
        if len(buf) % BANK_SIZE_SYSEX == 0 and len(buf) >= BANK_SIZE_SYSEX:
            n = len(buf) // BANK_SIZE_SYSEX
            wrote = 0
            for i in range(n):
                chunk = buf[i * BANK_SIZE_SYSEX:(i + 1) * BANK_SIZE_SYSEX]
                if not _is_dx7_bulk(chunk):
                    self.dropped_hdr += 1
                    continue
                nm = base if n == 1 else _cap_name(f"{base}_{i + 1}")
                self._write(rel_dir, nm, chunk)
                wrote += 1
            if wrote:
                self.kept += 1
                self.split_extra += wrote - 1
            return

        self.dropped_size += 1

    def _write(self, rel_dir: str, base: str, buf: bytes) -> None:
        rel = self._unique(rel_dir, base)
        dst = os.path.join(self.dexed, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(buf)


def _walk_syx(root: str):
    """Yield (relative_parts, base_name_no_ext, bytes) for every .syx under root."""
    for dirpath, _dirs, files in os.walk(root):
        rel = os.path.relpath(dirpath, root)
        parts = [] if rel == "." else rel.split(os.sep)
        for fn in files:
            if not fn.lower().endswith(".syx"):
                continue
            with open(os.path.join(dirpath, fn), "rb") as f:
                buf = f.read()
            yield parts, os.path.splitext(fn)[0], buf


def stage(dexed_dir: str, src_root: str) -> Stager:
    if os.path.isdir(dexed_dir):
        shutil.rmtree(dexed_dir)
    os.makedirs(dexed_dir, exist_ok=True)
    st = Stager(dexed_dir)
    for parts, base, buf in _walk_syx(src_root):
        st.add(parts, base, buf)
    return st


def push(out_dexed: str) -> None:
    # push_to_teensy.ps1 lands the source folder under the card root using its
    # TOP-LEVEL name -> pass <out>/dexed (leaf "dexed") to land as /dexed.
    ps1 = os.path.join(os.path.dirname(__file__), "push_to_teensy.ps1")
    if not os.path.exists(ps1):
        print(f"[push] {ps1} not found; copy {out_dexed} to the card as /dexed manually.")
        return
    print("[push] invoking push_to_teensy.ps1 …")
    subprocess.run(
        ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", ps1,
         "-SourcePath", out_dexed],
        check=False,
    )


def main() -> int:
    ap = argparse.ArgumentParser(description="Assemble a DX7 cart library for T-DSP /dexed.")
    ap.add_argument("mode", nargs="?", default="alltheweb", choices=["alltheweb", "local"],
                    help="alltheweb: download Bobby Blues' pack; local: normalize --src DIR")
    ap.add_argument("--out", default="c:/tmp/t-dsp-dexed",
                    help="staging dir; carts land in <out>/dexed")
    ap.add_argument("--zip", help="use an already-downloaded DX7_AllTheWeb.zip instead of fetching")
    ap.add_argument("--src", help="local mode: folder of .syx files to normalize")
    ap.add_argument("--push", action="store_true", help="copy the staging dir to the card")
    args = ap.parse_args()

    out_dexed = os.path.join(args.out, "dexed")
    work = os.path.join(args.out, "_src")

    if args.mode == "local":
        if not args.src or not os.path.isdir(args.src):
            print("local mode needs --src DIR (a folder of .syx files)"); return 2
        src_root = args.src
        attribution = None
    else:
        if os.path.isdir(work):
            shutil.rmtree(work)
        os.makedirs(work, exist_ok=True)
        if args.zip:
            print(f"[alltheweb] extracting {args.zip}")
            data = open(args.zip, "rb").read()
        else:
            print(f"[alltheweb] downloading {ALLTHEWEB_URL} (~27 MB) …")
            data = _http_get(ALLTHEWEB_URL)
            print(f"[alltheweb] {len(data)//1024} KB")
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            z.extractall(work)
        src_root = work
        attribution = ATTRIBUTION.format(page=ALLTHEWEB_PAGE)

    print(f"[stage] normalizing carts -> {out_dexed}")
    st = stage(out_dexed, src_root)

    if attribution:
        with open(os.path.join(out_dexed, "ATTRIBUTION.dexed"), "w", encoding="utf-8") as f:
            f.write(attribution)

    total = st.kept + st.dropped_size + st.dropped_hdr
    print("\n=== dexed prep summary ===")
    print(f"  source .syx files seen : {total}")
    print(f"  carts kept             : {st.kept}")
    if st.split_extra:
        print(f"  + extra from splitting : {st.split_extra} (concatenated multi-bank files)")
    print(f"  dropped (bad size)     : {st.dropped_size}")
    print(f"  dropped (bad header)   : {st.dropped_hdr}")
    print(f"  TOTAL carts written    : {st.kept + st.split_extra}")
    print(f"  -> {out_dexed}")

    if args.push:
        push(out_dexed)
    else:
        print(f"\nHold: copy {out_dexed} onto the card as /dexed when the fast transfer is ready,")
        print("then send @REINDEX (or delete /tdsp/.sig first) to rebuild dexed.ndjson.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
