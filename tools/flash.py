#!/usr/bin/env python3
"""flash.py -- flash the right mix-kit firmware to the connected Teensy.

Reads tools/boards.tsv (serial -> env, board profile header), detects the
connected board's USB serial number, and runs the matching PlatformIO upload.

DRY-RUN by default: it prints the resolved `pio run ... -t upload` command but
does NOT flash. Add --upload to actually flash. Nothing here touches hardware
unless you pass --upload.

Examples:
    python tools/flash.py --list                 # show the serial->firmware map
    python tools/flash.py                         # detect board, print what WOULD flash
    python tools/flash.py --serial 18402920       # resolve a specific serial (dry-run)
    python tools/flash.py --serial 18402920 --upload   # actually flash it

NOTE (bench-verify): USB-serial detection via `pio device list` is best-effort
and OS/enumeration dependent (a Teensy in HID-bootloader mode is not a COM port).
Use --serial to bypass detection until the detection path is verified on a bench.
"""
import argparse
import csv
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TSV = os.path.join(HERE, "boards.tsv")
# The mix-kit PlatformIO project, promoted to firmware/mix-kit (out of projects/).
DEFAULT_PROJECT_DIR = os.path.normpath(
    os.path.join(HERE, "..", "firmware", "mix-kit")
)


def load_map(path=TSV):
    """serial -> {env, board_header, notes}. Skips comment/blank lines."""
    rows = {}
    with open(path, newline="") as f:
        reader = csv.reader((l for l in f if l.strip() and not l.startswith("#")),
                            delimiter="\t")
        for parts in reader:
            if len(parts) < 2:
                continue
            serial = parts[0].strip()
            rows[serial] = {
                "env": parts[1].strip() if len(parts) > 1 else "",
                "board_header": parts[2].strip() if len(parts) > 2 else "",
                "notes": parts[3].strip() if len(parts) > 3 else "",
            }
    return rows


def detect_serials():
    """Best-effort: connected USB serial numbers via `pio device list`."""
    try:
        out = subprocess.run(
            ["pio", "device", "list", "--json-output"],
            capture_output=True, text=True, check=True,
        ).stdout
        ports = json.loads(out)
    except (subprocess.CalledProcessError, FileNotFoundError, json.JSONDecodeError) as e:
        print(f"[warn] could not enumerate devices ({e}); use --serial", file=sys.stderr)
        return []
    serials = []
    for p in ports:
        hwid = p.get("hwid", "") or ""
        # Teensy (VID 16C0) reports its serial in hwid, e.g. "... SER=18402920 ..."
        m = re.search(r"(?:SER|SNR|SerialNumber)=([0-9A-Za-z]+)", hwid)
        if m:
            serials.append((m.group(1), p.get("port", "?"), p.get("description", "")))
    return serials


def resolve_and_run(serial, row, project_dir, upload):
    env = row["env"]
    header = row["board_header"]
    print(f"  board serial {serial}")
    print(f"    -> env          {env}")
    print(f"    -> board profile {header or '(pinned in env)'}")
    if row["notes"]:
        print(f"    -> notes        {row['notes']}")
    cmd = ["pio", "run", "-d", project_dir, "-e", env, "-t", "upload"]
    print(f"    -> command      {' '.join(cmd)}")
    if not upload:
        print("    (dry run -- pass --upload to actually flash)")
        return 0
    print("    flashing...")
    return subprocess.run(cmd).returncode


def main():
    ap = argparse.ArgumentParser(description="Flash the mix-kit firmware matching a Teensy's serial.")
    ap.add_argument("--serial", help="skip detection; resolve this serial number")
    ap.add_argument("--upload", action="store_true", help="actually flash (default: dry run)")
    ap.add_argument("--list", action="store_true", help="print the serial->firmware map and exit")
    ap.add_argument("--project-dir", default=DEFAULT_PROJECT_DIR, help="mix-kit PlatformIO project dir")
    ap.add_argument("--tsv", default=TSV, help="board map file")
    args = ap.parse_args()

    rows = load_map(args.tsv)

    if args.list:
        print(f"{'serial':<12} {'env':<24} {'board_header':<32} notes")
        for s, r in rows.items():
            print(f"{s:<12} {r['env']:<24} {r['board_header']:<32} {r['notes']}")
        return 0

    if args.serial:
        row = rows.get(args.serial)
        if not row:
            print(f"[error] serial {args.serial} not in {args.tsv}", file=sys.stderr)
            return 2
        return resolve_and_run(args.serial, row, args.project_dir, args.upload)

    # Auto-detect
    found = detect_serials()
    if not found:
        print("[error] no boards detected; pass --serial <n> (see --list)", file=sys.stderr)
        return 2
    known = [(s, port, desc) for (s, port, desc) in found if s in rows]
    if not known:
        print("[error] detected boards, but none are in the map:", file=sys.stderr)
        for s, port, desc in found:
            print(f"    serial {s} on {port} ({desc}) -- add it to {args.tsv}", file=sys.stderr)
        return 2
    rc = 0
    for s, port, desc in known:
        print(f"[detected] {s} on {port} ({desc})")
        rc |= resolve_and_run(s, rows[s], args.project_dir, args.upload)
    return rc


if __name__ == "__main__":
    sys.exit(main())
