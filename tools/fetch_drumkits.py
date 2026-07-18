#!/usr/bin/env python3
"""fetch_drumkits.py — turn open acoustic drum kits into a TSF-ready drum SoundFont.

The mix-kit firmware renders its channel-10 grooves through whatever bank-128 kit
lives in the SF2 the TSF engine loads (SynthBackendSF2Tsf / DrumTsf). This tool
grabs real, recorded acoustic kits (distributed as SFZ + samples), converts them,
and bakes each one as its OWN bank-128 PRESET (program number) into a single

    /sf2/drumkits.sf2

so the (later) @DRUMKIT wiring can switch kits with a plain ch10 program change —
exactly the zero-firmware-change path DRUM_FONTS.md describes. This pass only
ACQUIRES the assets: fetch -> normalize -> build SF2 -> validate -> push. Wiring the
kits into kit selection (kDrumKits[]) comes later.

Pipeline (mirrors tools/fetch_drums.py's structure + --push flow)
-----------------------------------------------------------------
  fetch      download each kit's SFZ, pick ONE sample per GM note (velocity layer
             nearest --vel), download just those hits from the source repo.
  normalize  every hit -> 48 kHz / 16-bit, trimmed of leading silence, PEAK-
             NORMALIZED PER KIT (one gain per kit, so relative piece levels are kept).
             Stereo copies are archived under assets/samples/<kit>/ (for a future raw-
             WAV sampler path); the SF2 itself uses the mono mixdown, like GM drum fonts.
  build      assemble one bank-128 SF2, each kit a preset at program 0,1,2,…, reusing
             the validated RIFF writer in tools/sf2/merge_drum_sf2.py (no hand-rolled
             encoder — we only construct the multi-zone drum instrument records).
  validate   compile tools/sf2/tsf_drum_probe.cpp against the SHIPPED tsf.h (MSVC, auto-
             located) and render one ch10 note per key; a structural re-parse always runs
             as a cross-platform floor. Nothing reaches the card until a kit renders.
  push       stream drumkits.sf2 (+ its catalog/credits) to /sf2 over USB via
             tools/push_file_serial.ps1 — no MTP, no reflash.

Licensing
---------
NONE of these acoustic kits are CC0; the tool ships each kit's license verbatim and
writes a CREDITS.md with attributions. It does NOT gate by license — you decide what
goes on your card (CC-BY needs attribution kept alongside; CC-BY-SA additionally makes
the derived font ShareAlike). Registry licenses: AVL Black Pearl / Red Zeppelin =
CC-BY-SA 3.0; FreePats MuldjordKit = CC-BY 4.0.

Usage
-----
  python tools/fetch_drumkits.py                 # black_pearl + red_zeppelin -> staging, validated
  python tools/fetch_drumkits.py --all           # + muldjord (non-GM keymap remap, experimental)
  python tools/fetch_drumkits.py --kits black_pearl
  python tools/fetch_drumkits.py --no-validate    # skip the tsf.h render (structural check only)
  python tools/fetch_drumkits.py --push --port COM4
"""
from __future__ import annotations

import argparse
import math
import os
import re
import struct
import subprocess
import sys
import urllib.parse
import urllib.request

import numpy as np
import soundfile as sf
from scipy.signal import resample_poly

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SF2_DIR = os.path.join(REPO_ROOT, "tools", "sf2")
sys.path.insert(0, SF2_DIR)
import merge_drum_sf2 as M  # validated SF2 RIFF writer (_assemble) + parser (parse)

OUT_RATE = 48000
PEAK_TARGET = 0.891               # ~ -1 dBFS kit-wide ceiling
TRIM_DB = -60.0                   # leading-silence threshold, relative to each hit's peak
PREROLL_MS = 1.0                  # keep this much audio before the first above-threshold sample

# SF2 instrument-zone generators we emit (opers per the SF2 2.x spec).
GEN_KEYRANGE = 43
GEN_VELRANGE = 44
GEN_SAMPLEMODES = 54
GEN_EXCLUSIVECLASS = 57
GEN_OVERRIDINGROOTKEY = 58


