#!/usr/bin/env python3
"""build_mars_kits.py — turn "…From Mars" one-shot packs into a TSF drum SoundFont.

The mix-kit renders channel-10 grooves through whatever bank-128 kit lives in the SF2
the TSF drum engine loads (DrumTsf.h -> /sf2/drumkits.sf2). fetch_drumkits.py already
builds that font from *acoustic SFZ kits*; this is its sibling for the local "Drums
From Mars" machine packs (808/909/606/…): the same output format, a different source.

Per pack we pick ONE representative one-shot per GM percussion note (by FILENAME, since
the packs' folder layouts are wildly inconsistent), normalize to 48 kHz/16-bit mono, and
bake each pack (optionally each tone-variant) as its own bank-128 PRESET into

    /sf2/drumkits.sf2

so ch10 program-change selects the kit — zero firmware change (DrumTsf prefers this file).

PSRAM: TSF loads the whole font into RAM, so the SF2 path needs a PSRAM board. To also
feed a future no-PSRAM SD-streaming sampler, every chosen one-shot is ALSO written to
    /drums/<kit>/<gmnote>-<piece>.wav   (stereo, 48k/16)   [--emit-oneshots, default on]
under the staging dir, ready to push to the card.

Reads WAVs straight out of the .zip packs (no 36 GB extraction). Incomplete Chrome
downloads (*.crdownload) are ignored.

Usage
-----
  python tools/build_mars_kits.py --list                       # what packs are present
  python tools/build_mars_kits.py --packs 808,909              # two kits -> staging, validated
  python tools/build_mars_kits.py --all                        # every complete pack
  python tools/build_mars_kits.py --packs 909 --split-variants # 909 Clean/Tube/Tape/… as separate kits
  python tools/build_mars_kits.py --all --push --port COM4      # build + stream to the card
"""
from __future__ import annotations

import argparse
import io
import os
import re
import sys
import zipfile

import numpy as np
import soundfile as sf

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(REPO_ROOT, "tools"))
import fetch_drumkits as F  # reuse: load_normalize, to_int16, build_sf2, validate, push, catalog

MARS_DIR_DEFAULT = r"C:\ALL THE DRUMS FROM MARS"
OUT_RATE = F.OUT_RATE  # 48000

