#!/usr/bin/env python3
"""fetch_opll_patches.py -- build OPLL (YM2413) user-voice patch BANKS for the T-DSP SD card.

The YM2413 has 15 melodic voices in ROM and ONE programmable "user voice" = 8 register bytes
($00..$07). Every other OPLL timbre (the Yamaha PortaSound 100-voice sets, the VRC7 / YMF281B
chip variants, tracker presets, game soundtracks) is just a collection of those 8-byte sets.
This tool pulls them from their public sources and writes them as bank files the firmware reads
from the card (/opll/<Bank>.txt; see lib/TDspYmfm/src/OpllBank.h), one file = one browser folder.

Nothing it downloads is stored in the repo: the output folder is git-ignored. Re-run to refresh.

USAGE
    python tools/fetch_opll_patches.py plgdavid            # Yamaha PSS-140 / PSS-270 / SHS-10 rips
    python tools/fetch_opll_patches.py emu2413             # VRC7 + YMF281B ROM voice sets (emu2413)
    python tools/fetch_opll_patches.py furnace             # Furnace tracker's OPLL instrument presets
    python tools/fetch_opll_patches.py vgm <path|url> ...  # rip user voices from VGM game logs
    python tools/fetch_opll_patches.py all                 # plgdavid + emu2413 + furnace

    Common options: --out DIR (default firmware/mix-kit/assets/opll), --min-name-len N
    emu2413:  --include-ym2413   also write the stock YM2413 ROM set (normally skipped: it's in the chip)
    furnace:  --furnace-dir DIR  parse a local Furnace checkout's instruments/OPLL instead of GitHub
    vgm:      --bank-per-file    one bank per VGM file (default: one bank per GAME, from the GD3 tag)
              --on-write         also capture patches re-written WHILE a note sounds (live tweaks; noisy)

THEN PUSH TO THE CARD
    python tools/sync_assets.py --opll [--port COMx|/dev/ttyACM0]      # USB, verifies CRC
      ...or over Wi-Fi, one file at a time through the ESP32 tunnel:
    python projects/t-dsp_esp32_bt_receiver/tools/esp32_ota_wifi.py 192.168.4.1 \\
           firmware/mix-kit/assets/opll/PSS-140.txt --stage-only --path /opll/PSS-140.txt
    Reboot the Teensy (or power-cycle) -- banks are scanned at boot. Synth C's voice browser then
    shows a folder per bank.

OUTPUT FORMAT (what the firmware parses)
    # comments
    #name: Display Name                 (optional; default = file name without .txt)
    13 01 18 0F 9E 60 00 9F<TAB>Piano 1 (8 hex bytes = OPLL regs $00..$07, TAB, patch name)

LICENSING (read before sharing a card image)
    plgDavid's PortaSound rips are marked "copyrighted, please only use for study" -- personal use.
    emu2413 (MIT) ROM tables, Furnace presets (GPL/MIT, author-credited), and anything ripped from
    game VGMs carry their own terms; game data is the publisher's. Keep these to your own boxes.

Requires only the Python standard library (urllib, zipfile, gzip).
"""
import argparse, gzip, io, json, os, re, struct, sys, urllib.request, zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_OUT = os.path.join(REPO, "firmware", "mix-kit", "assets", "opll")
UA = {"User-Agent": "t-dsp-fetch-opll-patches/1.0"}


# ---------------------------------------------------------------------------------------------
def fetch(url, binary=False):
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=60) as r:
        data = r.read()
    return data if binary else data.decode("utf-8", "replace")


def clean_name(n, fallback="patch"):
    n = re.sub(r"\s+", " ", n or "").strip().strip('",;')
    return n or fallback


