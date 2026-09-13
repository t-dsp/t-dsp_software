#!/usr/bin/env python3
"""build_dexed_index.py — build the /dexed voice-search index OFF the Teensy and push it.

The app's Synth/Voices search box sends "@READ=@dxfind:<query>"; the firmware answers by
scanning /tdsp/.dxsearch, one line per cart:

    <rel>\\t<name>\\t<v0>\\x1f<v1>\\x1f...\\x1f<v31>\\n

    rel  = cart path relative to /dexed, keeping ".syx"  (what @DXVL / @DXPICK expect)
    name = the cart's file name without its extension    (display label)
    vN   = its 32 voice names

The firmware used to build this file itself on the first search after /dexed changed,
opening every cart inside one blocking @READ. That stalled the synth for minutes (the
app's read watchdog then kept re-sending searches), so the firmware no longer builds it.
Run this tool whenever the card's /dexed library changes. Search reports "no search
index" until it has been run once.

Two sources:

  device (default)  Walk the card over USB with @DXLS (the browser's own listing, so the
                    index matches exactly what the browser can open) + @DXVL per cart.
                    Every request is short, so the synth keeps running while it walks.
                    Needs the USB port: close the app / serial monitor first.

  --from-dir DIR    Read a local copy of /dexed (a card reader mount, or the staging
                    folder from tools/fetch_dexed.py). Much faster, but only correct if
                    DIR really matches what is on the card.

Examples
--------
    python tools/build_dexed_index.py                      # read the card, push the index
    python tools/build_dexed_index.py --port COM4
    python tools/build_dexed_index.py --from-dir E:/dexed  # card reader: writes E:/tdsp/.dxsearch
    python tools/build_dexed_index.py --from-dir c:/tmp/t-dsp-dexed/dexed --push
    python tools/build_dexed_index.py --out c:/tmp/dxsearch --no-push   # device walk, keep file only

Carts the firmware can't open are left out and listed at the end: rel paths of 160+
bytes (the @DXLS/@DXPICK buffers), names of 64+ bytes (truncated by the @DXLS listing),
names containing '|' (the @DXLS/@DXVL field separator), and invalid carts.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sync_assets import Device, push_one, resolve_port  # noqa: E402  (shared @WB client)

DEV_INDEX_PATH = "/tdsp/.dxsearch"
MAX_DEPTH = 5            # tdsp::catdb::kMaxDepth — firmware browses at most this deep
MAX_REL_BYTES = 159      # char rel[160] in @DXLS / char buf[160] in @DXPICK
MAX_NAME_BYTES = 63      # SdDirEntry::name[64] — longer names come back truncated from @DXLS
MAX_LINE_BYTES = 1000    # firmware scan buffer is 1024; longer lines are skipped on device
VOICES = 32
VMEM = 128
NAME_OFF = 118           # voice name = VMEM bytes 118..127
NAME_LEN = 10


class Skips:
    def __init__(self):
        self.items = []   # (rel, reason)

    def add(self, rel, reason):
        self.items.append((rel, reason))

    def report(self):
        if not self.items:
            return
        print(f"\nSkipped {len(self.items)} entr{'y' if len(self.items) == 1 else 'ies'} "
              f"the firmware can't open:", file=sys.stderr)
        for rel, reason in self.items[:40]:
            print(f"  {reason:<28} {rel}", file=sys.stderr)
        if len(self.items) > 40:
            print(f"  ... and {len(self.items) - 40} more", file=sys.stderr)


def clean_voice_name(raw: bytes) -> bytes:
    """Firmware voiceNameFromVmem, plus: control/high bytes -> space, so a stray \\t, \\n or
    \\x1f in a patch name can't break the index line format."""
    b = bytes(c if 0x20 <= c < 0x7F else 0x20 for c in raw)
    return b.rstrip(b" ")


def cart_voice_names(buf: bytes):
    """Mirror of firmware cartPayloadOffset + sdCartVoiceNames. None if not a cart."""
    if len(buf) >= 6 + 4096 + 2 and buf[0] == 0xF0 and buf[1] == 0x43 and buf[3] == 0x09:
        off = 6
    elif len(buf) == 4096:
        off = 0
    else:
        return None
    payload = buf[off:off + VOICES * VMEM]
    if len(payload) < VOICES * VMEM:
        return None
    return [clean_voice_name(payload[v * VMEM + NAME_OFF:v * VMEM + NAME_OFF + NAME_LEN])
            for v in range(VOICES)]


def strip_ext(name: bytes) -> bytes:
    dot = name.rfind(b".")
    return name[:dot] if dot >= 0 else name


def index_line(rel: bytes, voices) -> bytes:
    name = strip_ext(rel.rsplit(b"/", 1)[-1])
    return rel + b"\t" + name + b"\t" + b"\x1f".join(voices) + b"\n"


