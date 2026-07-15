#!/usr/bin/env python3
"""
build_drum_matrix.py — drum-enhanced GM fonts for every PSRAM tier.

For each base font in the PSRAM ladder (tools/sf2/fonts/gm_*.sf2) and each staged drum kit
(tools/sf2/drum_fonts/downloads/*.sf2), graft the kit onto the base and DOWNSAMPLE the result to
fit that tier's PSRAM budget, so you get a ready-to-activate drum-enhanced font for whatever RAM
you end up with. The downsampler (build_gu_fonts) only touches samples above the target rate — the
base is already at its tier rate, so effectively only the grafted drum samples get resampled.

Fit logic (per base × kit):
  drum_budget = tier_smpl_budget(base) - base_smpl - margin
  pick the highest standard rate whose downsampled kit fits drum_budget; if even the floor
  (RATE_FLOOR) is too big -> SKIP and report (this is how the 244 MB Perfect Drums drops out of
  every tier ≤64 MB automatically). Kit downsamples are cached and reused across bases.

Output: tools/sf2/drum_fonts/matrix/<basetag>__<kit>[@<rate>k].sf2  + a MATRIX.md report.

Usage:
  python tools/sf2/build_drum_matrix.py [--fonts DIR] [--only kit1,kit2] [--floor 12000]
"""
import os, sys, math

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import merge_drum_sf2 as M
import build_gu_fonts as B

DL_DIR   = os.path.join(HERE, "drum_fonts", "downloads")
CACHE    = os.path.join(HERE, "drum_fonts", "kitcache")
OUT_DIR  = os.path.join(HERE, "drum_fonts", "matrix")
FONTS    = os.path.join(HERE, "fonts")

# base font -> installed PSRAM MB it targets (from FONTS.md)
TIERS = [
    ("gm_tim",     8),
    ("gm_gu8",    16),
    ("gm_gu12",   16),
    ("gm_gu14",   16),
    ("gm_gu16",   32),
    ("gm_gu18",   32),
    ("gm_gu22",   32),
    ("gm_gu24",   32),
    ("gm_gu_full",64),
]
# Per-PSRAM merged-`smpl` CEILING. The merge REPLACES the base's drums (GC prunes them), so the
# gate is the MERGED font's smpl size, not base+kit. Ceilings are the FONTS.md known-good tier
# baselines + a little room (gm_tsf 5.5@8MB, gu12 11.7 / gu14 13.5@16MB, gu24 22.3@32MB,
# gu_full 30.6@64MB all fit their tier, and merged tables ~= base tables).
CEILINGS    = {8: 6.5, 16: 14.0, 32: 26.0, 64: 45.0}
STD_RATES   = [24000, 22050, 16000, 12000]
NATIVE      = 44100


def smpl_mb_of(path):
    f = M.parse(path)
    return len(f["pcm"]) / 1048576.0