def write_bank(out_dir, file_stem, display, patches, source_note):
    """patches: list of (regs[8], name). Dedupes identical register sets (first name wins)."""
    os.makedirs(out_dir, exist_ok=True)
    seen, rows = set(), []
    for regs, name in patches:
        key = bytes(regs)
        if key in seen:
            continue
        seen.add(key)
        rows.append("%s\t%s" % (" ".join("%02X" % b for b in regs), clean_name(name)))
    if not rows:
        print("  (no patches, skipping %s)" % file_stem)
        return 0
    safe = re.sub(r'[\\/:*?"<>|]+', "-", file_stem).strip() or "bank"
    path = os.path.join(out_dir, safe + ".txt")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# OPLL (YM2413) user-voice bank: 8 hex bytes = regs $00..$07, TAB, name. Built by tools/fetch_opll_patches.py\n")
        f.write("# source: %s\n" % source_note)
        f.write("#name: %s\n" % display)
        f.write("\n".join(rows) + "\n")
    print("  wrote %-40s %4d patches" % (os.path.relpath(path, REPO), len(rows)))
    return len(rows)


# ---------------------------------------------------------------------------------------------
# plgDavid -- Yamaha PortaSound rips (logic-analyzer captures of the keyboard writing the user voice)
PLG_API = "https://api.github.com/repos/plgDavid/misc/contents/OPLL%20Synth%20Patches"
PLG_NAMES = {"pss140": ("PSS-140", "Yamaha PSS-140"), "pss270": ("PSS-270", "Yamaha PSS-270"),
             "shs10": ("SHS-10", "Yamaha SHS-10")}
HEX8_DOLLAR = re.compile(r"((?:\$[0-9A-Fa-f]{2}\s*){8})")
HEX8_0X = re.compile(r"\{\s*((?:0x[0-9A-Fa-f]{2}\s*,?\s*){8})\}\s*,?\s*(?://\s*(.*))?")


def parse_plg_text(text, names_text=None):
    """Handles all three plgDavid layouts: '$..x8' per line + separate names file (PSS-140),
    'Name   $..x8 [notes]' (PSS-270), and '{0x..,x8},//Name' C arrays (SHS-10)."""
    names = [l.rstrip() for l in names_text.splitlines() if l.strip()] if names_text else None
    out, idx = [], 0
    for line in text.splitlines():
        m = HEX8_0X.search(line)
        if m:
            regs = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1))]
            out.append((regs, m.group(2) or "patch %d" % (idx + 1))); idx += 1
            continue
        m = HEX8_DOLLAR.search(line)
        if not m:
            continue
        regs = [int(x, 16) for x in re.findall(r"\$([0-9A-Fa-f]{2})", m.group(1))]
        before, after = line[:m.start()].strip(), line[m.end():].strip()
        if names and idx < len(names):
            name = names[idx]
        elif before:
            name = re.sub(r"\(partial\)|\(.*?layer.*?\)", "", before, flags=re.I)
        else:
            name = after.split("//")[-1] if "//" in after else ("patch %d" % (idx + 1))
        out.append((regs, name)); idx += 1
    return out


def do_plgdavid(args):
    print("== plgDavid 'OPLL Synth Patches' ==")
    try:
        listing = json.loads(fetch(PLG_API))
        files = {e["name"]: e["download_url"] for e in listing if e.get("type") == "file"}
    except Exception as e:
        print("  GitHub listing failed (%s); falling back to the repo's study copy of the PSS-140 set" % e)
        files = {}
    total = 0
    if files:
        done = set()
        for fname, url in sorted(files.items()):
            if not fname.lower().endswith(".txt") or "names" in fname.lower() or fname in done:
                continue
            key = fname.split("_")[0].lower()
            stem, display = PLG_NAMES.get(key, (os.path.splitext(fname)[0], os.path.splitext(fname)[0]))
            names_url = None
            for cand in (fname.replace("_patches.txt", "_patches_names.txt"), fname.replace("_patches.txt", "_names.txt")):
                if cand in files and cand != fname:
                    names_url = files[cand]; done.add(cand); break
            try:
                text = fetch(url)
                names = fetch(names_url) if names_url else None
            except Exception as e:
                print("  %s: download failed (%s)" % (fname, e)); continue
            patches = parse_plg_text(text, names)
            total += write_bank(args.out, stem, display, patches, "plgDavid/misc 'OPLL Synth Patches' %s (study-only per author)" % fname)
    else:
        base = os.path.join(REPO, "lib", "TDspYmfm", "patches", "pss140")
        try:
            text = open(os.path.join(base, "pss140_patches.txt"), encoding="utf-8").read()
            names = open(os.path.join(base, "pss140_names.txt"), encoding="utf-8").read()
            total += write_bank(args.out, "PSS-140", "Yamaha PSS-140", parse_plg_text(text, names), "repo copy of plgDavid pss140 (offline fallback)")
        except Exception as e:
            print("  offline fallback failed too: %s" % e)
    return total