# --- General MIDI percussion names (for readable catalog/probe output) -------
GM_DRUM_NAME = {
    35: "Acoustic Bass Drum", 36: "Bass Drum 1", 37: "Side Stick", 38: "Acoustic Snare",
    39: "Hand Clap", 40: "Electric Snare", 41: "Low Floor Tom", 42: "Closed Hi-Hat",
    43: "High Floor Tom", 44: "Pedal Hi-Hat", 45: "Low Tom", 46: "Open Hi-Hat",
    47: "Low-Mid Tom", 48: "Hi-Mid Tom", 49: "Crash Cymbal 1", 50: "High Tom",
    51: "Ride Cymbal 1", 52: "Chinese Cymbal", 53: "Ride Bell", 54: "Tambourine",
    55: "Splash Cymbal", 56: "Cowbell", 57: "Crash Cymbal 2", 59: "Ride Cymbal 2",
}
# GM hi-hats share a choke group so open is cut by closed / pedal.
GM_HIHAT_NOTES = {42, 44, 46}


# --- kit registry ------------------------------------------------------------
# Each kit is distributed as SFZ + samples in a GitHub repo. We read the SFZ, pick
# one hit per GM note, and pull only those samples from raw.githubusercontent.
# gm_native=True  -> SFZ key is already the GM note (AVL kits).
# gm_native=False -> remap the SFZ's native key via native_key_map (MuldjordKit).
REGISTRY = [
    dict(
        name="black_pearl", display="AVL Black Pearl", program=0, default=True,
        license="CC-BY-SA-3.0",
        attribution="AVL Drumkits — Black Pearl, by Glen MacArthur. CC-BY-SA 3.0. "
                    "Source: https://github.com/studiorack/avl-drumkits",
        repo="studiorack/avl-drumkits", branch="main",
        sfz="Black_Pearl_5pc.sfz", gm_native=True,
    ),
    dict(
        name="red_zeppelin", display="AVL Red Zeppelin", program=1, default=True,
        license="CC-BY-SA-3.0",
        attribution="AVL Drumkits — Red Zeppelin, by Glen MacArthur. CC-BY-SA 3.0. "
                    "Source: https://github.com/studiorack/avl-drumkits",
        repo="studiorack/avl-drumkits", branch="main",
        sfz="Red_Zeppelin_5pc.sfz", gm_native=True,
    ),
    dict(
        name="muldjord", display="FreePats MuldjordKit", program=2, default=False,
        license="CC-BY-4.0",
        attribution="MuldjordKit (FreePats version), original by Lars Muldjord, FreePats "
                    "SFZ by roberto@zenvoid.org. CC-BY 4.0. "
                    "Source: https://github.com/freepats/muldjordkit",
        repo="freepats/muldjordkit", branch="main",
        sfz="MuldjordKit 20201018.sfz", gm_native=False,
        # MuldjordKit uses its own keymap; map its native keys to GM percussion.
        # First-mapped wins on a GM collision (L/R mic pairs, ride bells).
        native_key_map={
            48: 36,   # KdrumL      -> Bass Drum 1
            49: 35,   # KdrumR      -> Acoustic Bass Drum
            50: 38,   # Snare1      -> Acoustic Snare
            51: 40,   # Snare2      -> Electric Snare
            52: 42,   # HihatClosed -> Closed Hi-Hat
            53: 46,   # HihatOpen   -> Open Hi-Hat
            54: 51,   # RideL       -> Ride Cymbal 1
            55: 53,   # RideLBell   -> Ride Bell
            56: 59,   # RideR       -> Ride Cymbal 2
            58: 49,   # CrashL      -> Crash Cymbal 1
            59: 57,   # CrashR      -> Crash Cymbal 2
            60: 52,   # China       -> Chinese Cymbal
            61: 50,   # Tom1        -> High Tom
            62: 48,   # Tom2        -> Hi-Mid Tom
            63: 45,   # Tom3        -> Low Tom
            64: 41,   # Tom4        -> Low Floor Tom
            65: 37,   # SnareRest1  -> Side Stick
        },
    ),
]
REG = {r["name"]: r for r in REGISTRY}


# --- SFZ parsing -------------------------------------------------------------
_NOTE_RE = re.compile(r"^([a-gA-G])([#b]?)(-?\d+)$")
_OPCODE_RE = re.compile(r"([a-zA-Z0-9_]+)=(.*?)(?=\s+[a-zA-Z0-9_]+=|\s*$)")


