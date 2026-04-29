"""
overlay_teensy_usb_audio.py -- per-project PlatformIO extra_scripts hook.

Copies the teensy-4-usbAudio reference's changedCorefiles/ verbatim onto
PlatformIO's framework-arduinoteensy package cache before each compile.
This is what the reference's README tells you to do manually for the
Arduino IDE; we automate it for PIO.

The marker file content encodes the reference's git-log hash, so:
  - Re-runs of this project skip the copy when nothing changed.
  - Switching to another project (e.g. spike_f32_usb_loopback) whose
    overlay uses a DIFFERENT source path will see a marker mismatch
    and re-overlay correctly.

Also applies the gcc 11 toolchain override -- platform-teensy 5.1.0
hard-codes gcc 5.4 which can't parse the C++17 in the cores. Same fix
as ../../tools/cores_overlay.py.
"""

import os
import shutil
import subprocess
from pathlib import Path

try:
    Import("env")  # type: ignore  # noqa: F821
except NameError:
    env = None


def _find_repo_root() -> Path:
    if env is not None:
        start = Path(str(env["PROJECT_DIR"]))
    else:
        start = Path(os.getcwd())
    for candidate in [start, *start.parents]:
        if (candidate / "vendored.json").exists():
            return candidate
    raise RuntimeError("could not locate repo root (vendored.json)")


REPO_ROOT = _find_repo_root()
REFERENCE_CORES = (REPO_ROOT / "references" / "teensy-4-usbAudio"
                   / "changedCorefiles")
MARKER_FILENAME = ".tdsp_overlay_applied"


def _state_marker(path: Path) -> str:
    try:
        rel = path.relative_to(REPO_ROOT)
        out = subprocess.run(
            ["git", "log", "-1", "--format=%H", "--", str(rel)],
            cwd=REPO_ROOT, capture_output=True, text=True, check=True,
        )
        return out.stdout.strip() or "unknown"
    except Exception:
        return "unknown"


def _apply_overlay() -> None:
    if env is None:
        return
    pkg_dir = env.PioPlatform().get_package_dir("framework-arduinoteensy")
    if not pkg_dir:
        print("overlay_teensy_usb_audio: framework not in package cache")
        return
    target = Path(pkg_dir) / "cores" / "teensy4"
    if not target.parent.exists():
        print(f"overlay_teensy_usb_audio: missing {target.parent}")
        return
    if not REFERENCE_CORES.is_dir():
        print(f"overlay_teensy_usb_audio: missing {REFERENCE_CORES}")
        return

    state = "ref-" + _state_marker(REFERENCE_CORES)
    marker = target / MARKER_FILENAME
    if marker.exists() and marker.read_text().strip() == state:
        print(f"overlay_teensy_usb_audio: already applied ({state[:16]})")
    else:
        print(f"overlay_teensy_usb_audio: {REFERENCE_CORES} -> {target}")
        for src in REFERENCE_CORES.rglob("*"):
            if src.is_file():
                rel = src.relative_to(REFERENCE_CORES)
                dst = target / rel
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, dst)
        marker.write_text(state + "\n")
        print(f"overlay_teensy_usb_audio: applied ({state[:16]})")


def _override_toolchain() -> None:
    if env is None:
        return
    teensy_tc = env.PioPlatform().get_package_dir(
        "toolchain-gccarmnoneeabi-teensy")
    if not teensy_tc:
        print("overlay_teensy_usb_audio: gcc 11 toolchain not installed")
        return
    bin_dir = Path(teensy_tc) / "bin"
    if not bin_dir.is_dir():
        return
    env.PrependENVPath("PATH", str(bin_dir))
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
    print(f"overlay_teensy_usb_audio: gcc 11 PATH prepended {bin_dir}")


_apply_overlay()
_override_toolchain()