# ---------------------------------------------------------------------------------------------
# emu2413 -- the YM2413 / VRC7 / YMF281B ROM voice tables, as 19 rows x 8 bytes per chip
EMU_URL = "https://raw.githubusercontent.com/digital-sound-antiques/emu2413/master/emu2413.c"
EMU_SETS = ["YM2413", "VRC7", "YMF281B"]
VRC7_NAMES = ["Buzzy Bell", "Guitar", "Wurly", "Flute", "Clarinet", "Synth", "Trumpet", "Organ",
              "Bells", "Vibes", "Vibraphone", "Tutti", "Fretless", "Synth Bass", "Sweep"]   # nesdev wiki
# Community names for the YMF281B (OPLLP) set as documented by the MSX scene; treat as descriptive.
YMF281B_NAMES = ["Electric Strings", "Bow Wow", "Electric Guitar", "Organ", "Clarinet", "Saxophone",
                 "Trumpet", "Street Organ", "Synth Brass", "Electric Piano", "Bass", "Vibraphone",
                 "Chimes", "Tom Tom II", "Noise"]
YM2413_NAMES = ["Violin", "Guitar", "Piano", "Flute", "Clarinet", "Oboe", "Trumpet", "Organ", "Horn",
                "Synthesizer", "Harpsichord", "Vibraphone", "Synth Bass", "Acoustic Bass", "Electric Guitar"]


def do_emu2413(args):
    print("== emu2413 ROM voice tables ==")
    src = fetch(EMU_URL)
    m = re.search(r"default_inst\s*\[\s*OPLL_TONE_NUM\s*\]\s*\[[^\]]*\]\s*=\s*\{(.*?)\n\};", src, re.S)
    if not m:
        print("  could not locate default_inst[] in emu2413.c (format changed?)"); return 0
    bytes_ = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1))]
    per_set = 19 * 8
    nsets = len(bytes_) // per_set
    if nsets < 2 or len(bytes_) % per_set:
        print("  unexpected table size %d bytes" % len(bytes_)); return 0
    total = 0
    for si in range(nsets):
        name = EMU_SETS[si] if si < len(EMU_SETS) else "emu2413 set %d" % si
        if name == "YM2413" and not args.include_ym2413:
            print("  skipping YM2413 set (identical to the chip ROM the firmware already lists)"); continue
        rows = bytes_[si * per_set:(si + 1) * per_set]
        names = {"VRC7": VRC7_NAMES, "YMF281B": YMF281B_NAMES, "YM2413": YM2413_NAMES}.get(name, [])
        patches = []
        for i in range(1, 16):   # row 0 = user slot, rows 16..18 = rhythm
            regs = rows[i * 8:(i + 1) * 8]
            patches.append((regs, names[i - 1] if i - 1 < len(names) else "%s %d" % (name, i)))
        display = {"VRC7": "Konami VRC7 (ROM)", "YMF281B": "Yamaha YMF281B (ROM)", "YM2413": "YM2413 ROM (emu2413)"}.get(name, name)
        total += write_bank(args.out, name, display, patches, "digital-sound-antiques/emu2413 default_inst (MIT)")
    return total


# ---------------------------------------------------------------------------------------------
# Furnace -- .fui instrument files (FINS feature-block format), OPLL type = 13
FUR_API = "https://api.github.com/repos/tildearrow/furnace/contents/instruments/OPLL"


