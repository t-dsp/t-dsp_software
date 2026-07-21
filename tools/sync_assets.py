#!/usr/bin/env python3
"""sync_assets.py — push the repo's SD-card assets onto a Teensy over USB CDC.

Run this AFTER flashing to (re)populate the card with the songs / drum grooves
the mix-kit firmware expects. No MTP, no card reader, no reflash. Cross-platform
(Windows + Linux, e.g. the jay-mint flash host) — the Python/pyserial companion
to the push_*_serial.ps1 scripts, speaking the same TDspSdXfer '@WB' protocol.

Each tracked asset category is streamed to its canonical path on the card:

    firmware/mix-kit/assets/midi/songs/**.mid  ->  /midi/songs
    firmware/mix-kit/assets/midi/drums/**.mid  ->  /midi/drums

Every file is CRC32-verified by the firmware as it lands, so a clean run means
the card byte-for-byte matches the repo. Flashing the Teensy does NOT wipe the
SD card, so on an unchanged card this just re-verifies; on a fresh card (or after
editing assets) it writes whatever is missing or changed.

The serial port is auto-detected (PJRC VID 0x16C0) unless you pass --port. On
Linux that resolves to /dev/ttyACM*, on Windows to COMx.

BIG BINARIES (soundfonts) are opt-in via --soundfont, because per repo policy
they are NOT tracked in git (see tools/sf2/fonts/.gitignore). --soundfont pushes
the local SF2 to the card path the TSF GM engine loads (/sf2/gm_tsf.sf2).

Examples
--------
    # Auto-detect the Teensy and sync songs + drums:
    python tools/sync_assets.py

    # Explicit port (e.g. on jay-mint), plus the GM soundfont:
    python tools/sync_assets.py --port /dev/ttyACM0 --soundfont

    # Just show which port would be used:
    python tools/sync_assets.py --list

Notes
-----
The Teensy does NOT reset when the port is opened, so this is safe against a live
board. Close any serial monitor / control.html page first — it holds the port
exclusively (you'll get a "permission denied" / "access denied" on open).
"""
import argparse
import os
import sys
import time
import zlib

import serial
from serial.tools import list_ports

US = "\x1f"                      # unit separator used by the @WB framing
TEENSY_VID = 0x16C0             # PJRC (Teensy) USB vendor id
REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

# Asset manifest: add a row to teach the sync a new category.
ASSET_SETS = [
    {"name": "songs", "src": "firmware/mix-kit/assets/midi/songs", "sd_root": "/midi/songs", "glob": ".mid"},
    {"name": "drums", "src": "firmware/mix-kit/assets/midi/drums", "sd_root": "/midi/drums", "glob": ".mid"},
]


def find_teensy_ports():
    return [p for p in list_ports.comports() if (p.vid == TEENSY_VID)]


def resolve_port(requested):
    if requested:
        return requested
    cands = find_teensy_ports()
    if not cands:
        sys.exit("No Teensy (VID 16C0) serial port found. Plug in / power the board, or pass --port.")
    if len(cands) > 1:
        names = ", ".join(p.device for p in cands)
        print(f"Multiple Teensy-class ports found ({names}); using {cands[0].device}. "
              f"Pass --port to override.", file=sys.stderr)
    return cands[0].device