def firmware_can_open(rel: bytes, leaf: bytes, skips: Skips) -> bool:
    shown = rel.decode("utf-8", "replace")
    if len(leaf) > MAX_NAME_BYTES:
        skips.add(shown, "name > 63 bytes")
        return False
    if len(rel) > MAX_REL_BYTES:
        skips.add(shown, "path > 159 bytes")
        return False
    if b"|" in rel or b"\t" in rel or b"\x1f" in rel:
        skips.add(shown, "'|'/tab in path")
        return False
    return True


# ---- source: local directory -------------------------------------------------
def walk_local(root: str, skips: Skips):
    lines = []

    def visit(abs_dir, rel_dir: bytes, depth):
        if depth > MAX_DEPTH:
            return
        try:
            entries = sorted(os.scandir(abs_dir), key=lambda e: e.name.lower())
        except OSError as e:
            skips.add(rel_dir.decode("utf-8", "replace"), f"unreadable ({e.strerror})")
            return
        for e in entries:
            if e.name.startswith(".") or e.name == "System Volume Information":
                continue
            leaf = e.name.encode("utf-8")
            rel = rel_dir + b"/" + leaf if rel_dir else leaf
            if e.is_dir():
                if len(leaf) > MAX_NAME_BYTES or len(rel) > MAX_REL_BYTES:
                    firmware_can_open(rel, leaf, skips)
                    continue
                visit(e.path, rel, depth + 1)
                continue
            if not e.name.lower().endswith(".syx") or e.stat().st_size not in (4104, 4096):
                continue
            if not firmware_can_open(rel, leaf, skips):
                continue
            with open(e.path, "rb") as f:
                voices = cart_voice_names(f.read())
            if voices is None:
                skips.add(rel.decode("utf-8", "replace"), "not a 32-voice cart")
                continue
            lines.append(index_line(rel, voices))

    visit(root, b"", 0)
    return lines


# ---- source: the card, over USB -------------------------------------------------
class DeviceWalker:
    """Talks latin-1 end to end so SD file-name bytes round-trip unchanged."""

    def __init__(self, dev: Device, timeout: float):
        self.dev = dev
        self.timeout = timeout

    def _ask(self, cmd: str, prefix: str, retries=2):
        for _ in range(retries + 1):
            self.dev.write_line(cmd)
            line = self.dev.wait_line([prefix], self.timeout)
            if line is not None:
                return line
        return None

    def listdir(self, rel: str):
        """All @DXLS pages for /dexed/<rel> -> (folders, carts), names as latin-1 str."""
        folders, carts = [], []
        page, npages = 0, 1
        while page < npages:
            line = self._ask(f"@DXLS={rel}\t{page}", f"@DXLS={rel}\t{page}\t")
            if line is None:
                raise TimeoutError(f"no @DXLS reply for /dexed/{rel} page {page}")
            fields = line[len("@DXLS="):].split("|")
            head = fields[0].split("\t")
            npages = max(1, int(head[2])) if len(head) >= 3 and head[2].isdigit() else 1
            for ent in fields[1:]:
                if len(ent) < 2:
                    continue
                (folders if ent[0] == "D" else carts).append(ent[1:])
            page += 1
        return folders, carts

    def voices(self, rel: str):
        """32 voice names (bytes) for /dexed/<rel>, or [] if the firmware can't read the cart."""
        line = self._ask(f"@DXVL={rel}", f"@DXVL={rel}")
        if line is None:
            raise TimeoutError(f"no @DXVL reply for {rel}")
        body = line[len(f"@DXVL={rel}"):]
        if body and body[0] != "|":
            return self._voices_from_file(rel)   # prefix matched another cart's reply
        names = body.split("|")[1:]
        if len(names) == VOICES:
            return [n.encode("latin-1") for n in names]
        if not names:
            return []                            # firmware couldn't read it
        # A '|' inside a voice name makes the @DXVL reply ambiguous: read the raw cart instead.
        return self._voices_from_file(rel)

    def _voices_from_file(self, rel: str):
        data = self.read_file("/dexed/" + rel)
        voices = cart_voice_names(data) if data is not None else None
        return voices or []

    def read_file(self, path: str):
        """@READ a small file (@FB/@FD/@FE framing). Returns bytes, or None on @FERR."""
        import base64
        self.dev.write_line(f"@READ={path}")
        head = self.dev.wait_line(["@FB=", "@FERR="], self.timeout)
        if head is None:
            raise TimeoutError(f"no @READ reply for {path}")
        if head.startswith("@FERR="):
            return None
        fid = head[4:].split("\x1f")[0]
        parts = {}
        while True:
            line = self.dev.wait_line([f"@FD={fid}\x1f", f"@FE={fid}\x1f"], self.timeout)
            if line is None:
                raise TimeoutError(f"@READ of {path} stalled")
            if line.startswith("@FE="):
                break
            _, seq, b64 = line[4:].split("\x1f", 2)
            parts[int(seq)] = b64
        return b"".join(base64.b64decode(parts[k]) for k in sorted(parts))