def parse_fui(data):
    """Returns (name, regs[8]) or (None, reason). Implements the FINS container; the FM feature's
    per-operator bit layout per papers/newIns.md. Instruments that just reference a ROM preset
    (opllPreset != 0) are skipped: they're not custom patches."""
    if len(data) < 8 or data[:4] != b"FINS":
        if data[:16] == b"-Furnace instr.-":
            return None, "old pre-FINS .fui format (open + re-save in Furnace to convert)"
        return None, "not a FINS file"
    ver, itype = struct.unpack_from("<HH", data, 4)
    if itype != 13:
        return None, "instrument type %d is not OPLL (13)" % itype
    pos, name, fm, has_macros = 8, "", None, False
    while pos + 4 <= len(data):
        code = data[pos:pos + 2]; ln = struct.unpack_from("<H", data, pos + 2)[0]; pos += 4
        blk = data[pos:pos + ln]; pos += ln
        if code == b"EN":
            break
        if code == b"NA":
            name = blk.split(b"\0", 1)[0].decode("utf-8", "replace")
        elif code == b"FM":
            fm = blk
        elif code == b"MA":
            has_macros = True
    if fm is None or len(fm) < 4 + 16:
        return None, "no FM block"
    alg_fb = fm[1]; fb = alg_fb & 7
    hdr = 5 if ver >= 224 else 4
    opbytes = bytes(fm[hdr:hdr + 16])
    # Furnace's stock OPLL instrument template. Presets in the repo mostly keep this default FM data
    # and make their sound with a "wave" macro that steps through the chip's ROM presets over time --
    # that is not an 8-byte patch, so there is nothing to convert.
    FURNACE_OPLL_TEMPLATE = bytes.fromhex("55282F0540330800" "51000F0140B60800")
    if opbytes == FURNACE_OPLL_TEMPLATE:
        return None, "default OPLL template%s (sound comes from ROM-preset macros, not a custom patch)" % (" + macros" if has_macros else "")
    if not any(opbytes):
        return None, "empty operator data (ROM preset reference)"
    ops = []
    for k in range(2):   # OPLL: op0 = modulator, op1 = carrier
        b = fm[hdr + k * 8: hdr + k * 8 + 8]
        if len(b) < 8:
            return None, "FM block truncated"
        ops.append(dict(
            ksr=(b[0] >> 7) & 1, mult=b[0] & 15,
            sus=(b[1] >> 7) & 1, tl=b[1] & 127,
            vib=(b[2] >> 5) & 1, ar=b[2] & 31,
            am=(b[3] >> 7) & 1, ksl=(b[3] >> 5) & 3, dr=b[3] & 31,
            egt=(b[4] >> 7) & 1,
            sl=(b[5] >> 4) & 15, rr=b[5] & 15,
            ssg=b[6] & 15, ws=b[7] & 7,
        ))
    mod, car = ops
    # OPLL "EG type" (sustained tone) bit: Furnace keeps it in the op's SUS flag for the OPL family
    # and some versions mirror it into SSG-EG bit 3; honour either.
    eg = lambda o: 1 if (o["sus"] or (o["ssg"] & 8) or o["egt"]) else 0
    r = [
        (mod["am"] << 7) | (mod["vib"] << 6) | (eg(mod) << 5) | (mod["ksr"] << 4) | mod["mult"],
        (car["am"] << 7) | (car["vib"] << 6) | (eg(car) << 5) | (car["ksr"] << 4) | car["mult"],
        (mod["ksl"] << 6) | (mod["tl"] & 63),
        (car["ksl"] << 6) | ((car["ws"] & 1) << 4) | ((mod["ws"] & 1) << 3) | fb,
        ((mod["ar"] & 15) << 4) | (mod["dr"] & 15),
        ((car["ar"] & 15) << 4) | (car["dr"] & 15),
        (mod["sl"] << 4) | mod["rr"],
        (car["sl"] << 4) | car["rr"],
    ]
    return name, r