def kit_variant(kit_name, rate):
    """Return path to the kit downsampled to `rate` (cached). rate=None -> native source."""
    src = os.path.join(DL_DIR, kit_name + ".sf2")
    if rate is None:
        return src
    os.makedirs(CACHE, exist_ok=True)
    out = os.path.join(CACHE, "%s@%dk.sf2" % (kit_name, rate // 1000))
    if not os.path.exists(out):
        data, sub = B.parse(src)
        B.build(data, sub, rate, out)
    return out


def main(argv):
    global FONTS
    only = None
    if "--only" in argv:
        only = [x.strip() for x in argv[argv.index("--only") + 1].split(",")]
    if "--fonts" in argv:
        FONTS = argv[argv.index("--fonts") + 1]
    floor = int(argv[argv.index("--floor") + 1]) if "--floor" in argv else 12000

    kits = [os.path.splitext(fn)[0] for fn in sorted(os.listdir(DL_DIR)) if fn.endswith(".sf2")] if os.path.isdir(DL_DIR) else []
    if only:
        kits = [k for k in kits if k in only]
    if not kits:
        sys.exit("no staged kits in %s (run fetch_drum_fonts.py first)" % DL_DIR)
    os.makedirs(OUT_DIR, exist_ok=True)

    # precompute kit native sizes + drum bank
    kit_mb, kit_bank = {}, {}
    for k in kits:
        p = os.path.join(DL_DIR, k + ".sf2")
        f = M.parse(p)
        kit_mb[k] = len(f["pcm"]) / 1048576.0
        kit_bank[k] = M.DRUM_BANK if M.DRUM_BANK in {pp["bank"] for _, pp in M.preset_list(f)} else \
                      min({pp["bank"] for _, pp in M.preset_list(f)})

    rows = []
    for base_tag, psram in TIERS:
        base_path = os.path.join(FONTS, base_tag + ".sf2")
        if not os.path.exists(base_path):
            rows.append((base_tag, psram, "-", "-", "base font missing (%s)" % os.path.basename(base_path)))
            continue
        base = M.parse(base_path)
        ceiling = CEILINGS[psram]
        for k in kits:
            # cheap pre-skip: if the drums ALONE at the floor rate already blow the ceiling,
            # no merge can fit (this drops 244 MB Perfect Drums from every tier without work).
            if kit_mb[k] * (floor / NATIVE) > ceiling:
                rows.append((base_tag, psram, k, "skip",
                             "drums alone %.0fMB@%dk > %.1fMB ceiling"
                             % (kit_mb[k] * floor / NATIVE, floor // 1000, ceiling)))
                continue
            # try native, then each standard rate down to the floor; keep the first that fits.
            candidates = ([None] if kit_mb[k] <= ceiling else []) + [r for r in STD_RATES if r >= floor]
            best = None
            for r in candidates:
                kv = kit_variant(k, r)
                merged = M.merge(base, M.parse(kv), drum_bank=kit_bank[k], dest_bank=M.DRUM_BANK)
                got = len(M.parse(merged)["pcm"]) / 1048576.0
                best = (r, merged, got)
                if got <= ceiling:
                    break
            r, merged, got = best
            tag = "native" if r is None else "%dk" % (r // 1000)
            if got > ceiling:
                rows.append((base_tag, psram, k, "skip",
                             "min %.1fMB@%s > %.1fMB ceiling (won't fit %dMB above %dk floor)"
                             % (got, tag, ceiling, psram, floor // 1000)))
                print("  %-11s %2dMB  %-16s skip    (min %.1fMB > %.1f)" % (base_tag, psram, k, got, ceiling))
                continue
            suffix = "" if r is None else "@%dk" % (r // 1000)
            out = os.path.join(OUT_DIR, "%s__%s%s.sf2" % (base_tag, k, suffix))
            open(out, "wb").write(merged)
            rows.append((base_tag, psram, k, tag, "%.1fMB smpl (ceiling %.1f) OK -> %s"
                         % (got, ceiling, os.path.basename(out))))
            print("  %-11s %2dMB  %-16s %-7s %.1fMB OK" % (base_tag, psram, k, tag, got))

    # report
    md = ["# Drum matrix — per-PSRAM-tier drum-enhanced fonts\n",
          "Generated by build_drum_matrix.py. Each row = a base tier × drum kit, downsampled to fit.\n",
          "Activate by copying the chosen file to the card as the path your build env loads",
          "(default `/sf2/gm_tsf.sf2`), or point a `TSF_FONT_*` env at it.\n",
          "| Base | PSRAM | Kit | Drum rate | Result |",
          "|---|---|---|---|---|"]
    for base_tag, psram, k, tag, note in rows:
        md.append("| %s | %dMB | %s | %s | %s |" % (base_tag, psram, k, tag, note))
    open(os.path.join(OUT_DIR, "MATRIX.md"), "w", encoding="utf-8").write("\n".join(md) + "\n")
    made = sum(1 for r in rows if r[3] not in ("skip", "-"))
    print("\n%d fonts built, %d combos skipped. Report: %s"
          % (made, len(rows) - made, os.path.relpath(os.path.join(OUT_DIR, "MATRIX.md"))))


if __name__ == "__main__":
    main(sys.argv[1:])