class Device:
    """Buffered line reader over a pyserial port (skips firmware status spam)."""

    def __init__(self, port, baud, attempts=30, delay=0.5):
        # A Teensy re-enumerates after a flash: on Windows the COM name can appear
        # before the device is actually openable (OSError 22/433 "not connected").
        # Retry those; but a "busy/denied" port is held by another app and won't
        # free itself, so fail fast with guidance.
        last = None
        for _ in range(attempts):
            try:
                self.s = serial.Serial(port, baud, timeout=0.2)
                break
            except (serial.SerialException, OSError) as e:
                msg = str(e)
                low = msg.lower()
                if "denied" in low or "busy" in low or "permission" in low:
                    sys.exit(f"Could not open {port}: port is busy. Close any serial monitor / "
                             f"control.html page holding it, then re-run. ({msg})")
                last = e
                time.sleep(delay)
        else:
            sys.exit(f"Could not open {port} after {attempts} attempts "
                     f"(board still enumerating?): {last}")
        try:
            self.s.dtr = True
        except (OSError, ValueError):
            pass
        self._buf = b""

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass

    def reset(self):
        self.s.reset_input_buffer()
        self._buf = b""

    def write_line(self, line):
        self.s.write((line + "\n").encode("latin-1"))

    def _read_line(self, deadline):
        while time.monotonic() < deadline:
            nl = self._buf.find(b"\n")
            if nl >= 0:
                line = self._buf[:nl]
                self._buf = self._buf[nl + 1:]
                return line.rstrip(b"\r").decode("latin-1", "replace")
            chunk = self.s.read(4096)
            if chunk:
                self._buf += chunk
        return None

    def wait_line(self, prefixes, timeout):
        deadline = time.monotonic() + timeout
        while True:
            line = self._read_line(deadline)
            if line is None:
                return None
            for p in prefixes:
                if line.startswith(p):
                    return line


def push_one(dev, local_path, dev_path, fid):
    """Stream one file via @WB. Returns True on CRC-verified success."""
    with open(local_path, "rb") as fh:
        data = fh.read()
    crc = "%08x" % (zlib.crc32(data) & 0xFFFFFFFF)

    dev.reset()
    dev.write_line(f"@WB={fid}{US}{dev_path}{US}{len(data)}{US}{crc}")
    ack = dev.wait_line([f"@WOK={fid}", f"@WERR={fid}"], 5.0)
    if not ack or ack.startswith("@WERR"):
        return False

    mv = memoryview(data)
    off = 0
    while off < len(data):
        c = min(4096, len(data) - off)
        dev.s.write(mv[off:off + c])
        off += c
    dev.s.flush()

    # @WE timeout scales with size (SD write of a multi-MB soundfont takes longer).
    we_timeout = max(30.0, len(data) / 200_000.0)
    res = dev.wait_line([f"@WE={fid}", f"@WERR={fid}"], we_timeout)
    if not res or res.startswith("@WERR"):
        return False
    parts = res.split(US)
    if len(parts) > 2 and parts[2] and parts[2] != crc:
        return False
    return True


def push_files(dev, files, next_id):
    """files: list of (local_path, dev_path). Returns (ok_count, failures[])."""
    ok = 0
    failures = []
    fid = next_id
    for local_path, dev_path in files:
        fid = (fid % 250) + 1
        # Up to 3 attempts with growing backoff. Absorbs the transient that can
        # hit the FIRST transfer(s) right after a flash, while USB/firmware are
        # still settling (a single retry sometimes lands entirely inside it).
        good = False
        for backoff in (0.0, 0.25, 0.6):
            if backoff:
                time.sleep(backoff)
            good = push_one(dev, local_path, dev_path, fid)
            if good:
                break
        if good:
            ok += 1
            print(f"  ok   {dev_path}")
        else:
            failures.append(dev_path)
            print(f"  FAIL {dev_path}", file=sys.stderr)
    return ok, failures, fid


def collect(src_abs, sd_root, ext):
    out = []
    for root, _dirs, names in os.walk(src_abs):
        for n in sorted(names):
            if not n.lower().endswith(ext):
                continue
            local = os.path.join(root, n)
            rel = os.path.relpath(local, src_abs).replace(os.sep, "/")
            out.append((local, f"{sd_root}/{rel}"))
    return out