# --- GM percussion classifier ------------------------------------------------
# Ordered (regex, gm_note, canonical). FIRST match wins, so put specific before generic:
# "open hh"/"closed hh" before bare "hat", "low tom" before "tom", "ride bell" before "ride".
# Word boundaries keep "BD"/"SD"/"CH"/"OH" from matching inside longer words.
_CLASSIFY = [
    (r"\b(pedal|foot)\b.*\b(hh|hat)\b|\bpedal\s*hh\b", 44, "Pedal Hi-Hat"),
    (r"\b(open|oh)\b.*\b(hh|hat)\b|\bopen\s*hh\b|\boh\b", 46, "Open Hi-Hat"),
    (r"\b(closed|clsd|ch)\b.*\b(hh|hat)\b|\bclosed\s*hh\b|\bch\b", 42, "Closed Hi-Hat"),
    (r"\b(hi\s*hat|hihat|hh)\b", 42, "Closed Hi-Hat"),          # bare hat -> closed
    (r"\bbass\s*drum\b|\bkick\b|\bbd\b", 36, "Bass Drum"),
    (r"\bsnare\b|\bsd\b|\bsnr\b", 38, "Snare"),
    (r"\brim\s*shot\b|\brimshot\b|\brim\b|\bstick\b|\bxstick\b", 37, "Side Stick"),
    (r"\b(hand\s*)?clap\b|\bclp\b", 39, "Hand Clap"),
    (r"\b(lo|low)\b.*\btom\b|\btom\s*(lo|low|1)\b", 45, "Low Tom"),
    (r"\b(mid|med)\b.*\btom\b|\btom\s*(mid|med|2)\b", 47, "Mid Tom"),
    (r"\b(hi|high)\b.*\btom\b|\btom\s*(hi|high|3)\b", 50, "High Tom"),
    (r"\btom\b", 47, "Mid Tom"),                                # generic tom
    (r"\b(lo|low)\b.*\bconga\b", 64, "Low Conga"),
    (r"\b(mid|hi|high|open|mute)\b.*\bconga\b|\bconga\b", 63, "Hi Conga"),
    (r"\b(hi|high)\b.*\bbongo\b|\bbongo\s*hi\b", 60, "Hi Bongo"),
    (r"\b(lo|low)\b.*\bbongo\b|\bbongo\b", 61, "Low Bongo"),
    (r"\bcowbell\b|\bcow\s*bell\b|\bcb\b", 56, "Cowbell"),
    (r"\bclave|\bclav\b", 75, "Claves"),
    (r"\bmaraca", 70, "Maracas"),
    (r"\bshaker\b|\bcabasa\b|\bcaba\b", 69, "Cabasa"),
    (r"\btambourine\b|\btamb\b", 54, "Tambourine"),
    (r"\b(hi|high)\b.*\bagogo\b|\bagogo\b", 67, "Hi Agogo"),
    (r"\b(lo|low)\b.*\bagogo\b", 68, "Low Agogo"),
    (r"\b(hi|high)\b.*\btimbale\b|\btimbale\b", 65, "Hi Timbale"),
    (r"\b(lo|low)\b.*\btimbale\b", 66, "Low Timbale"),
    (r"\bwood\s*block\b|\bwoodblock\b", 76, "Wood Block"),
    (r"\btriangle\b", 81, "Open Triangle"),
    (r"\bcrash\b", 49, "Crash Cymbal"),
    (r"\bchina\b", 52, "Chinese Cymbal"),
    (r"\bsplash\b", 55, "Splash Cymbal"),
    (r"\bride\b.*\b(bell|cup)\b|\bcup\b", 53, "Ride Bell"),
    (r"\bride\b", 51, "Ride Cymbal"),
    (r"\bcymbal\b|\bcym\b|\bcy\b", 49, "Crash Cymbal"),         # generic cymbal -> crash
]
_CLASSIFY = [(re.compile(rx, re.I), n, nm) for rx, n, nm in _CLASSIFY]

# Recognized tone-variant tokens (for --split-variants). Grouped so alternates collapse.
_VARIANTS = [
    ("Clean", r"\bclean\b"), ("Tube", r"\btube\b"), ("Tape", r"\btape\b"),
    ("Color", r"\bcolor\b|\bcolour\b"), ("Dirt", r"\bdirt\b|\bdirty\b"),
    ("Distorted", r"\bdist(orted)?\b|\bclip(ped)?\b|\bsat(urat\w*)?\b"),
    ("Vinyl", r"\bvinyl\b|\blofi\b|\blo-?fi\b"), ("Modified", r"\bmodif\w*\b|\bmod\b"),
    ("Processed", r"\bprocess\w*\b|\bproc\b"), ("SP1200", r"\bsp\s*-?1200\b|\bsp12\b"),
    ("MPC60", r"\bmpc\s*-?60\b"), ("MP", r"\bmp\b"), ("Warm", r"\bwarm\b"),
]
_VARIANTS = [(name, re.compile(rx, re.I)) for name, rx in _VARIANTS]

# path tokens that mark a NON-one-shot (loops, kits, demos, DAW project audio)
_SKIP_PATH = re.compile(
    r"__macosx|/\._|\.ds_store|loop|/beats?/|groove|\bfill\b|pattern|construction|"
    r"\bdemo\b|\bkit\b\.adg|reason|maschine|kontakt|ableton|logic|\.exs|\.nki|\.sxt",
    re.I)
# path tokens that POSITIVELY mark a one-shot area (used only to prefer, if present)
_ONESHOT_PREF = re.compile(r"individual\s*hits|/hits/|one.?shots?|single\s*hits?", re.I)