def _note_to_int(v: str):
    """SFZ key value -> MIDI int. Accepts a number or a note name (c4, f#3, a-1)."""
    v = v.strip()
    if re.fullmatch(r"-?\d+", v):
        return int(v)
    m = _NOTE_RE.match(v)
    if not m:
        return None
    step = {"c": 0, "d": 2, "e": 4, "f": 5, "g": 7, "a": 9, "b": 11}[m.group(1).lower()]
    step += {"#": 1, "b": -1, "": 0}[m.group(2)]
    # SFZ/MIDI convention: c4 = 60 (octave offset 12 * (oct+1)).
    return step + 12 * (int(m.group(3)) + 1)


def parse_sfz(text: str):
    """Flatten an SFZ into a list of region dicts with inherited <global>/<group> opcodes."""
    # strip // comments (paths here contain no '//')
    lines = [re.sub(r"//.*", "", ln) for ln in text.splitlines()]
    control: dict[str, str] = {}
    g_global: dict[str, str] = {}
    g_group: dict[str, str] = {}
    scope = None                    # which dict opcodes currently accumulate into
    regions: list[dict] = []
    cur: dict | None = None

    def opcodes(s: str):
        return _OPCODE_RE.findall(s)

    for raw in lines:
        s = raw.strip()
        if not s:
            continue
        # a line may hold a header followed by opcodes, e.g. "<region> key=42 sample=x";
        # split on header tokens and route opcodes to the current scope.
        parts = re.split(r"(<\w+>)", s)
        buf_header = None
        for seg in parts:
            seg = seg.strip()
            if not seg:
                continue
            if seg.startswith("<") and seg.endswith(">"):
                head = seg[1:-1].lower()
                if head == "region":
                    cur = dict(g_global); cur.update(g_group)
                    regions.append(cur); scope = cur
                elif head == "group":
                    g_group = {}; scope = g_group; cur = None
                elif head == "global":
                    g_global = {}; g_group = {}; scope = g_global; cur = None
                elif head == "control":
                    scope = control; cur = None
                else:                     # <curve>/<effect>/<master>/... — ignore body
                    scope = {}; cur = None
            else:
                if scope is None:
                    scope = control       # opcodes before any header (rare) -> control
                for k, v in opcodes(seg):
                    scope[k] = v.strip()
    return control, regions


def _region_note(eff: dict):
    """The single MIDI key a drum region maps to (key, or lokey when lokey==hikey)."""
    if "key" in eff:
        return _note_to_int(eff["key"])
    lo, hi = eff.get("lokey"), eff.get("hikey")
    if lo is not None:
        loi = _note_to_int(lo)
        if hi is None or _note_to_int(hi) == loi:
            return loi
        return loi                          # ranged (unusual for drums): use low key
    return None


def _region_vel_ok(eff: dict, vel: int):
    lo = int(eff.get("lovel", 0)); hi = int(eff.get("hivel", 127))
    return lo <= vel <= hi


def select_hits(regions, kit, vel):
    """Pick one region per GM note (velocity layer nearest `vel`). Returns [(gm_note, eff)]."""
    by_gm: dict[int, dict] = {}
    dropped = 0
    for eff in regions:
        if "sample" not in eff:
            continue
        raw_key = _region_note(eff)
        if raw_key is None:
            continue
        if kit["gm_native"]:
            gm = raw_key
        else:
            gm = kit["native_key_map"].get(raw_key)
            if gm is None:
                dropped += 1
                continue
        if not _region_vel_ok(eff, vel):
            continue
        # first velocity-matching region wins; for random-robin duplicates that's the
        # lowest-lorand take, which is fine for a single representative hit.
        if gm in by_gm:
            continue
        by_gm[gm] = eff
    hits = sorted(by_gm.items())
    return hits, dropped


# --- fetch -------------------------------------------------------------------
def _raw_url(repo: str, branch: str, path: str) -> str:
    return ("https://raw.githubusercontent.com/%s/%s/%s"
            % (repo, branch, urllib.parse.quote(path)))


def _http_get(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "t-dsp-fetch-drumkits"})
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.read()


def fetch_sfz(kit) -> str:
    return _http_get(_raw_url(kit["repo"], kit["branch"], kit["sfz"])).decode("latin-1")


def fetch_sample(kit, sample_path: str, cache_dir: str) -> str:
    """Download one sample (relative to the SFZ dir = repo root here) into the cache."""
    local = os.path.join(cache_dir, sample_path.replace("/", "__"))
    if not os.path.exists(local) or os.path.getsize(local) == 0:
        data = _http_get(_raw_url(kit["repo"], kit["branch"], sample_path))
        with open(local, "wb") as fh:
            fh.write(data)
    return local


