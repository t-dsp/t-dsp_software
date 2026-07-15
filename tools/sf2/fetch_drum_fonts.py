#!/usr/bin/env python3
"""
fetch_drum_fonts.py — download drum-only SoundFonts, stage them, and copy to the SD card.

Companion to merge_drum_sf2.py / DRUM_FONTS.md. Three jobs:

  fetch   download the curated drum fonts, verify each is a real SF2, stage under
          tools/sf2/drum_fonts/downloads/ (git-ignored). Idempotent (size-checked).
  merge   graft each staged drum kit onto a base GM font (default TimGM6mb) via
          merge_drum_sf2.py, producing device-ready fonts under drum_fonts/merged/.
  to-sd   detect the removable/SD drive (or --drive X:) and copy staged fonts onto the
          card. --watch polls until a card is inserted, then copies. Archival drum
          fonts go to <sd>/sf2/drums_src/; a chosen merged font can be --activate'd to
          the live path <sd>/sf2/gm_tsf.sf2 (backing up the existing one first).

Downloads are VERIFIED against real, license-checked sources (see REGISTRY): archive.org
direct URLs and Google-Drive (usercontent + confirm token). Both paths were probed live.

Usage
-----
  python tools/sf2/fetch_drum_fonts.py                 # fetch the default (small, clean) set
  python tools/sf2/fetch_drum_fonts.py list            # registry + what's staged / on which drives
  python tools/sf2/fetch_drum_fonts.py fetch --all     # include the big 256 MB Perfect Drums
  python tools/sf2/fetch_drum_fonts.py fetch --only phattkit,ns_kit
  python tools/sf2/fetch_drum_fonts.py merge --base tools/sf2/fonts/gm_tim.sf2
  python tools/sf2/fetch_drum_fonts.py to-sd --watch                 # wait for card, copy raw kits
  python tools/sf2/fetch_drum_fonts.py to-sd --drive E: --activate ns_kit   # also set the live font

Windows-only for the SD auto-detect (uses ctypes GetDriveTypeW); fetch/merge are cross-platform.
"""
import sys, os, time, shutil, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import merge_drum_sf2 as M   # parser + merge engine + validation

STAGE   = os.path.join(HERE, "drum_fonts")
DL_DIR  = os.path.join(STAGE, "downloads")
MRG_DIR = os.path.join(STAGE, "merged")
UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/120 Safari/537.36"

# ---------------------------------------------------------------- the registry
# license notes:
#   wtfpl        = Do What The F You Want — unambiguous, product-safe.
#   pd-mark      = archive.org uploader marked Public Domain Mark 1.0. These are
#                  free-release fonts (fine for personal/dev use); confirm the
#                  author's terms before COMMERCIAL redistribution on the card.
REGISTRY = [
    dict(name="phattkit", size_hint=859082, license="pd-mark", drum_bank=0,
         kind="acoustic/electronic combo kit, tiny (0.8 MB) — good smoke-test",
         source=("archive", "https://archive.org/download/sf2soundfonts/phattkit.sf2"),
         default=True),
    dict(name="ns_kit", size_hint=23264256, license="pd-mark", drum_bank=None,
         kind="Natural Studio 'Douglas' acoustic kit V2.0 (~22 MB) — best free acoustic",
         source=("archive", "https://archive.org/download/sf2soundfonts/"
                            "Drums%20Douglas%20Natural%20Studio%20Kit%20V2.0%20%2822%2C719KB%29.sf2"),
         default=True),
    dict(name="perfect_drums", size_hint=256317998, license="wtfpl", drum_bank=128,
         kind="'Definitive Perfect Drums' V1 fixed-banks (256 MB!) — detailed, needs heavy "
              "downsample; cleanest license",
         source=("gdrive", "1KVa0HRgulxV01SdnWarto6rkEztf_fL0"),
         default=False),
    dict(name="perfect_drums_v2", size_hint=None, license="wtfpl", drum_bank=128,
         kind="Perfect Drums V2 (fixed hi-hats) — same size class as V1",
         source=("gdrive", "1GyacTR1iSo34bJuBxUEURyxADLBYpAyV"),
         default=False),
]
REG = {r["name"]: r for r in REGISTRY}


# ---------------------------------------------------------------- download
def _curl(url, out, extra=None):
    cmd = ["curl", "-fL", "-sS", "--retry", "3", "-A", UA, "-o", out, url]
    if extra:
        cmd[1:1] = extra
    return subprocess.call(cmd) == 0


def _download(rec, out):
    kind, loc = rec["source"]
    if kind == "archive":
        return _curl(loc, out)
    if kind == "gdrive":
        url = ("https://drive.usercontent.google.com/download?id=%s&export=download&confirm=t" % loc)
        return _curl(url, out)
    raise ValueError("unknown source kind %r" % kind)