# tokens in a filename that make a hit a WORSE representative (want the plain, short hit)
_BAD_TOKENS = re.compile(r"\baccent|\bacc\b|\broll\b|\bflam\b|\bghost\b|\blong\b|\brev(erse)?\b|\bfx\b|\bnoise\b", re.I)
_GOOD_TOKENS = re.compile(r"\bshort\b|\bclean\b|\btight\b|\bnice\b|\ba\s*0?1\b", re.I)


def classify(path: str):
    """Return (gm_note, canonical_name) for a WAV path, or None if it isn't a GM piece."""
    base = os.path.basename(path)
    for rx, note, name in _CLASSIFY:
        if rx.search(base):
            return note, name
    return None


def variant_of(path: str) -> str:
    for name, rx in _VARIANTS:
        if rx.search(path):
            return name
    return "Main"


def pack_display(zip_name: str) -> str:
    """808-from-mars.zip -> '808 From Mars'; 909_from_mars -> '909 From Mars'."""
    stem = re.sub(r"\.zip$", "", zip_name, flags=re.I)
    stem = re.sub(r"_legacy$", " Legacy", stem, flags=re.I)
    words = re.split(r"[-_ ]+", stem)
    return " ".join(w.capitalize() if w.isalpha() else w for w in words if w)


def pack_shortname(zip_name: str) -> str:
    """A short, filesystem-safe id for the pack: the stem before '…from mars'.
    '808-from-mars.zip'->'808', 'lm1_from_mars'->'lm1', 'lo-fi_drum_machines…'->'lo_fi_drum_machines'."""
    stem = re.sub(r"\.zip$", "", zip_name, flags=re.I)
    stem = re.split(r"[-_ ]from[-_ ]mars", stem, flags=re.I)[0]
    safe = re.sub(r"[^A-Za-z0-9]+", "_", stem).strip("_").lower()
    return safe or re.sub(r"\.zip$", "", zip_name, flags=re.I).lower()


# Order of importance for --per-pack (matched as substrings of pack_shortname). The iconic
# machines first, then notable/high-piece packs; anything unlisted sorts after, by piece count.
PRIORITY = [
    "808", "909", "linndrum", "lm1", "linn60", "dmx", "drumulator", "606", "707", "727",
    "626", "505", "dr_bohm", "jupiter", "junos", "lo_fi_drum_machines", "vinyl_drum_machines",
    "vinyl_drums", "vinyl_sp", "cassette_drums", "drumtrax", "lindrum", "dr_sample", "s950",
    "sample_journal", "soviet_synths", "viscount", "wendel", "ekko", "tom", "indie_tapes",
    "perkons", "dmx", "sp_909", "essential_wav_16bit",
]


def priority_rank(zip_name: str) -> int:
    short = pack_shortname(zip_name)
    for i, kw in enumerate(PRIORITY):
        if kw in short:
            return i
    return len(PRIORITY)


# --- pack scanning -----------------------------------------------------------
def list_packs(mars_dir: str):
    """Complete one-shot packs only: *.zip, skip *.crdownload and the big loop packs."""
    out = []
    for fn in sorted(os.listdir(mars_dir)):
        if not fn.lower().endswith(".zip"):
            continue
        if re.search(r"loops?", fn, re.I):        # loop packs handled in a later phase
            continue
        out.append(fn)
    return out


def scan_pack(zip_path: str):
    """List (member, gm_note, name, variant) for every classifiable one-shot WAV in a zip."""
    hits = []
    with zipfile.ZipFile(zip_path) as zf:
        names = [n for n in zf.namelist() if n.lower().endswith(".wav")]
    names = [n for n in names if not _SKIP_PATH.search(n)]
    pref = [n for n in names if _ONESHOT_PREF.search(n)]
    pool = pref if pref else names            # prefer explicit one-shot areas when present
    for n in pool:
        c = classify(n)
        if c:
            hits.append((n, c[0], c[1], variant_of(n)))
    return hits