def do_furnace(args):
    print("== Furnace OPLL instrument presets ==")
    patches, skipped = [], []
    if args.furnace_dir:
        d = os.path.join(args.furnace_dir, "instruments", "OPLL") if os.path.isdir(os.path.join(args.furnace_dir, "instruments")) else args.furnace_dir
        sources = [(f, open(os.path.join(d, f), "rb").read()) for f in sorted(os.listdir(d)) if f.lower().endswith(".fui")]
    else:
        listing = json.loads(fetch(FUR_API))
        sources = []
        for e in listing:
            if e.get("type") == "file" and e["name"].lower().endswith(".fui"):
                try:
                    sources.append((e["name"], fetch(e["download_url"], binary=True)))
                except Exception as ex:
                    skipped.append("%s: download failed (%s)" % (e["name"], ex))
    for fname, data in sources:
        name, regs = parse_fui(data)
        if name is None:
            skipped.append("%s: %s" % (fname, regs)); continue
        patches.append((regs, name or os.path.splitext(fname)[0].replace("_", " ").replace("%20", " ")))
    for s_ in skipped:
        print("  skip " + s_)
    return write_bank(args.out, "Furnace OPLL", "Furnace OPLL presets", patches,
                      "tildearrow/furnace instruments/OPLL (.fui -> OPLL regs; verify by ear)")


# ---------------------------------------------------------------------------------------------
# VGM game rips -- replay the YM2413 register stream, snapshot the user voice at every key-on that
# uses instrument 0 (and, with --on-write, every re-write of $00..$07 while a note sounds).
def vgm_gd3(data):
    try:
        off = struct.unpack_from("<I", data, 0x14)[0]
        if not off:
            return {}
        p = 0x14 + off
        if data[p:p + 4] != b"Gd3 ":
            return {}
        ln = struct.unpack_from("<I", data, p + 8)[0]
        s = data[p + 12:p + 12 + ln].decode("utf-16-le", "replace").split("\0")
        keys = ["track_en", "track_jp", "game_en", "game_jp", "system_en", "system_jp", "author_en", "author_jp", "date", "ripper", "notes"]
        return dict(zip(keys, s))
    except Exception:
        return {}


# command -> operand byte count (commands with fixed sizes)
VGM_FIXED = {}
VGM_FIXED.update({c: 1 for c in range(0x30, 0x40)})
VGM_FIXED.update({c: 2 for c in range(0x40, 0x4F)})
VGM_FIXED.update({0x4F: 1, 0x50: 1})
VGM_FIXED.update({c: 2 for c in range(0x51, 0x60)})
VGM_FIXED.update({0x61: 2, 0x62: 0, 0x63: 0, 0x64: 3})
VGM_FIXED.update({c: 0 for c in range(0x70, 0x90)})
VGM_FIXED.update({0x90: 4, 0x91: 4, 0x92: 5, 0x93: 10, 0x94: 1, 0x95: 4})
VGM_FIXED.update({c: 2 for c in range(0xA0, 0xC0)})
VGM_FIXED.update({c: 3 for c in range(0xC0, 0xE0)})
VGM_FIXED.update({c: 4 for c in range(0xE0, 0x100)})


def rip_vgm(data, on_write=False):
    if data[:2] == b"\x1f\x8b":
        data = gzip.decompress(data)
    if data[:4] != b"Vgm ":
        return None, []
    version = struct.unpack_from("<I", data, 0x08)[0]
    rel = struct.unpack_from("<I", data, 0x34)[0] if version >= 0x150 and len(data) > 0x38 else 0
    pos = 0x34 + rel if rel else 0x40
    regs = [0] * 0x40
    user = [0] * 8
    keyed = [False] * 9
    found = []   # ordered unique
    seen = set()

    def snap(tag):
        key = bytes(user)
        if key not in seen:
            seen.add(key); found.append((list(user), tag))

    n = len(data)
    while pos < n:
        c = data[pos]; pos += 1
        if c == 0x66:
            break
        if c == 0x67:   # data block: 0x66 tt ss ss ss ss <data>
            if pos + 6 > n: break
            size = struct.unpack_from("<I", data, pos + 2)[0] & 0x7FFFFFFF
            pos += 6 + size; continue
        if c == 0x68:   # PCM RAM write: 0x66 + 11 bytes
            pos += 12; continue
        if c == 0x51 and pos + 2 <= n:
            a, d = data[pos], data[pos + 1]
            if a < 8:
                user[a] = d
                if on_write and any(keyed[ch] and (regs[0x30 + ch] >> 4) == 0 for ch in range(9)):
                    snap("tweak")
            elif 0x20 <= a <= 0x28:
                ch = a - 0x20
                was, now = keyed[ch], bool(d & 0x10)
                keyed[ch] = now
                if now and not was and (regs[0x30 + ch] >> 4) == 0:
                    snap("keyon")
            if a < 0x40:
                regs[a] = d
            pos += 2; continue
        pos += VGM_FIXED.get(c, 0)
    return vgm_gd3(data), found