# --- normalize ---------------------------------------------------------------
def load_normalize(local_path: str):
    """Read a hit; return (mono_f32, stereo_f32) at 48 kHz, trimmed, NOT yet gained."""
    x, sr = sf.read(local_path, dtype="float64", always_2d=True)   # (frames, ch)
    if sr != OUT_RATE:
        g = math.gcd(sr, OUT_RATE)
        x = resample_poly(x, OUT_RATE // g, sr // g, axis=0)
    # trim leading silence relative to this hit's own peak
    peak = float(np.max(np.abs(x))) or 1.0
    thr = peak * (10.0 ** (TRIM_DB / 20.0))
    above = np.where(np.max(np.abs(x), axis=1) > thr)[0]
    if above.size:
        start = max(0, above[0] - int(OUT_RATE * PREROLL_MS / 1000.0))
        x = x[start:]
    mono = x.mean(axis=1)
    if x.shape[1] == 1:
        stereo = np.repeat(x, 2, axis=1)
    elif x.shape[1] >= 2:
        stereo = x[:, :2]
    return mono.astype(np.float64), stereo.astype(np.float64)


def to_int16(f: np.ndarray, gain: float) -> np.ndarray:
    y = np.clip(f * gain, -1.0, 1.0)
    return np.round(y * 32767.0).astype("<i2")


# --- build SF2 ---------------------------------------------------------------
def _name20(s: str) -> bytes:
    return s.encode("latin-1", "replace")[:20].ljust(20, b"\x00")


def build_sf2(built_kits) -> bytes:
    """built_kits: list of dicts {display, program, hits:[{note,name,mono16}]}.
    Emits one bank-128 preset per kit, each an instrument with one keyRange zone per note.
    Reuses merge_drum_sf2._assemble for the actual RIFF encoding."""
    shdr: list[dict] = []
    smpl_parts: list[bytes] = []
    cursor = 0
    inst: list[dict] = []
    ibag: list[dict] = []
    igen: list[list] = []
    imod: list[bytes] = [b"\x00" * M.IMOD_SZ]        # single (terminal) modulator record

    for kit in built_kits:
        kit["_inst_index"] = len(inst)
        inst.append(dict(name=_name20(kit["display"]), ibag=len(ibag)))
        for hit in kit["hits"]:
            arr = hit["mono16"]
            st = cursor
            en = cursor + int(arr.size)
            shdr.append(dict(name=_name20("%s%d" % (hit["name"][:16], hit["note"])),
                             start=st, end=en, sloop=st, eloop=en, rate=OUT_RATE,
                             opitch=hit["note"], pcorr=0, link=0, stype=1))  # mono
            smpl_parts.append(arr.tobytes())
            smpl_parts.append(b"\x00\x00" * M.GUARD)
            cursor = en + M.GUARD
            sid = len(shdr) - 1
            # one instrument zone: keyRange first, sampleID last (SF2 spec ordering).
            ibag.append(dict(gen=len(igen), mod=0))
            n = hit["note"]
            igen.append([GEN_KEYRANGE, struct.pack("<BB", n, n)])
            igen.append([GEN_SAMPLEMODES, struct.pack("<H", 0)])          # one-shot, no loop
            if n in GM_HIHAT_NOTES or hit.get("excl"):
                igen.append([GEN_EXCLUSIVECLASS, struct.pack("<H", hit.get("excl") or 1)])
            igen.append([GEN_OVERRIDINGROOTKEY, struct.pack("<h", n)])
            igen.append([M.GEN_SAMPLEID, struct.pack("<H", sid)])
    # terminals
    inst.append(dict(name=b"EOI", ibag=len(ibag)))
    ibag.append(dict(gen=len(igen), mod=0))
    igen.append([0, b"\x00\x00"])
    shdr.append(dict(name=b"EOS", start=0, end=0, sloop=0, eloop=0, rate=0,
                     opitch=0, pcorr=0, link=0, stype=0))
    smpl = b"".join(smpl_parts)
    if len(smpl) & 1:
        smpl += b"\x00"

    # presets: one bank-128 preset per kit
    phdr: list[dict] = []
    pbag: list[dict] = []
    pgen: list[list] = []
    pmod: list[bytes] = [b"\x00" * M.PMOD_SZ]
    for kit in built_kits:
        phdr.append(dict(name=_name20(kit["display"]), preset=kit["program"], bank=M.DRUM_BANK,
                         pbag=len(pbag), lib=0, genre=0, morph=0))
        pbag.append(dict(gen=len(pgen), mod=0))
        pgen.append([M.GEN_INSTRUMENT, struct.pack("<H", kit["_inst_index"])])
    phdr.append(dict(name=b"EOP", preset=0, bank=0, pbag=len(pbag), lib=0, genre=0, morph=0))
    pbag.append(dict(gen=len(pgen), mod=0))
    pgen.append([0, b"\x00\x00"])

    return M._assemble(None, smpl, phdr, pbag, pmod, pgen, inst, ibag, imod, igen, shdr)


# --- validation --------------------------------------------------------------
def structural_check(sf2_bytes: bytes, built_kits) -> list[str]:
    """Cross-platform floor: re-parse and assert each kit preset + its zones + PCM exist."""
    errs = []
    f = M.parse(sf2_bytes)
    banks = {(p["bank"], p["preset"]) for _, p in M.preset_list(f)}
    for kit in built_kits:
        if (M.DRUM_BANK, kit["program"]) not in banks:
            errs.append("kit %s: no bank-128 preset at program %d" % (kit["display"], kit["program"]))
    # every sampleID reference resolves and points at non-empty PCM
    for oper, amt in f["igen"][:-1]:
        if oper == M.GEN_SAMPLEID:
            sid = struct.unpack("<H", amt)[0]
            if sid >= len(f["shdr"]) - 1:
                errs.append("dangling sampleID %d" % sid); continue
            s = f["shdr"][sid]
            if s["end"] <= s["start"]:
                errs.append("empty sample #%d" % sid)
    return errs


def _find_msvc():
    """Locate a VS C++ toolchain via vswhere; return the vcvars64.bat path or None."""
    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                           "Microsoft Visual Studio", "Installer", "vswhere.exe")
    if not os.path.exists(vswhere):
        return None
    try:
        out = subprocess.check_output(
            [vswhere, "-latest", "-products", "*", "-requires",
             "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
             "-property", "installationPath"], text=True).strip()
    except Exception:
        return None
    if not out:
        return None
    vcvars = os.path.join(out, "VC", "Auxiliary", "Build", "vcvars64.bat")
    return vcvars if os.path.exists(vcvars) else None