def _score(member: str):
    """Lower is a better representative hit."""
    b = os.path.basename(member)
    s = 0
    if _BAD_TOKENS.search(b):
        s += 10
    if not _GOOD_TOKENS.search(b):
        s += 2
    # prefer the first indexed take (…A 01) and shorter names
    m = re.search(r"(\d+)\s*\.wav$", b)
    if m:
        s += min(int(m.group(1)), 9) * 0.1
    return (s, len(b), b.lower())


def pick_representatives(hits, prefer_variant: str | None):
    """From [(member,note,name,variant)], pick ONE member per GM note.
    If prefer_variant is set, only that variant's hits are considered (fallback to any)."""
    by_note: dict[int, list] = {}
    for member, note, name, var in hits:
        if prefer_variant and var != prefer_variant:
            continue
        by_note.setdefault(note, []).append((member, name))
    if prefer_variant and not by_note:            # variant had nothing -> fall back to all
        for member, note, name, var in hits:
            by_note.setdefault(note, []).append((member, name))
    chosen = []
    for note in sorted(by_note):
        member, name = min(by_note[note], key=lambda mn: _score(mn[0]))
        chosen.append((note, name, member))
    return chosen


# --- build one kit (bank-128 preset) from chosen members ---------------------
def build_kit(zip_path, display, program, chosen, emit_dir, kit_key):
    """Extract+normalize each chosen hit; return a fetch_drumkits-style kit dict.
    Also writes stereo one-shots to emit_dir/<kit_key>/ for the future SD sampler."""
    mono_f, meta = [], []
    stereo_cache = {}
    kit_peak = 1e-9
    note_peak = {}                       # per-GM-note peak, for kick-anchored normalization
    with zipfile.ZipFile(zip_path) as zf:
        for note, name, member in chosen:
            data = zf.read(member)
            x, sr = sf.read(io.BytesIO(data), dtype="float64", always_2d=True)
            # write a temp file so we can reuse fetch_drumkits.load_normalize (trim/resample)
            tmp = os.path.join(emit_dir, "_tmp.wav")
            sf.write(tmp, x, sr, subtype="PCM_16")
            mono, stereo = F.load_normalize(tmp)
            p = max(float(np.max(np.abs(mono))), float(np.max(np.abs(stereo))))
            kit_peak = max(kit_peak, p)
            note_peak[note] = max(note_peak.get(note, 0.0), p)
            mono_f.append(mono)
            stereo_cache[note] = stereo
            meta.append(dict(note=note, name=name))
    os.remove(os.path.join(emit_dir, "_tmp.wav")) if os.path.exists(os.path.join(emit_dir, "_tmp.wav")) else None

    # Normalization: per-kit *kick-anchored* gain, not per-kit *peak*. Grooves are kick-driven,
    # so anchoring the kick (GM 36, fallback 35) to KICK_TARGET makes perceived loudness
    # consistent across kits — whereas peak-normalizing lets a loud crash/outlier under-normalize
    # the kick, so 909 (big open crash) played ~11 dB quieter than 808 under peak-norm.
    # One gain per kit preserves the kit's internal balance; a clip guard keeps the loudest
    # sample under PEAK_CEIL so kits whose crash is louder than the kick still don't clip.
    KICK_TARGET = 0.70                    # kick lands ~-3 dBFS
    PEAK_CEIL   = 0.985                   # nothing in the kit exceeds this
    kick_peak = note_peak.get(36) or note_peak.get(35)
    if kick_peak and kick_peak > 1e-6:
        gain = min(KICK_TARGET / kick_peak, PEAK_CEIL / kit_peak)
    else:
        gain = F.PEAK_TARGET / kit_peak   # no kick in this kit -> fall back to peak-norm
    kit_out = os.path.join(emit_dir, kit_key)
    os.makedirs(kit_out, exist_ok=True)
    hits = []
    for i, m in enumerate(meta):
        mono16 = F.to_int16(mono_f[i], gain)
        stereo16 = F.to_int16(stereo_cache[m["note"]], gain)
        sf.write(os.path.join(kit_out, "%d-%s.wav" % (m["note"], re.sub(r"[^A-Za-z0-9]+", "_", m["name"]))),
                 stereo16, OUT_RATE, subtype="PCM_16")
        excl = 1 if m["note"] in F.GM_HIHAT_NOTES else None
        hits.append(dict(note=m["note"], name=m["name"], excl=excl, mono16=mono16))
    return dict(name=kit_key, display=display, program=program,
                license="Drums From Mars (free pack) — personal use",
                attribution="Sampled from a '…From Mars' pack (goldbaby/from-mars, free).",
                hits=hits)