def main():
    ap = argparse.ArgumentParser(description="Sync repo SD-card assets onto a Teensy over USB.")
    ap.add_argument("--port", help="serial port (default: auto-detect Teensy VID 16C0)")
    ap.add_argument("--baud", type=int, default=115200, help="nominal baud (CDC ignores it)")
    ap.add_argument("--soundfont", action="store_true", help="also push the GM soundfont (large, opt-in)")
    ap.add_argument("--sf2-src", help="soundfont source (default: tools/sf2/fonts/gm_tim.sf2)")
    ap.add_argument("--sf2-dest", default="/sf2/gm_tsf.sf2", help="card path the TSF engine loads")
    ap.add_argument("--list", action="store_true", help="list detected Teensy ports and exit")
    ap.add_argument("--dry-run", action="store_true", help="show what would be pushed, don't open the port")
    ap.add_argument("--loops-src", help="also push a STAGED loops dir (from tools/fetch_loops.py, "
                    "e.g. c:/tmp/t-dsp-loops/loops) to /midi/loops. Loops are NOT tracked in the "
                    "repo, so they are pushed from wherever they were staged, not from assets/.")
    args = ap.parse_args()

    if args.list:
        cands = find_teensy_ports()
        if not cands:
            print("No Teensy (VID 16C0) ports found.")
        for p in cands:
            print(f"{p.device}  {p.description}")
        return 0

    # Build the work list from the manifest.
    plan = []   # (label, files[])
    for s in ASSET_SETS:
        src_abs = os.path.join(REPO_ROOT, s["src"])
        if not os.path.isdir(src_abs):
            print(f"skip  {s['name']}: source not found ({s['src']})", file=sys.stderr)
            continue
        files = collect(src_abs, s["sd_root"], s["glob"])
        plan.append((f"{s['name']} -> {s['sd_root']}", files))

    # Loops are staged (not committed), so their source is an explicit dir, not assets/.
    # Push the .mid tree AND its catalog.tsv (the loops manifest points the app at it).
    if args.loops_src:
        loops_abs = os.path.abspath(args.loops_src)
        if not os.path.isdir(loops_abs):
            print(f"skip  loops: --loops-src not found ({loops_abs})", file=sys.stderr)
        else:
            files = collect(loops_abs, "/midi/loops", ".mid")
            cat = os.path.join(loops_abs, "catalog.tsv")
            if os.path.isfile(cat):
                files.append((cat, "/midi/loops/catalog.tsv"))
            plan.append(("loops -> /midi/loops", files))

    if args.soundfont:
        sf2_src = args.sf2_src or os.path.join(REPO_ROOT, "tools/sf2/fonts/gm_tim.sf2")
        if not os.path.isfile(sf2_src):
            print(f"skip  soundfont: source not found ({sf2_src}). "
                  f"Run tools/fetch_samples.py or pass --sf2-src.", file=sys.stderr)
        else:
            mb = os.path.getsize(sf2_src) / (1024 * 1024)
            print(f"note  soundfont {os.path.basename(sf2_src)} ({mb:.1f} MB) -> {args.sf2_dest}; "
                  f"confirm it matches your flashed engine (TSF expects int16-patched TimGM6mb).",
                  file=sys.stderr)
            plan.append((f"soundfont -> {args.sf2_dest}", [(sf2_src, args.sf2_dest)]))

    total = sum(len(f) for _, f in plan)
    print(f"Repo  : {REPO_ROOT}")
    for label, files in plan:
        print(f"  {label}: {len(files)} file(s)")
    print(f"Total : {total} file(s)")

    if args.dry_run:
        for label, files in plan:
            print(f"\n=== {label} ===")
            for _local, dev_path in files:
                print(f"  {dev_path}")
        return 0

    port = resolve_port(args.port)
    print(f"Port  : {port}\n")

    dev = Device(port, args.baud)
    time.sleep(0.2)
    dev.reset()

    all_failures = []
    fid = 0
    t0 = time.monotonic()
    try:
        for label, files in plan:
            print(f"=== {label}  ({len(files)} file(s)) ===")
            ok, failures, fid = push_files(dev, files, fid)
            all_failures.extend(failures)
            print()
    finally:
        dev.close()

    secs = time.monotonic() - t0
    if all_failures:
        print(f"Sync finished WITH ERRORS in {secs:.1f}s - {len(all_failures)} file(s) failed:",
              file=sys.stderr)
        for f in all_failures:
            print(f"  {f}", file=sys.stderr)
        return 1
    print(f"Sync complete in {secs:.1f}s - card assets match the repo.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
