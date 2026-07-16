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

After a successful --upload, the repo's SD assets (songs/drums) are synced to the
board automatically via tools/sync_assets.py. Disable with --no-sync or by setting
TDSP_SYNC_ASSETS=false. Flashing doesn't wipe the SD card, so this only writes
what's missing/changed. (Dry runs never sync.)

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
import time

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


def finish(args, rc):
    """Run the post-flash asset sync when appropriate; return the process exit code.
    Only syncs after a real, successful --upload. A sync hiccup is surfaced but the
    flash itself is never undone by it."""
    if not args.upload:
        return rc                                      # dry run -> never sync
    if rc != 0:
        print("[sync] skipped: upload failed", file=sys.stderr)
        return rc
    if sync_disabled(args):
        print("[sync] skipped (disabled via --no-sync / TDSP_SYNC_ASSETS)")
        return rc
    print("[sync] flash OK -> syncing SD assets...")
    port = wait_for_teensy_port()
    if port is None:
        print("[sync] no Teensy port detected after flash; run "
              "'python tools/sync_assets.py' manually once the board is up.", file=sys.stderr)
        return rc
    src = run_sync(port, args.sync_soundfont)
    if src != 0:
        print("[sync] asset sync reported errors (see above); flash itself succeeded.",
              file=sys.stderr)
        return src
    return rc


def sync_disabled(args):
    """Auto-sync is on by default; off via --no-sync or TDSP_SYNC_ASSETS=false/0/no/off."""
    if args.no_sync:
        return True
    return os.environ.get("TDSP_SYNC_ASSETS", "").strip().lower() in ("0", "false", "no", "off")


def wait_for_teensy_port(timeout=20.0, settle=2.5):
    """After a flash the board reboots + re-enumerates. Wait for a Teensy (VID
    16C0) port to (re)appear, then let the firmware boot and mount the SD card.
    Returns the port name, or None if pyserial is missing / nothing showed up."""
    try:
        from serial.tools import list_ports
    except ImportError:
        print("[sync] pyserial not available; letting sync_assets.py self-detect", file=sys.stderr)
        return None
    time.sleep(2.0)                                    # let the reboot begin (old port drops)
    deadline = time.monotonic() + timeout
    port = None
    while time.monotonic() < deadline:
        cands = [p.device for p in list_ports.comports() if p.vid == 0x16C0]
        if cands:
            port = cands[0]
            break
        time.sleep(0.3)
    if port:
        time.sleep(settle)                             # firmware boot + SD mount
    return port


def run_sync(port_hint, soundfont):
    script = os.path.join(HERE, "sync_assets.py")
    cmd = [sys.executable, script]
    if port_hint:
        cmd += ["--port", port_hint]
    if soundfont:
        cmd += ["--soundfont"]
    print(f"[sync] {' '.join(cmd)}")
    return subprocess.run(cmd).returncode


def main():
    ap = argparse.ArgumentParser(description="Flash the mix-kit firmware matching a Teensy's serial.")
    ap.add_argument("--serial", help="skip detection; resolve this serial number")
    ap.add_argument("--upload", action="store_true", help="actually flash (default: dry run)")
    ap.add_argument("--list", action="store_true", help="print the serial->firmware map and exit")
    ap.add_argument("--project-dir", default=DEFAULT_PROJECT_DIR, help="mix-kit PlatformIO project dir")
    ap.add_argument("--tsv", default=TSV, help="board map file")
    ap.add_argument("--no-sync", action="store_true",
                    help="do NOT sync SD assets after a successful upload")
    ap.add_argument("--sync-soundfont", action="store_true",
                    help="also push the GM soundfont during the post-flash sync (big, opt-in)")
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
        rc = resolve_and_run(args.serial, row, args.project_dir, args.upload)
        return finish(args, rc)

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
    return finish(args, rc)


if __name__ == "__main__":
    sys.exit(main())