def write_kit_tsv(out_dir: str, basename: str, built) -> str:
    """Per-font kit list, same columns loadDrumKitsTsv() expects (drumkits.tsv format)."""
    path = os.path.join(out_dir, basename + ".tsv")
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# program\tname\tlicense\tpieces\tdisplay\n")
        for k in built:
            fh.write("%d\t%s\t%s\t%d\t%s\n" %
                     (k["program"], k["name"], k["license"], len(k["hits"]), k["display"]))
    return path


def emit_font(built, out_dir, basename, do_validate):
    """Assemble <basename>.sf2 (+ .tsv), structural-check, optional tsf.h render.
    Returns {path, tsv, bytes, kits, display} or None on structural failure."""
    sf2_bytes = F.build_sf2(built)
    path = os.path.join(out_dir, basename + ".sf2")
    with open(path, "wb") as fh:
        fh.write(sf2_bytes)
    errs = F.structural_check(sf2_bytes, built)
    if errs:
        for e in errs:
            print("    [validate] ERROR " + e)
        return None
    tsv = write_kit_tsv(out_dir, basename, built)
    if do_validate:
        F.render_check(path, built, out_dir)
    return dict(path=path, tsv=tsv, bytes=len(sf2_bytes), kits=len(built),
                display=built[0]["display"])


def build_pack_kits(zip_path, display0, short, hits, split_variants, min_pieces, drums_dir):
    """Build the list of kit dicts for one pack (variants -> programs when split_variants)."""
    variants = sorted({h[3] for h in hits}) if split_variants else [None]
    if split_variants and variants == ["Main"]:
        variants = [None]
    kits, program = [], 0
    for var in variants:
        chosen = pick_representatives(hits, var)
        if len(chosen) < min_pieces:
            continue
        disp = display0 if var in (None, "Main") else "%s %s" % (short.upper(), var)
        key = "%s_%s" % (short, (var or "main").lower())
        kits.append(build_kit(zip_path, disp, program, chosen, drums_dir, key))
        program += 1
    return kits