def iter_vgm_sources(spec):
    """Yields (label, bytes) for a path/dir/zip/url."""
    def from_zip(zb, label):
        with zipfile.ZipFile(io.BytesIO(zb)) as z:
            for nm in sorted(z.namelist()):
                if nm.lower().endswith((".vgm", ".vgz")):
                    yield "%s/%s" % (label, os.path.basename(nm)), z.read(nm)
    if re.match(r"^https?://", spec):
        blob = fetch(spec, binary=True)
        label = os.path.splitext(os.path.basename(spec.split("?")[0]))[0] or "download"
        if blob[:2] == b"PK":
            yield from from_zip(blob, label)
        else:
            yield label, blob
    elif os.path.isdir(spec):
        for root, _d, files in os.walk(spec):
            for f in sorted(files):
                p = os.path.join(root, f)
                if f.lower().endswith(".zip"):
                    yield from from_zip(open(p, "rb").read(), os.path.splitext(f)[0])
                elif f.lower().endswith((".vgm", ".vgz")):
                    yield "%s/%s" % (os.path.basename(root), f), open(p, "rb").read()
    elif spec.lower().endswith(".zip"):
        yield from from_zip(open(spec, "rb").read(), os.path.splitext(os.path.basename(spec))[0])
    else:
        yield os.path.basename(spec), open(spec, "rb").read()


def do_vgm(args):
    print("== VGM game rips ==")
    banks = {}   # bank stem -> (display, [(regs, name)], note)
    for spec in args.inputs:
        for label, blob in iter_vgm_sources(spec):
            gd3, found = rip_vgm(blob, on_write=args.on_write)
            if gd3 is None:
                print("  %s: not a VGM" % label); continue
            if not found:
                continue
            game = clean_name(gd3.get("game_en") or label.split("/")[0], "Unknown game")
            track = clean_name(gd3.get("track_en") or os.path.splitext(label.split("/")[-1])[0], "track")
            stem = ("Rip - " + (track if args.bank_per_file else game))[:60]
            disp, lst, _ = banks.setdefault(stem, (stem, [], "ripped from VGM logs (%s)" % game))
            for k, (regs, tag) in enumerate(found, 1):
                lst.append((regs, "%s %d" % (track[:22], k) if not args.bank_per_file else "patch %d" % k))
            print("  %-48s %3d patch(es)" % (label[:48], len(found)))
    total = 0
    for stem, (disp, lst, note) in sorted(banks.items()):
        total += write_bank(args.out, stem, disp, lst, note)
    return total


# ---------------------------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", choices=["plgdavid", "emu2413", "furnace", "vgm", "all"])
    ap.add_argument("inputs", nargs="*", help="vgm: files, folders, .zip packs or URLs")
    ap.add_argument("--out", default=DEFAULT_OUT, help="bank output folder (default firmware/mix-kit/assets/opll)")
    ap.add_argument("--include-ym2413", action="store_true", help="emu2413: also write the stock YM2413 ROM set")
    ap.add_argument("--furnace-dir", help="furnace: local Furnace checkout (or its instruments/OPLL dir)")
    ap.add_argument("--bank-per-file", action="store_true", help="vgm: one bank per VGM file instead of per game")
    ap.add_argument("--on-write", action="store_true", help="vgm: also capture patches re-written while a note sounds")
    args = ap.parse_args()
    if args.source == "vgm" and not args.inputs:
        ap.error("vgm needs at least one VGM file / folder / zip / URL")
    total = 0
    if args.source in ("plgdavid", "all"): total += do_plgdavid(args)
    if args.source in ("emu2413", "all"):  total += do_emu2413(args)
    if args.source in ("furnace", "all"):  total += do_furnace(args)
    if args.source == "vgm":               total += do_vgm(args)
    print("done: %d patches in %s" % (total, os.path.relpath(args.out, REPO)))
    print("push: python tools/sync_assets.py --opll   (then reboot the Teensy; banks load at boot)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