def walk_device(dev: Device, timeout: float, skips: Skips):
    w = DeviceWalker(dev, timeout)
    lines = []
    t0 = time.monotonic()

    def visit(rel_dir: str, depth):
        if depth > MAX_DEPTH:
            return
        folders, carts = w.listdir(rel_dir)
        for leaf in carts:
            rel = f"{rel_dir}/{leaf}" if rel_dir else leaf
            rel_b = rel.encode("latin-1")
            if not firmware_can_open(rel_b, leaf.encode("latin-1"), skips):
                continue
            names = w.voices(rel)
            if not names:
                skips.add(rel_b.decode("utf-8", "replace"), "not a readable 32-voice cart")
                continue
            voices = [clean_voice_name(n) for n in names]
            lines.append(index_line(rel_b, voices))
            if len(lines) % 100 == 0:
                print(f"  {len(lines)} carts  ({time.monotonic() - t0:.0f}s)  {rel_dir or '/'}",
                      flush=True)
        for leaf in folders:
            rel = f"{rel_dir}/{leaf}" if rel_dir else leaf
            if not firmware_can_open(rel.encode("latin-1"), leaf.encode("latin-1"), skips):
                continue
            visit(rel, depth + 1)

    visit("", 0)
    return lines


def main():
    ap = argparse.ArgumentParser(description="Build + push the /dexed voice-search index (/tdsp/.dxsearch).")
    ap.add_argument("--from-dir", help="build from a local copy of /dexed instead of reading the card")
    ap.add_argument("--port", help="serial port (default: auto-detect Teensy VID 16C0)")
    ap.add_argument("--baud", type=int, default=115200, help="nominal baud (CDC ignores it)")
    ap.add_argument("--out", help="also write the index to this local file "
                    "(default with --from-dir: <DIR>/../tdsp/.dxsearch)")
    push = ap.add_mutually_exclusive_group()
    push.add_argument("--push", action="store_true", help="push to the card over USB (default for device mode)")
    push.add_argument("--no-push", action="store_true", help="don't push; just write --out")
    ap.add_argument("--timeout", type=float, default=5.0, help="per-request reply timeout, seconds")
    args = ap.parse_args()

    skips = Skips()
    dev = None
    try:
        if args.from_dir:
            root = os.path.abspath(args.from_dir)
            if not os.path.isdir(root):
                sys.exit(f"--from-dir not found: {root}")
            print(f"Reading local library {root} ...")
            lines = walk_local(root, skips)
            out = args.out or os.path.join(os.path.dirname(root), "tdsp", ".dxsearch")
            do_push = args.push
        else:
            port = resolve_port(args.port)
            print(f"Reading /dexed from the card on {port} ...")
            dev = Device(port, args.baud)
            time.sleep(0.2)
            dev.reset()
            lines = walk_device(dev, args.timeout, skips)
            out = args.out
            do_push = not args.no_push

        long_lines = [l for l in lines if len(l) > MAX_LINE_BYTES]
        for l in long_lines:
            skips.add(l.split(b"\t", 1)[0].decode("utf-8", "replace"), "index line too long")
        lines = [l for l in lines if len(l) <= MAX_LINE_BYTES]
        data = b"".join(lines)
        print(f"\nIndexed {len(lines)} carts ({len(data) / 1024:.0f} KB).")
        skips.report()
        if not lines:
            sys.exit("Nothing indexed — is /dexed on the card (or --from-dir) populated?")

        if out:
            os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
            with open(out, "wb") as f:
                f.write(data)
            print(f"Wrote {out}")

        if do_push:
            if dev is None:
                port = resolve_port(args.port)
                dev = Device(port, args.baud)
                time.sleep(0.2)
            tmp = out
            if not tmp:
                tmp = os.path.join(os.environ.get("TEMP") or "/tmp", "t-dsp-dxsearch")
                with open(tmp, "wb") as f:
                    f.write(data)
            print(f"Pushing {DEV_INDEX_PATH} ...")
            ok = any(push_one(dev, tmp, DEV_INDEX_PATH, fid) for fid in (201, 202, 203))
            if not ok:
                sys.exit(f"Push FAILED — {DEV_INDEX_PATH} not written (CRC/transfer error).")
            print("Pushed + CRC-verified. Voice search is ready.")
        elif not args.from_dir and not out:
            print("--no-push without --out: nothing saved.", file=sys.stderr)
    finally:
        if dev is not None:
            dev.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
