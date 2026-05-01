#!/usr/bin/env python3
"""fetch_samples.py — assemble sample banks for the T-DSP multisample slot.

Outputs a folder structure ready to drag onto the Teensy's SD card:

    <out>/samples/<bank>/<note>.wav

where each WAV is 48 kHz / 16-bit signed PCM / stereo and the filename
matches the note name parsed by NoteParser (C4, F#3, A0, etc.).

Modes
-----
test-tones
    Generate sine waves at common piano root notes. No download required;
    works offline. Useful to verify the audio path end-to-end before
    committing to a real piano content download. ~7 MB of WAVs.

salamander-trimmed
    (TODO) Download a curated Salamander Grand Piano V3 subset and
    convert OGG -> WAV. Requires ffmpeg on PATH. Bigger download (~80 MB).

Usage
-----
    python tools/fetch_samples.py test-tones
    python tools/fetch_samples.py test-tones --bank piano --out C:/tmp/t-dsp-samples
    python tools/fetch_samples.py test-tones --push    # auto-detect & copy
    python tools/fetch_samples.py salamander-trimmed   # not yet implemented

By default the script writes to a staging directory (default
c:/tmp/t-dsp-samples) and prints instructions for dragging the result
onto the SD card.

With --push, after generating the files the script invokes
tools/push_to_teensy.ps1 which auto-detects the destination:
  * a mounted drive whose volume label matches "T-DSP*" or "*Teensy*"
    (card reader path), or
  * a Windows MTP portable device with a matching name (companion-
    firmware path; requires t-dsp_mtp_disk firmware running on the
    Teensy).
"""

from __future__ import annotations

import argparse
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import wave
from pathlib import Path

# ----------------------------------------------------------------------------
# Note <-> MIDI <-> frequency conversion (matches src/synth/NoteParser.h's
# C-1=0, A0=21, C4=60 convention).
# ----------------------------------------------------------------------------

_PITCH_CLASS = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}


def note_to_midi(name: str) -> int:
    """Convert a note name like 'C4', 'F#3', 'Bb-1' to a MIDI number."""
    s = name.strip()
    if not s:
        raise ValueError("empty note name")
    letter = s[0].upper()
    if letter not in _PITCH_CLASS:
        raise ValueError(f"bad letter: {s!r}")
    semis = _PITCH_CLASS[letter]
    i = 1
    if i < len(s) and s[i] == "#":
        semis += 1
        i += 1
    elif i < len(s) and s[i] in ("b", "B") and i + 1 < len(s) and s[i + 1] in "-0123456789":
        semis -= 1
        i += 1
    octave_str = s[i:]
    try:
        octave = int(octave_str)
    except ValueError:
        raise ValueError(f"bad octave in {s!r}")
    midi = (octave + 1) * 12 + semis
    if not 0 <= midi <= 127:
        raise ValueError(f"midi out of range: {s!r} -> {midi}")
    return midi


def midi_to_freq(midi: int) -> float:
    return 440.0 * (2.0 ** ((midi - 69) / 12.0))


# ----------------------------------------------------------------------------
# WAV writing — 48 kHz, 16-bit signed PCM, stereo.
# ----------------------------------------------------------------------------

SAMPLE_RATE = 48000
BIT_DEPTH = 16
CHANNELS = 2  # stereo (slot expects 2 channels; mono drops the right side)