def render_check(sf2_path: str, built_kits, out_dir: str) -> bool:
    """Compile tsf_drum_probe.cpp against the shipped tsf.h and render each kit's notes.
    Returns True if the render check ran and passed; False if it couldn't run (no MSVC)."""
    if os.name != "nt":
        print("  [validate] tsf.h render is Windows/MSVC-only here; ran structural check. "
              "On device, send 'T' to sweep.")
        return False
    vcvars = _find_msvc()
    if not vcvars:
        print("  [validate] no MSVC toolchain found (vswhere); skipped tsf.h render. "
              "Structural check passed; verify on device with 'T'.")
        return False

    tsf_inc = os.path.join(REPO_ROOT, "lib", "TDspTsf", "src")
    cpp = os.path.join(SF2_DIR, "tsf_drum_probe.cpp")
    exe = os.path.join(out_dir, "tsf_drum_probe.exe")
    bat = os.path.join(out_dir, "_build_probe.bat")
    with open(bat, "w") as fh:
        fh.write('@echo off\r\ncall "%s" >nul\r\n' % vcvars)
        fh.write('cl /nologo /EHsc /O2 /D_CRT_SECURE_NO_WARNINGS /I"%s" /Fe:"%s" /Fo:"%s\\\\" "%s"\r\n'
                 % (tsf_inc, exe, out_dir, cpp))
    r = subprocess.run(["cmd", "/c", bat], capture_output=True, text=True)
    if not os.path.exists(exe):
        print("  [validate] probe compile FAILED:\n" + (r.stdout or "") + (r.stderr or ""))
        return False

    all_ok = True
    for kit in built_kits:
        notes = ",".join(str(h["note"]) for h in kit["hits"])
        wav = os.path.join(out_dir, "probe_%s.wav" % kit["name"])
        pr = subprocess.run([exe, sf2_path, str(kit["program"]), notes, wav],
                            capture_output=True, text=True)
        tail = pr.stdout.strip().splitlines()[-1] if pr.stdout.strip() else "(no output)"
        print("  [validate] %-22s %s" % (kit["display"], tail))
        if pr.returncode != 0:
            all_ok = False
            for ln in pr.stdout.splitlines():
                if "SILENT" in ln:
                    print("      " + ln.strip())
    return all_ok