def _verify(path):
    """Return (ok, msg). Confirms RIFF/sfbk and that our parser reads presets/banks."""
    try:
        with open(path, "rb") as fh:
            head = fh.read(12)
        if head[:4] != b"RIFF" or head[8:12] != b"sfbk":
            return False, "not a SoundFont (bad RIFF/sfbk magic — probably an HTML error page)"
        f = M.parse(path)
        banks = sorted({p["bank"] for _, p in M.preset_list(f)})
        npre = len(f["phdr"]) - 1
        return True, "%d presets, banks %s, %.1f MB smpl" % (npre, banks, len(f["pcm"]) / 1048576)
    except Exception as e:
        return False, "parse failed: %s" % e


def cmd_fetch(names, include_big):
    os.makedirs(DL_DIR, exist_ok=True)
    picked = names or [r["name"] for r in REGISTRY if (r["default"] or include_big)]
    for nm in picked:
        rec = REG.get(nm)
        if not rec:
            print("  ?? unknown font %r (see 'list')" % nm); continue
        out = os.path.join(DL_DIR, nm + ".sf2")
        if os.path.exists(out) and (rec["size_hint"] is None or os.path.getsize(out) == rec["size_hint"]):
            ok, msg = _verify(out)
            print("  = %-16s already staged (%s)" % (nm, msg if ok else "STALE: " + msg)); continue
        print("  v %-16s [%s] downloading %s ..." % (nm, rec["license"], rec["kind"]))
        tmp = out + ".part"
        if not _download(rec, tmp):
            print("    !! download failed. Manual: see DRUM_FONTS.md / the source URL."); continue
        ok, msg = _verify(tmp)
        if not ok:
            print("    !! %s (kept as %s for inspection)" % (msg, os.path.basename(tmp))); continue
        os.replace(tmp, out)
        print("    ok  %s  -> %s  (%s)" % (msg, os.path.relpath(out), rec["license"]))
    _write_gitignore()


def _write_gitignore():
    gi = os.path.join(STAGE, ".gitignore")
    if not os.path.exists(gi):
        os.makedirs(STAGE, exist_ok=True)
        open(gi, "w").write("# staged drum fonts + merged outputs are binaries — do not commit\n*\n!.gitignore\n")


# ---------------------------------------------------------------- merge
def _auto_drum_bank(rec, f):
    if rec.get("drum_bank") is not None:
        return rec["drum_bank"]
    banks = {p["bank"] for _, p in M.preset_list(f)}
    return M.DRUM_BANK if M.DRUM_BANK in banks else min(banks)  # 128 if present, else lowest


def cmd_merge(names, base_path):
    if not os.path.exists(base_path):
        sys.exit("base font not found: %s (build/point --base at a GM font, e.g. tools/sf2/fonts/gm_tim.sf2)" % base_path)
    os.makedirs(MRG_DIR, exist_ok=True)
    base = M.parse(base_path)
    base_tag = os.path.splitext(os.path.basename(base_path))[0]
    staged = names or [os.path.splitext(fn)[0] for fn in os.listdir(DL_DIR) if fn.endswith(".sf2")] if os.path.isdir(DL_DIR) else []
    if not staged:
        sys.exit("nothing staged — run 'fetch' first")
    for nm in staged:
        src = os.path.join(DL_DIR, nm + ".sf2")
        if not os.path.exists(src):
            print("  ?? %s not staged, skipping" % nm); continue
        drum = M.parse(src)
        db = _auto_drum_bank(REG.get(nm, {}), drum)
        out = os.path.join(MRG_DIR, "%s__%s.sf2" % (base_tag, nm))
        try:
            data = M.merge(base, drum, drum_bank=db, dest_bank=M.DRUM_BANK)
        except SystemExit as e:
            print("  !! %s: %s" % (nm, e)); continue
        open(out, "wb").write(data)
        ok, msg = _verify(out)
        print("  merged %-16s (drum bank %s -> 128)  %s  -> %s" % (nm, db, msg, os.path.relpath(out)))
    _write_gitignore()


# ---------------------------------------------------------------- SD detection / copy
def _removable_drives():
    """Windows: list (letter, has_tdsp) for DRIVE_REMOVABLE volumes. has_tdsp = looks like the card."""
    if os.name != "nt":
        return []
    import ctypes
    k = ctypes.windll.kernel32
    drives = []
    mask = k.GetLogicalDrives()
    for i in range(26):
        if not (mask >> i) & 1:
            continue
        letter = "%s:\\" % chr(ord("A") + i)
        dtype = k.GetDriveTypeW(ctypes.c_wchar_p(letter))
        if dtype == 2:   # DRIVE_REMOVABLE (SD readers usually enumerate here)
            has = os.path.isdir(os.path.join(letter, "sf2")) or os.path.isdir(os.path.join(letter, "songs"))
            drives.append((letter, has))
    return drives


def _pick_drive(explicit):
    if explicit:
        d = explicit.rstrip("\\/") + "\\"
        if not os.path.isdir(d):
            sys.exit("drive %s not present" % explicit)
        return d
    drives = _removable_drives()
    if not drives:
        return None
    # prefer a removable drive that already looks like the T-DSP card
    tdsp = [d for d, has in drives if has]
    if len(tdsp) == 1:
        return tdsp[0]
    if len(drives) == 1:
        return drives[0][0]
    print("  multiple removable drives — pass --drive X: :")
    for d, has in drives:
        print("    %s %s" % (d, "(looks like the T-DSP card: has /sf2 or /songs)" if has else ""))
    sys.exit(1)