def main() -> int:
    ap = argparse.ArgumentParser(description="Build /sf2/drumkits.sf2 from 'Drums From Mars' packs.")
    ap.add_argument("--mars-dir", default=MARS_DIR_DEFAULT, help="folder holding the *.zip packs")
    ap.add_argument("--packs", help="comma list by number/name (e.g. 808,909); default: all complete")
    ap.add_argument("--all", action="store_true", help="every complete non-loop pack")
    ap.add_argument("--per-pack", action="store_true",
                    help="build one SWAPPABLE /sf2/mars_<pack>.sf2 per pack + a fonts.tsv manifest "
                         "(for @DRUMFONT runtime swap); default drumkits.sf2 = top-priority pack")
    ap.add_argument("--split-variants", action="store_true",
                    help="each detected tone variant (Clean/Tube/Tape/…) becomes its own kit")
    ap.add_argument("--min-pieces", type=int, default=4, help="skip a kit with fewer GM pieces")
    ap.add_argument("--out", default="c:/tmp/t-dsp-drumkits", help="staging dir")
    ap.add_argument("--no-validate", action="store_true", help="skip the tsf.h render check")
    ap.add_argument("--push", action="store_true", help="stream drumkits.sf2 to /sf2 over USB")
    ap.add_argument("--port", default="COM4", help="serial port for --push")
    ap.add_argument("--list", action="store_true", help="list complete packs (with piece counts) and exit")
    args = ap.parse_args()

    all_packs = list_packs(args.mars_dir)
    if args.list:
        print("Complete one-shot packs in %s:" % args.mars_dir)
        for fn in all_packs:
            hits = scan_pack(os.path.join(args.mars_dir, fn))
            notes = sorted({h[1] for h in hits})
            vars_ = sorted({h[3] for h in hits})
            print("  %-26s %2d GM pieces  variants: %s" %
                  (fn, len(notes), ", ".join(vars_) if vars_ else "(none)"))
        return 0

    if args.packs:
        want = [p.strip().lower() for p in args.packs.split(",")]
        packs = [fn for fn in all_packs if any(w in pack_shortname(fn) or w in fn.lower() for w in want)]
    else:
        packs = all_packs        # --all or default
    if not packs:
        sys.exit("no matching complete packs (see --list)")

    packs = sorted(packs, key=lambda fn: (priority_rank(fn), fn.lower()))

    os.makedirs(args.out, exist_ok=True)
    drums_dir = os.path.join(args.out, "drums")           # /drums/<kit>/ one-shot archive
    os.makedirs(drums_dir, exist_ok=True)

    # ---- per-pack: one swappable /sf2/mars_<pack>.sf2 each + a fonts.tsv manifest ----
    if args.per_pack:
        import shutil
        manifest = []   # (sd_path, display, kits, bytes, basename)
        for fn in packs:
            zip_path = os.path.join(args.mars_dir, fn)
            display0, short = pack_display(fn), pack_shortname(fn)
            hits = scan_pack(zip_path)
            if not hits:
                continue
            kits = build_pack_kits(zip_path, display0, short, hits,
                                   args.split_variants, args.min_pieces, drums_dir)
            if not kits:
                continue
            base = "mars_%s" % short
            info = emit_font(kits, args.out, base, not args.no_validate)
            if not info:
                print("  [%s] structural check failed — skipped" % display0); continue
            mb = info["bytes"] / 1048576
            flag = "  [>6MB: 16MB-board only]" if mb > 6.0 else ""
            print("  /sf2/%s.sf2  %.2f MB  %d kit(s)  %s%s" % (base, mb, info["kits"], display0, flag))
            manifest.append(("/sf2/%s.sf2" % base, display0, info["kits"], info["bytes"], base))
        if not manifest:
            sys.exit("no fonts built.")
        top = manifest[0]                          # top-priority pack = the boot default
        shutil.copyfile(os.path.join(args.out, top[4] + ".sf2"), os.path.join(args.out, "drumkits.sf2"))
        shutil.copyfile(os.path.join(args.out, top[4] + ".tsv"), os.path.join(args.out, "drumkits.tsv"))
        mpath = os.path.join(args.out, "fonts.tsv")
        with open(mpath, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("# path\tdisplay\tkits\tbytes\n")
            for p, disp, k, b, _base in manifest:
                fh.write("%s\t%s\t%d\t%d\n" % (p, disp, k, b))
        total = sum(m[3] for m in manifest) / 1048576
        print("\n[per-pack] %d font(s), %.1f MB total on card. manifest -> %s" % (len(manifest), total, mpath))
        print("  boot default /sf2/drumkits.sf2 = %s (%.2f MB)" % (top[1], top[3] / 1048576))
        big = [m[0] for m in manifest if m[3] / 1048576 > 6.0]
        if big:
            print("  [PSRAM] exceed the 8MB-board ~6MB ceiling (won't load on jay-mint): " + ", ".join(big))
        if args.push:
            files = []
            for p, _disp, _k, _b, base in manifest:
                files.append((os.path.join(args.out, base + ".sf2"), p))
                files.append((os.path.join(args.out, base + ".tsv"), p[:-4] + ".tsv"))
            files += [(os.path.join(args.out, "drumkits.sf2"), "/sf2/drumkits.sf2"),
                      (os.path.join(args.out, "drumkits.tsv"), "/sf2/drumkits.tsv"),
                      (mpath, "/sf2/fonts.tsv")]
            F.push(files, args.port)
        else:
            print("\nDone. Push with: python tools/build_mars_kits.py --per-pack --push --port <PORT>")
        return 0

    built = []
    program = 0
    for fn in packs:
        zip_path = os.path.join(args.mars_dir, fn)
        display0 = pack_display(fn)
        short = pack_shortname(fn)
        hits = scan_pack(zip_path)
        if not hits:
            print("[%s] no classifiable one-shots — skipped" % display0)
            continue
        variants = sorted({h[3] for h in hits}) if args.split_variants else [None]
        # Main/None first, and only split when >1 real variant exists
        if args.split_variants and variants == ["Main"]:
            variants = [None]
        for var in variants:
            chosen = pick_representatives(hits, var)
            if len(chosen) < args.min_pieces:
                continue
            disp = display0 if var in (None, "Main") else "%s %s" % (short.upper(), var)
            key = short if var in (None, "Main") else "%s_%s" % (short, var.lower())
            print("[%s] program %d: %d GM pieces%s" %
                  (disp, program, len(chosen), "" if var in (None, "Main") else "  (variant %s)" % var))
            if program > 127:
                print("  !! 128-preset limit reached; stopping"); break
            built.append(build_kit(zip_path, disp, program, chosen, drums_dir, key))
            program += 1
        if program > 127:
            break
    if not built:
        sys.exit("no kits built (try without --split-variants, or lower --min-pieces).")

    print("[build] assembling drumkits.sf2 (%d preset(s)) …" % len(built))
    sf2_bytes = F.build_sf2(built)
    sf2_path = os.path.join(args.out, "drumkits.sf2")
    with open(sf2_path, "wb") as fh:
        fh.write(sf2_bytes)
    mb = len(sf2_bytes) / 1048576
    print("    wrote %s (%.2f MB, %d kit(s))" % (sf2_path, mb, len(built)))
    # TSF loads the whole font into PSRAM. Warn before it can't fit alongside firmware
    # working RAM. Rough usable ceilings: ~6 MB on an 8 MB board, ~14 MB on a 16 MB board
    # (leave the rest for a melodic engine's buffers). No-PSRAM boards can't load it at all.
    if mb > 14.0:
        print("  [PSRAM] %.1f MB — too big even for a 16 MB board. Trim kits/variants." % mb)
    elif mb > 6.0:
        print("  [PSRAM] %.1f MB — fits a 16 MB board; TOO BIG for 8 MB. Use fewer kits for the 8 MB Teensy." % mb)
    else:
        print("  [PSRAM] %.1f MB — fits both your 8 MB and 16 MB boards. (No-PSRAM board needs the Phase-2 SD sampler.)" % mb)

    errs = F.structural_check(sf2_bytes, built)
    if errs:
        for e in errs:
            print("  [validate] ERROR " + e)
        sys.exit("aborting: SF2 failed structural check.")
    print("  [validate] structural check OK")
    if not args.no_validate:
        if F.render_check(sf2_path, built, args.out):
            print("  [validate] tsf.h render OK — every kit sounds.")

    cat = F.write_catalog(args.out, built)
    cred = F.write_credits(args.out, built)
    print("    catalog -> %s\n    credits -> %s\n    one-shots -> %s (for future no-PSRAM SD sampler)"
          % (cat, cred, drums_dir))

    if args.push:
        F.push([(sf2_path, "/sf2/drumkits.sf2"), (cat, "/sf2/drumkits.tsv"),
                (cred, "/sf2/drumkits_CREDITS.md")], args.port)
    else:
        print("\nDone. Copy %s to the card as /sf2/drumkits.sf2 (or re-run with --push)." % sf2_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