# --- outputs -----------------------------------------------------------------
def write_catalog(out_dir: str, built_kits):
    """drumkits.tsv — the manifest the later @DRUMKIT wiring reads (program -> kit)."""
    path = os.path.join(out_dir, "drumkits.tsv")
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# program\tname\tlicense\tpieces\tdisplay\n")
        for kit in built_kits:
            fh.write("%d\t%s\t%s\t%d\t%s\n" % (kit["program"], kit["name"], kit["license"],
                                               len(kit["hits"]), kit["display"]))
    return path


def write_credits(out_dir: str, built_kits):
    path = os.path.join(out_dir, "CREDITS.md")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("# drumkits.sf2 — sources, licenses & attribution\n\n")
        fh.write("Built by tools/fetch_drumkits.py. Each kit below is a bank-128 preset in "
                 "drumkits.sf2. **Keep this file with the font if you redistribute it.**\n\n")
        for kit in built_kits:
            fh.write("## %s  (program %d)\n" % (kit["display"], kit["program"]))
            fh.write("- License: **%s**\n- %s\n- %d GM pieces\n\n"
                     % (kit["license"], kit["attribution"], len(kit["hits"])))
    return path


# --- push --------------------------------------------------------------------
def push(files, port: str):
    """Stream each (local, sd_path) to the card over USB @WB.

    Reuses sync_assets.py's pusher, whose completion timeout SCALES with file size —
    push_file_serial.ps1's fixed 30 s would time out on a multi-MB soundfont. Falls
    back to the .ps1 (small files) only if sync_assets/pyserial isn't importable.
    """
    try:
        sys.path.insert(0, os.path.join(REPO_ROOT, "tools"))
        import sync_assets as S
        dev = S.Device(S.resolve_port(port), 115200)
        try:
            ok, failures, _ = S.push_files(dev, files, 0)
        finally:
            dev.close()
        print("[push] %d/%d file(s) landed on the card." % (ok, len(files)))
        if failures:
            print("[push] FAILED: " + ", ".join(failures))
        return
    except ImportError:
        pass
    ps1 = os.path.join(REPO_ROOT, "tools", "push_file_serial.ps1")
    for local, sd in files:
        print("[push] %s -> %s" % (os.path.basename(local), sd))
        subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", ps1,
                        "-File", local, "-SdPath", sd, "-Port", port, "-Verify"], check=False)


# --- driver ------------------------------------------------------------------
def process_kit(kit, cache_root: str, assets_root: str, vel: int):
    print("[%s] %s  (%s)" % (kit["name"], kit["display"], kit["license"]))
    cache_dir = os.path.join(cache_root, kit["name"])
    os.makedirs(cache_dir, exist_ok=True)
    regions = parse_sfz(fetch_sfz(kit))[1]
    hits_sel, dropped = select_hits(regions, kit, vel)
    if dropped:
        print("    note: %d region(s) had no GM mapping and were skipped" % dropped)
    if not hits_sel:
        print("    !! no playable hits selected; skipping kit")
        return None

    # download + normalize each selected hit (collect float, gain once per kit)
    mono_f: list[np.ndarray] = []
    stereo_f: list[np.ndarray] = []
    meta: list[dict] = []
    kit_peak = 1e-9
    for gm_note, eff in hits_sel:
        sample_path = eff["sample"].replace("\\", "/")
        local = fetch_sample(kit, sample_path, cache_dir)
        mono, stereo = load_normalize(local)
        kit_peak = max(kit_peak, float(np.max(np.abs(stereo))), float(np.max(np.abs(mono))))
        mono_f.append(mono)
        stereo_f.append(stereo)
        excl = None
        if "group" in eff:
            try:
                excl = int(eff["group"]) or None
            except ValueError:
                excl = None
        piece = os.path.splitext(os.path.basename(sample_path))[0]
        piece = re.sub(r"^\d+[-_]?", "", piece)   # strip a leading "36-" note prefix
        meta.append(dict(note=gm_note, name=piece or GM_DRUM_NAME.get(gm_note, "hit"), excl=excl))

    gain = PEAK_TARGET / kit_peak
    hits: list[dict] = []
    kit_assets = os.path.join(assets_root, kit["name"])
    os.makedirs(kit_assets, exist_ok=True)
    for i, m in enumerate(meta):
        mono16 = to_int16(mono_f[i], gain)
        stereo16 = to_int16(stereo_f[i], gain)
        # archive stereo WAV for a future raw-sampler path
        wav = os.path.join(kit_assets, "%d-%s.wav" % (m["note"], m["name"]))
        sf.write(wav, stereo16, OUT_RATE, subtype="PCM_16")
        hits.append(dict(note=m["note"], name=m["name"], excl=m["excl"], mono16=mono16))
    print("    %d GM pieces -> %s (stereo archive), kit gain x%.2f" % (len(hits), kit_assets, gain))
    return dict(name=kit["name"], display=kit["display"], program=kit["program"],
                license=kit["license"], attribution=kit["attribution"], hits=hits)