def cmd_to_sd(drive, watch, activate, assume_yes):
    have = [fn for fn in sorted(os.listdir(DL_DIR))] if os.path.isdir(DL_DIR) else []
    raw = [fn for fn in have if fn.endswith(".sf2")]
    if not raw and not os.path.isdir(MRG_DIR):
        sys.exit("nothing staged — run 'fetch' (and maybe 'merge') first")

    target = _pick_drive(drive)
    if target is None and watch:
        print("  waiting for an SD card / removable drive (Ctrl-C to stop) ...")
        while target is None:
            time.sleep(2.0)
            target = _pick_drive(drive)
    if target is None:
        sys.exit("no removable drive found. Insert the card, or pass --drive X:. "
                 "(On Windows some readers show as 'fixed' — then --drive is required.)")

    print("  target card: %s" % target)
    # 1) archival copy of the raw drum fonts (kept as source material on the card)
    dst_src = os.path.join(target, "sf2", "drums_src")
    plan = [(os.path.join(DL_DIR, fn), os.path.join(dst_src, fn)) for fn in raw]
    # 2) optional: activate a merged font as the live /sf2/gm_tsf.sf2
    act_src = None
    if activate:
        cand = os.path.join(MRG_DIR, activate) if activate.endswith(".sf2") else None
        if not cand or not os.path.exists(cand):
            # accept a bare kit name -> newest merged font containing it
            matches = [fn for fn in (os.listdir(MRG_DIR) if os.path.isdir(MRG_DIR) else []) if activate in fn]
            if not matches:
                sys.exit("--activate %r: no merged font matches. Run 'merge' first, or pass a filename from %s"
                         % (activate, os.path.relpath(MRG_DIR)))
            cand = os.path.join(MRG_DIR, sorted(matches)[-1])
        act_src = cand

    print("  will copy:")
    for s, d in plan:
        print("    %s  ->  %s" % (os.path.basename(s), d))
    if act_src:
        live = os.path.join(target, "sf2", "gm_tsf.sf2")
        print("    %s  ->  %s   (LIVE font; existing backed up to gm_tsf.sf2.bak)"
              % (os.path.basename(act_src), live))
    if not assume_yes:
        try:
            if input("  proceed? [y/N] ").strip().lower() not in ("y", "yes"):
                print("  aborted."); return
        except EOFError:
            print("  non-interactive and no --yes; aborting to be safe."); return

    os.makedirs(dst_src, exist_ok=True)
    for s, d in plan:
        shutil.copy2(s, d); print("    copied %s" % os.path.basename(d))
    if act_src:
        live = os.path.join(target, "sf2", "gm_tsf.sf2")
        os.makedirs(os.path.dirname(live), exist_ok=True)
        if os.path.exists(live):
            shutil.copy2(live, live + ".bak"); print("    backed up existing gm_tsf.sf2 -> gm_tsf.sf2.bak")
        shutil.copy2(act_src, live); print("    ACTIVATED %s as /sf2/gm_tsf.sf2" % os.path.basename(act_src))
    print("  done. Boot the device and send 'T' over serial to sweep-test the kits.")


# ---------------------------------------------------------------- list
def cmd_list():
    print("Registry:")
    for r in REGISTRY:
        staged = os.path.join(DL_DIR, r["name"] + ".sf2")
        mark = "staged" if os.path.exists(staged) else "-"
        star = " (default)" if r["default"] else ""
        print("  %-16s %-8s %-7s %s%s\n      %s"
              % (r["name"], r["license"], mark, r["source"][0], star, r["kind"]))
    if os.path.isdir(MRG_DIR) and os.listdir(MRG_DIR):
        print("\nMerged (device-ready):")
        for fn in sorted(os.listdir(MRG_DIR)):
            print("  %s" % fn)
    print("\nRemovable drives:")
    for d, has in (_removable_drives() or []):
        print("  %s %s" % (d, "<- looks like the T-DSP card" if has else ""))
    if os.name != "nt":
        print("  (SD auto-detect is Windows-only; use --drive on other platforms)")


# ---------------------------------------------------------------- cli
def _opt(argv, name, default=None):
    return argv[argv.index(name) + 1] if name in argv else default


def main(argv):
    cmd = argv[0] if argv and not argv[0].startswith("-") else "fetch"
    rest = argv[1:] if (argv and not argv[0].startswith("-")) else argv
    only = _opt(rest, "--only")
    names = [x.strip() for x in only.split(",")] if only else None
    if cmd == "list":
        cmd_list()
    elif cmd == "fetch":
        cmd_fetch(names, include_big="--all" in rest)
        print(); cmd_list()
    elif cmd == "merge":
        cmd_merge(names, _opt(rest, "--base", os.path.join(HERE, "fonts", "gm_tim.sf2")))
    elif cmd == "to-sd":
        cmd_to_sd(_opt(rest, "--drive"), "--watch" in rest, _opt(rest, "--activate"), "--yes" in rest)
    else:
        print(__doc__); sys.exit(1)


if __name__ == "__main__":
    main(sys.argv[1:])
