"""dexed_bend_range.py — PlatformIO pre-script: lift synth_dexed's pitch-bend range cap 12 -> 24.

synth_dexed (dcoredump) hard-caps the pitch-bend range at 12 semitones in dexed.cpp:

    void Dexed::setPitchbendRange(uint8_t range) {
      range = constrain(range, 0, 12);      // <-- this line
      ...

so DexedPoolSink's kBendRange = 24 (a full MPE two-octave bend) is silently clamped to one
octave — the Dexed pool then bends only +-12 while the OPLL engines do the intended +-24. DX7
hardware maxes at 12, but the FM pitch math handles wider, and our MPE setup wants +-24 across
every engine. This rewrites the cap to 24 in the FETCHED lib_dep before it is compiled.

Robustness / scope:
  * Idempotent — skips a file already patched; a clean re-fetch just gets re-patched next build.
  * Only touches the setPitchbendRange line ("range = constrain(range, 0, 12)"), NOT the separate
    setPitchbendStep line ("step = constrain(step, 0, 12)").
  * A no-op for envs that don't pull in synth_dexed (the glob finds nothing).
  * Loaded via [env:common] extra_scripts; PlatformIO installs lib_deps before SCons runs the
    extra_scripts, so the fetched source is present when this executes (even on a clean build).

If synth_dexed upstream changes this line, the WARNING below fires and the cap silently stays 12 —
re-check dexed.cpp then. See planning / memory: Dexed pitch-bend +-12 cap.
"""
from __future__ import annotations
import glob
import os

try:
    Import("env")  # type: ignore  # noqa: F821  (injected by PlatformIO/SCons)
except NameError:  # pragma: no cover — only when linted standalone
    env = None

# Lift the cap to 48 semitones (a 4-octave slide) — the widest TDSP_MPE_BEND_RANGE we build with.
# A build that uses a narrower range (e.g. 24) still works: the cap only sets the ceiling. Handles a
# fresh checkout ("0, 12") AND a lib_dep already patched to 24 by an earlier build.
_OLD_VARIANTS = ("range = constrain(range, 0, 12);", "range = constrain(range, 0, 24);")
_NEW = "range = constrain(range, 0, 48);"


def _patch():
    if env is None:
        return
    libdeps = str(env["PROJECT_LIBDEPS_DIR"])
    pioenv = str(env["PIOENV"])
    hits = glob.glob(os.path.join(libdeps, pioenv, "**", "dexed.cpp"), recursive=True)
    for path in hits:
        try:
            with open(path, "r", encoding="utf-8", errors="ignore") as f:
                txt = f.read()
        except OSError:
            continue
        if _NEW in txt:
            print(f"[dexed_bend_range] already +-48: {path}")
            continue
        old = next((o for o in _OLD_VARIANTS if o in txt), None)
        if old:
            with open(path, "w", encoding="utf-8") as f:
                f.write(txt.replace(old, _NEW))
            print(f"[dexed_bend_range] pitch-bend range cap -> 48 in {path}")
        else:
            print(f"[dexed_bend_range] WARNING: cap line not found in {path} — synth_dexed changed? cap unchanged")


_patch()
