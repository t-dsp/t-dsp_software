"""
framework_overlay.py — PlatformIO extra_scripts hook that overlays vendored
content from lib/ onto PlatformIO's framework-arduinoteensy package cache
before each compile.

The overlay pattern: rather than patching individual files (which rot when
PlatformIO updates the framework), we replace whole files in the framework
cache with our vendored copies. This is robust to upstream churn — if
upstream files drift in incompatible ways we discover it at compile time
in our own source, not as a mysterious patch-application failure.

Currently overlays:
    lib/teensy_cores/teensy4/  ->  framework-arduinoteensy/cores/teensy4/
    lib/CNMAT_OSC/             ->  framework-arduinoteensy/libraries/OSC/

Audio library overlay (lib/Audio/ -> framework-arduinoteensy/libraries/Audio/)
is intentionally NOT yet wired up. It will be added in Spike 2 when we need
multi-channel USB extensions to the Audio library.

Loaded via:
    [env:teensy41]
    extra_scripts = pre:../../tools/cores_overlay.py

See planning/osc-mixer-foundation/05-vendoring-strategy.md for design notes.
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path

# PlatformIO injects an `env` SConstruct global into extra_scripts. Defensive
# import so this file can also be linted standalone.
try:
    Import("env")  # type: ignore  # noqa: F821
except NameError:  # pragma: no cover — only hit if loaded outside PlatformIO
    env = None


def _find_repo_root() -> Path:
    """
    Locate the T-DSP repo root. Walk up from the PlatformIO project directory
    looking for vendored.json (our repo-root marker). Don't use __file__ —
    PlatformIO's SCons exec() doesn't set it.
    """
    if env is not None:
        start = Path(str(env["PROJECT_DIR"]))
    else:
        start = Path(os.getcwd())
    for candidate in [start, *start.parents]:
        if (candidate / "vendored.json").exists():
            return candidate
    raise RuntimeError(
        f"framework_overlay: could not locate repo root (vendored.json) starting from {start}"
    )


REPO_ROOT = _find_repo_root()
MARKER_FILENAME = ".tdsp_overlay_applied"

# (vendored_subdir, framework_target_subpath, friendly name) tuples.
# Add new overlays here when needed (e.g. lib/Audio -> libraries/Audio).
OVERLAYS = [
    (REPO_ROOT / "lib" / "teensy_cores" / "teensy4",
     ("cores", "teensy4"),
     "teensy_cores"),
    (REPO_ROOT / "lib" / "CNMAT_OSC",
     ("libraries", "OSC"),
     "CNMAT_OSC"),
]


def _framework_pkg_dir() -> Path | None:
    if env is None:
        return None
    pkg_dir = env.PioPlatform().get_package_dir("framework-arduinoteensy")
    return Path(pkg_dir) if pkg_dir else None


def _vendored_state_marker(vendored_path: Path) -> str:
    """
    Return a string identifying the current state of one vendored subtree.
    Uses git log of the path so re-overlay only fires when content changes.
    """
    try:
        rel = vendored_path.relative_to(REPO_ROOT)
        out = subprocess.run(
            ["git", "log", "-1", "--format=%H", "--", str(rel)],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            check=True,
        )
        return out.stdout.strip() or "unknown"
    except Exception:
        return "unknown"


def _apply_overlay(vendored: Path, target: Path, name: str) -> None:
    if not vendored.is_dir():
        print(f"framework_overlay: WARNING — {name} not found at {vendored}")
        return
    if not target.parent.exists():
        print(f"framework_overlay: WARNING — target parent missing: {target.parent}")
        return
    target.mkdir(parents=True, exist_ok=True)

    marker = target / MARKER_FILENAME
    state = _vendored_state_marker(vendored)
    if marker.exists() and marker.read_text().strip() == state:
        print(f"framework_overlay: {name} already applied at {target}")
        return

    print(f"framework_overlay: {name}: {vendored} -> {target}")
    # Skip git internals if present (the subtree dir doesn't have .git, but
    # be defensive).
    skip_dirs = {".git", "examples", ".github"}
    for src in vendored.rglob("*"):
        if any(part in skip_dirs for part in src.relative_to(vendored).parts):
            continue
        if src.is_file():
            rel = src.relative_to(vendored)
            dst = target / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)

    marker.write_text(state + "\n")
    print(f"framework_overlay: {name}: applied; marker = {state[:12]}")


def apply_all_overlays() -> None:
    if env is None:
        print("framework_overlay: not running inside PlatformIO; skipping")
        return
    pkg_dir = _framework_pkg_dir()
    if pkg_dir is None or not pkg_dir.is_dir():
        print("framework_overlay: WARNING — framework-arduinoteensy not found in package cache")
        return
    print(f"framework_overlay: framework cache at {pkg_dir}")
    for vendored, subpath, name in OVERLAYS:
        target = pkg_dir.joinpath(*subpath)
        _apply_overlay(vendored, target, name)


def override_toolchain_to_teensy_gcc11() -> None:
    """
    PlatformIO platform-teensy 5.1.0 hard-codes `toolchain-gccarmnoneeabi`
    (gcc 5.4.1, 2016) as the toolchain it expects, but the framework cores it
    ships use C++17 features that gcc 5.4 cannot parse. The right toolchain
    is `toolchain-gccarmnoneeabi-teensy` (gcc 11.3.1, 2023). Rather than
    fighting PlatformIO's package resolution, we override the SCons compiler
    env directly to point at the gcc 11 binaries. This bypasses the platform's
    toolchain selection entirely — the build uses gcc 11 regardless of which
    toolchain package PlatformIO thinks should be active.

    Requires `platform_packages = platformio/toolchain-gccarmnoneeabi-teensy`
    in platformio.ini so the package is installed and present in the cache.
    """
    if env is None:
        return
    teensy_tc = env.PioPlatform().get_package_dir("toolchain-gccarmnoneeabi-teensy")
    if not teensy_tc:
        print("framework_overlay: WARNING — toolchain-gccarmnoneeabi-teensy not installed")
        print("framework_overlay: add it to platform_packages in platformio.ini")
        return
    bin_dir = Path(teensy_tc) / "bin"
    if not bin_dir.is_dir():
        print(f"framework_overlay: WARNING — gcc 11 bin dir missing at {bin_dir}")
        return
    # PrependENVPath puts the gcc 11 bin directory at the front of the PATH
    # used for SCons subprocess calls. PlatformIO's build commands invoke the
    # compiler by unqualified name (e.g. `arm-none-eabi-g++`), so a PATH
    # prepend is what actually causes the gcc 11 binaries to be picked up.
    env.PrependENVPath("PATH", str(bin_dir))
    # Belt + suspenders: also Replace the explicit tool variables in case
    # any build step references them by SCons env var name.
    prefix = "arm-none-eabi-"
    env.Replace(
        CC=str(bin_dir / f"{prefix}gcc.exe"),
        CXX=str(bin_dir / f"{prefix}g++.exe"),
        AS=str(bin_dir / f"{prefix}gcc.exe"),
        AR=str(bin_dir / f"{prefix}gcc-ar.exe"),
        RANLIB=str(bin_dir / f"{prefix}gcc-ranlib.exe"),
        LD=str(bin_dir / f"{prefix}gcc.exe"),
        OBJCOPY=str(bin_dir / f"{prefix}objcopy.exe"),
        OBJDUMP=str(bin_dir / f"{prefix}objdump.exe"),
        SIZETOOL=str(bin_dir / f"{prefix}size.exe"),
        NM=str(bin_dir / f"{prefix}gcc-nm.exe"),
    )
    print(f"framework_overlay: toolchain override -> gcc 11 PATH prepended {bin_dir}")


# Run on script load (PlatformIO `extra_scripts = pre:` semantics).
apply_all_overlays()
override_toolchain_to_teensy_gcc11()