def write_sine_wav(path: Path, freq_hz: float, duration_s: float = 2.0,
                   amplitude: float = 0.5) -> None:
    """Write a stereo sine WAV with brief in/out fades (50 ms) to avoid clicks."""
    n_total = int(SAMPLE_RATE * duration_s)
    fade_n = int(SAMPLE_RATE * 0.05)
    peak = int(amplitude * 32767)
    omega = 2.0 * math.pi * freq_hz / SAMPLE_RATE

    frames = bytearray()
    for i in range(n_total):
        if i < fade_n:
            env = i / fade_n
        elif i > n_total - fade_n:
            env = max(0.0, (n_total - i) / fade_n)
        else:
            env = 1.0
        s = int(env * peak * math.sin(omega * i))
        # interleave L, R
        frames += struct.pack("<hh", s, s)

    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(CHANNELS)
        w.setsampwidth(BIT_DEPTH // 8)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(bytes(frames))


# ----------------------------------------------------------------------------
# Mode: test-tones
# ----------------------------------------------------------------------------

# Sparse layout: every 3-4 semitones, full piano range. The slot's
# closestSampleIdx() pitches up/down from the nearest sample, so this
# spacing keeps any played note within ~2 semitones of a real sample —
# audible aliasing is mild.
TEST_TONE_NOTES = [
    "A0", "C2", "E2", "A2",
    "C3", "E3", "A3",
    "C4", "E4", "A4",
    "C5", "E5", "A5",
    "C6", "A6",
    "C7", "A7", "C8",
]


def cmd_test_tones(out_dir: Path, bank: str) -> None:
    bank_dir = out_dir / "samples" / bank
    # Clear previous run so leftover files don't confuse the slot's bank scan.
    if bank_dir.exists():
        shutil.rmtree(bank_dir)
    print(f"Writing {len(TEST_TONE_NOTES)} test-tone WAVs to {bank_dir}")
    for name in TEST_TONE_NOTES:
        midi = note_to_midi(name)
        freq = midi_to_freq(midi)
        path = bank_dir / f"{name}.wav"
        write_sine_wav(path, freq)
        print(f"  {name:>4}  midi={midi:3d}  {freq:8.2f} Hz  -> {path}")
    print()
    print(f"Wrote staging tree at: {out_dir / 'samples'}")


def push_to_teensy(out_dir: Path) -> None:
    """Invoke tools/push_to_teensy.ps1 to copy the staged samples dir to
    the device (drive letter or MTP).
    """
    if os.name != "nt":
        print("--push is Windows-only (Shell.Application MTP path is "
              "Windows-specific). Drag the folder onto your card "
              "manually on this platform.", file=sys.stderr)
        sys.exit(2)
    samples_dir = out_dir / "samples"
    if not samples_dir.is_dir():
        print(f"Staging dir not found: {samples_dir}", file=sys.stderr)
        sys.exit(1)
    script = Path(__file__).parent / "push_to_teensy.ps1"
    if not script.is_file():
        print(f"push helper not found: {script}", file=sys.stderr)
        sys.exit(1)
    cmd = [
        "powershell.exe",
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", str(script),
        "-SourcePath", str(samples_dir),
    ]
    print(f"Running: {' '.join(cmd)}")
    sys.stdout.flush()  # so PowerShell's stdout interleaves correctly
    result = subprocess.run(cmd)
    sys.exit(result.returncode)


# ----------------------------------------------------------------------------
# Mode: source-dir — convert/rename a user-provided folder of audio files
# ----------------------------------------------------------------------------
#
# Salamander Grand Piano V3 ships its samples named like:
#   A0v1.ogg, A0v2.ogg, ..., A0v16.ogg, A1v1.ogg, ...
# where v8 is roughly mf velocity. Other piano packs use various
# conventions; we extract the note name with a permissive regex that
# pulls the first <letter>[#b]?<octave> sequence from the basename.
#
# Conversion via ffmpeg: -ar 48000 -ac 2 -c:a pcm_s16le matches the
# slot's required 48 kHz / 16-bit signed PCM / stereo input.

# Filename pattern: a note letter (A-G), optional accidental (#/b), and
# octave (possibly negative). Anchored to a word boundary on the left
# so we don't match the 'A' in "Piano".
_NOTE_RE = re.compile(r'(?:^|[_\-\.\s])([A-Ga-g])(#|b)?(-?\d+)')

# Release-sample filename detector. Matches:
#   - "rel<N>.flac"      (Salamander Grand Piano V3 SFZ build — 88 samples,
#                          one per piano key)
#   - "rls<N>.flac"      (some Salamander variants ship 3 round-robin)
#   - "release*.wav"     (verbose convention)
#   - "_release*.wav"    (our own output filename convention)
# Case-insensitive. The "rel" prefix must be followed by a non-letter
# (digit, underscore, dot, etc.) so we don't mis-classify words like
# "relativeXYZ".
_RELEASE_RE = re.compile(r'^_?(rls|rel|release)(?=$|[^a-zA-Z])', re.IGNORECASE)


def parse_note_in_filename(stem: str) -> str | None:
    """Pull a note name out of a filename. Returns None if no match."""
    m = _NOTE_RE.search(stem)
    if not m:
        return None
    letter = m.group(1).upper()
    accidental = m.group(2) or ""
    octave = m.group(3)
    note = f"{letter}{accidental}{octave}"
    try:
        note_to_midi(note)
    except ValueError:
        return None
    return note


def is_release_filename(stem: str) -> bool:
    """True if the basename (no extension) matches a release-sample
    convention (rls*, release*, with optional leading underscore)."""
    return bool(_RELEASE_RE.match(stem))


def _have_ffmpeg() -> bool:
    return shutil.which("ffmpeg") is not None


def _convert_via_soundfile(src: Path, dst: Path) -> bool:
    """Pure-Python fallback when ffmpeg isn't on PATH. Uses soundfile +
    scipy.signal for FLAC/OGG/WAV reading and resampling. Returns True
    on success."""
    try:
        import soundfile as sf
        import numpy as np
        from scipy.signal import resample_poly
        from math import gcd
    except ImportError as e:
        print(f"  soundfile fallback unavailable: {e}", file=sys.stderr)
        return False

    try:
        data, fs = sf.read(str(src), dtype="float32", always_2d=True)
    except Exception as e:
        print(f"  read failed for {src.name}: {e}", file=sys.stderr)
        return False

    # Mono -> stereo by duplicating the channel.
    if data.shape[1] == 1:
        data = np.column_stack([data[:, 0], data[:, 0]])
    elif data.shape[1] > 2:
        data = data[:, :2]

    target_fs = 48000
    if fs != target_fs:
        g = gcd(target_fs, fs)
        up = target_fs // g
        down = fs // g
        # resample_poly works on each channel independently.
        resampled = np.column_stack([
            resample_poly(data[:, c], up, down) for c in range(data.shape[1])
        ])
        data = resampled.astype(np.float32, copy=False)

    # Clip + convert to int16. soundfile writes pcm_16 from float32 input
    # automatically when subtype is set, but we clip explicitly to avoid
    # inf/NaN sneaking through if the source had any.
    np.clip(data, -1.0, 1.0, out=data)

    try:
        sf.write(str(dst), data, target_fs, subtype="PCM_16")
    except Exception as e:
        print(f"  write failed for {dst.name}: {e}", file=sys.stderr)
        return False
    return True


def _convert_one(src: Path, dst: Path) -> bool:
    """Convert one audio file to 48k/stereo/16-bit WAV. Prefers ffmpeg
    (fast, handles every input format) and falls back to a pure-Python
    soundfile+scipy path."""
    if _have_ffmpeg():
        cmd = [
            "ffmpeg",
            "-i", str(src),
            "-ar", "48000",
            "-ac", "2",
            "-c:a", "pcm_s16le",
            "-y",
            "-loglevel", "error",
            str(dst),
        ]
        return subprocess.run(cmd).returncode == 0
    return _convert_via_soundfile(src, dst)


_VELOCITY_RE = re.compile(r'v(\d+)', re.IGNORECASE)


def cmd_source_dir(out_dir: Path, bank: str, source_dir: Path,
                   velocity: int | None, max_samples: int) -> None:
    """Convert a user-supplied folder of audio samples to
    /samples/<bank>/<note>[_v<N>].wav, preserving velocity layers when
    present."""
    if not source_dir.is_dir():
        print(f"source dir not found: {source_dir}", file=sys.stderr)
        sys.exit(1)
    if _have_ffmpeg():
        print("(using ffmpeg for conversion)")
    else:
        try:
            import soundfile  # noqa: F401
            import scipy.signal  # noqa: F401
            print("(ffmpeg not found; using soundfile + scipy fallback)")
        except ImportError:
            print("ffmpeg not found AND soundfile/scipy not installed.", file=sys.stderr)
            print("Either install ffmpeg (https://ffmpeg.org) OR run:", file=sys.stderr)
            print("    pip install soundfile scipy", file=sys.stderr)
            sys.exit(1)

    bank_dir = out_dir / "samples" / bank
    if bank_dir.exists():
        shutil.rmtree(bank_dir)
    bank_dir.mkdir(parents=True)

    audio_exts = {".wav", ".ogg", ".flac", ".mp3", ".aif", ".aiff"}
    candidates = [p for p in source_dir.rglob("*") if p.suffix.lower() in audio_exts]
    if not candidates:
        print(f"no audio files found under {source_dir}", file=sys.stderr)
        sys.exit(1)

    # Partition into pitched note samples and release samples.
    # Release samples are short damper/key-up recordings that play on
    # note-off — Salamander ships rls1.flac/rls2.flac/rls3.flac.
    accepted: list[tuple[str, int | None, Path]] = []
    release_files: list[Path] = []
    for f in candidates:
        if is_release_filename(f.stem):
            # Velocity filter doesn't apply to release samples.
            release_files.append(f)
            continue
        note = parse_note_in_filename(f.stem)
        if note is None:
            continue
        m = _VELOCITY_RE.search(f.stem)
        v: int | None = int(m.group(1)) if m else None
        # If the user passed --velocity, treat it as a filter (single-
        # layer subset). Otherwise keep all layers.
        if velocity is not None:
            if v is None or v != velocity:
                continue
        if v is not None and (v < 1 or v > 16):
            continue
        accepted.append((note, v, f))

    if not accepted and not release_files:
        msg = "no files matched the note-name or release-sample pattern"
        if velocity is not None:
            msg += f" with v{velocity}"
        print(msg + ".", file=sys.stderr)
        sys.exit(1)

    # Sort by (midi, velocity) for stable output and a tidy log.
    accepted.sort(key=lambda t: (note_to_midi(t[0]), t[1] or 0))

    # Cap total file count. With velocity layers this can be 16x the
    # root-note count; default 1024 matches MultisampleSlot::kMaxSamples.
    if len(accepted) > max_samples:
        # Stride evenly so we don't drop one whole velocity layer.
        step = len(accepted) / max_samples
        accepted = [accepted[int(i * step)] for i in range(max_samples)]

    print(f"Converting {len(accepted)} samples to {bank_dir}")
    n_velocity_layered = sum(1 for _, v, _ in accepted if v is not None)
    if n_velocity_layered:
        print(f"  ({n_velocity_layered} files have velocity tags)")
    for note, v, src in accepted:
        if v is None:
            dst = bank_dir / f"{note}.wav"
        else:
            dst = bank_dir / f"{note}_v{v}.wav"
        if not _convert_one(src, dst):
            print(f"  conversion failed for {src}", file=sys.stderr)
            continue
        midi = note_to_midi(note)
        if v is None:
            print(f"  {note:>4}     midi={midi:3d}  {src.name}  ->  {dst.name}")
        else:
            print(f"  {note:>4} v{v:02d} midi={midi:3d}  {src.name}  ->  {dst.name}")

    # Release samples — output as _release1.wav, _release2.wav, ...
    # Salamander Grand Piano V3 ships 88 release samples (one per key).
    # The firmware's release pool is small (kMaxReleaseSamples = 8), so
    # subsample evenly across the source list when there are more than
    # 8. The firmware picks round-robin on each note-off.
    if release_files:
        kReleasePoolSize = 8

        def _natural_key(p: Path) -> tuple:
            # Sort "rel1, rel2, rel10, rel88" naturally instead of
            # "rel1, rel10, rel11, ..., rel2, rel20, ...".
            stem = p.stem.lower()
            digits = re.search(r'(\d+)', stem)
            return (digits is None, int(digits.group(1)) if digits else 0, stem)

        ordered = sorted(release_files, key=_natural_key)
        if len(ordered) > kReleasePoolSize:
            step = len(ordered) / kReleasePoolSize
            chosen = [ordered[int(i * step)] for i in range(kReleasePoolSize)]
            print(f"Picking {kReleasePoolSize} release samples from {len(ordered)} found")
        else:
            chosen = ordered
            print(f"Converting {len(chosen)} release samples")

        for idx, src in enumerate(chosen):
            dst = bank_dir / f"_release{idx + 1}.wav"
            if not _convert_one(src, dst):
                print(f"  conversion failed for {src}", file=sys.stderr)
                continue
            print(f"  release  {src.name}  ->  {dst.name}")

    print()
    print(f"Wrote staging tree at: {out_dir / 'samples'}")


# ----------------------------------------------------------------------------
# Mode: salamander-trimmed (TODO — hosting source not yet wired)
# ----------------------------------------------------------------------------

def cmd_salamander_trimmed(out_dir: Path, bank: str) -> None:
    print("salamander-trimmed auto-download is not yet implemented.", file=sys.stderr)
    print()
    print("Manual workflow until then:", file=sys.stderr)
    print("  1. Download Salamander Grand Piano V3 from", file=sys.stderr)
    print("     https://archive.org/details/SalamanderGrandPianoV3", file=sys.stderr)
    print("     (or the official drop at https://github.com/sfzinstruments).", file=sys.stderr)
    print("  2. Extract the .tar.bz2 to a folder (e.g. C:\\salamander).", file=sys.stderr)
    print("  3. Run:", file=sys.stderr)
    print("       python tools/fetch_samples.py source-dir \\", file=sys.stderr)
    print("           --source-dir C:\\salamander --velocity 8 --push", file=sys.stderr)
    print("     v8 = roughly mf (medium-loud); pick a different velocity layer", file=sys.stderr)
    print("     for a brighter (higher v) or quieter (lower v) sound.", file=sys.stderr)
    print("  4. Re-flash the audio firmware (the file_mode.sh script handles this).", file=sys.stderr)
    sys.exit(2)


# ----------------------------------------------------------------------------
# CLI
# ----------------------------------------------------------------------------

def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["test-tones", "source-dir", "salamander-trimmed"],
                    help="which sample set to assemble")
    ap.add_argument("--out", type=Path,
                    default=Path("c:/tmp/t-dsp-samples"),
                    help="staging directory (default: c:/tmp/t-dsp-samples)")
    ap.add_argument("--bank", default="piano",
                    help="bank name under /samples/ (default: piano)")
    ap.add_argument("--push", action="store_true",
                    help="after generating, copy to a connected Teensy "
                         "(via drive letter or MTP). Windows only.")
    ap.add_argument("--source-dir", type=Path,
                    help="source-dir mode: folder of audio files (wav/ogg/flac/...)")
    ap.add_argument("--velocity", type=int, default=None,
                    help="source-dir mode: filter Salamander-style 'v<N>' "
                         "velocity tag (8 = mf is a good starting point)")
    ap.add_argument("--max-samples", type=int, default=1024,
                    help="cap selected samples (slot's kMaxSamples is 1024; default keeps everything)")
    args = ap.parse_args()

    if args.mode == "test-tones":
        cmd_test_tones(args.out, args.bank)
    elif args.mode == "source-dir":
        if args.source_dir is None:
            print("source-dir mode requires --source-dir <path>", file=sys.stderr)
            sys.exit(2)
        cmd_source_dir(args.out, args.bank, args.source_dir,
                       args.velocity, args.max_samples)
    elif args.mode == "salamander-trimmed":
        cmd_salamander_trimmed(args.out, args.bank)

    if args.push:
        push_to_teensy(args.out)
    else:
        print()
        print(f"To deliver: drag '{args.out / 'samples'}' onto the T-DSP")
        print(f"SD drive in Explorer, OR re-run with --push for auto-copy.")


if __name__ == "__main__":
    main()