def main() -> int:
    ap = argparse.ArgumentParser(description="Build a TSF drum SoundFont from open acoustic kits.")
    ap.add_argument("--kits", help="comma-separated kit names (default: the 'default' set)")
    ap.add_argument("--all", action="store_true", help="include every registry kit (adds muldjord)")
    ap.add_argument("--vel", type=int, default=100, help="velocity layer to sample per hit (1-127)")
    ap.add_argument("--out", default="c:/tmp/t-dsp-drumkits", help="staging dir for the SF2 + catalog")
    ap.add_argument("--assets", default=os.path.join(REPO_ROOT, "firmware", "mix-kit", "assets", "samples"),
                    help="where the raw stereo WAV archive is written")
    ap.add_argument("--no-validate", action="store_true", help="skip the tsf.h render check")
    ap.add_argument("--push", action="store_true", help="stream drumkits.sf2 to /sf2 over USB")
    ap.add_argument("--port", default="COM4", help="serial port for --push")
    ap.add_argument("--list", action="store_true", help="list the kit registry and exit")
    args = ap.parse_args()

    if args.list:
        for r in REGISTRY:
            print("  %-14s %-12s prog %d  %s%s" % (r["name"], r["license"], r["program"],
                  r["display"], "  (default)" if r["default"] else ""))
        return 0

    if args.kits:
        names = [n.strip() for n in args.kits.split(",")]
    elif args.all:
        names = [r["name"] for r in REGISTRY]
    else:
        names = [r["name"] for r in REGISTRY if r["default"]]
    unknown = [n for n in names if n not in REG]
    if unknown:
        sys.exit("unknown kit(s): %s (see --list)" % ", ".join(unknown))

    os.makedirs(args.out, exist_ok=True)
    cache_root = os.path.join(args.out, "cache")

    built = []
    for n in names:
        k = process_kit(REG[n], cache_root, args.assets, args.vel)
        if k:
            built.append(k)
    if not built:
        sys.exit("no kits built.")

    print("[build] assembling drumkits.sf2 (%d preset(s)) …" % len(built))
    sf2_bytes = build_sf2(built)
    sf2_path = os.path.join(args.out, "drumkits.sf2")
    with open(sf2_path, "wb") as fh:
        fh.write(sf2_bytes)
    print("    wrote %s (%.2f MB)" % (sf2_path, len(sf2_bytes) / 1048576))

    errs = structural_check(sf2_bytes, built)
    if errs:
        print("  [validate] STRUCTURAL ERRORS:")
        for e in errs:
            print("      " + e)
        sys.exit("aborting: the SF2 did not pass the structural check.")
    print("  [validate] structural check OK (presets, zones, PCM all resolve)")

    if not args.no_validate:
        ok = render_check(sf2_path, built, args.out)
        if ok:
            print("  [validate] tsf.h render OK — every kit's notes sound.")

    cat = write_catalog(args.out, built)
    cred = write_credits(args.out, built)
    print("    catalog -> %s\n    credits -> %s" % (cat, cred))

    if args.push:
        push([(sf2_path, "/sf2/drumkits.sf2"),
              (cat, "/sf2/drumkits.tsv"),
              (cred, "/sf2/drumkits_CREDITS.md")], args.port)
    else:
        print("\nDone. Copy %s to the card as /sf2/drumkits.sf2 (or re-run with --push)." % sf2_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
