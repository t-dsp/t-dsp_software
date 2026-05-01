#!/usr/bin/env bash
# file_mode.sh — flash the MTP companion firmware so the Teensy 4.1
# appears as a USB drive on Windows / macOS / Linux, give the user a
# chance to copy WAV samples to /samples/<bank>/ on the SD card,
# then re-flash the audio firmware and launch the dev surface.
#
# Usage:
#   tools/file_mode.sh                # full flow
#   tools/file_mode.sh --skip-mtp     # skip MTP flash (already in MTP)
#   tools/file_mode.sh --restore-only # just re-flash audio + launch dev surface
#   tools/file_mode.sh --no-launch    # don't auto-launch the dev surface
#
# Prerequisites:
#   - Teensy 4.1 connected via USB
#   - SD card inserted in the Teensy's BUILT-IN slot (not the audio shield's)
#   - pnpm installed (for the dev surface launch step)
#
# Each upload step requires a manual press of the Teensy's PROGRAM
# button; the script pauses for confirmation between steps.

set -euo pipefail

PIO_BIN="${PIO_BIN:-/c/Users/jaysh/AppData/Roaming/Python/Python313/Scripts/platformio.exe}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MTP_PROJECT="$REPO_ROOT/projects/t-dsp_mtp_disk"
AUDIO_PROJECT="$REPO_ROOT/projects/t-dsp_f32_audio_shield"
DEV_SURFACE="$REPO_ROOT/projects/t-dsp_web_dev"

skip_mtp=0
restore_only=0
launch=1
for arg in "$@"; do
    case "$arg" in
        --skip-mtp)     skip_mtp=1 ;;
        --restore-only) restore_only=1 ;;
        --no-launch)    launch=0 ;;
        -h|--help)
            sed -n '2,18p' "${BASH_SOURCE[0]}"
            exit 0
            ;;
        *)
            echo "unknown option: $arg" >&2
            exit 2
            ;;
    esac
done

step()   { echo; echo "================================================================"; echo "  $*"; echo "================================================================"; }
prompt() { echo; read -p "$* (press Enter) " _ </dev/tty; }

if [ ! -x "$PIO_BIN" ] && ! command -v "$PIO_BIN" >/dev/null 2>&1; then
    echo "PlatformIO not found at $PIO_BIN" >&2
    echo "Set PIO_BIN to its full path and re-run." >&2
    exit 1
fi

# ----- Step 1: flash MTP firmware -----
if [ "$restore_only" -eq 0 ] && [ "$skip_mtp" -eq 0 ]; then
    step "Step 1/4 — flashing MTP companion firmware"
    "$PIO_BIN" run --project-dir "$MTP_PROJECT" --target upload || true
    echo
    echo ">>> If asked, press the PROGRAM button on the Teensy."
    prompt "Wait until the Teensy reboots, then press Enter"
fi

# ----- Step 2: copy files -----
if [ "$restore_only" -eq 0 ]; then
    step "Step 2/4 — copy your sample WAVs"
    cat <<'EOF'
The Teensy should now appear in File Explorer / Finder as a drive
named "T-DSP SD".

Recommended layout:
  /samples/piano/A0.wav
  /samples/piano/C4.wav
  /samples/piano/F#3.wav
  ...

File format: 48 kHz, 16-bit signed PCM, stereo.
Filename = note name (parsed by NoteParser; sharps and flats both ok).

Eject the drive cleanly before continuing so the SD card flushes.
EOF
    prompt "Press Enter once you're done copying"
fi

# ----- Step 3: flash audio firmware -----
step "Step 3/4 — flashing audio firmware (back to the synth)"
"$PIO_BIN" run --project-dir "$AUDIO_PROJECT" --target upload || true
echo
echo ">>> If asked, press the PROGRAM button on the Teensy."
prompt "Wait until the Teensy reboots, then press Enter"

# ----- Step 4: launch dev surface -----
if [ "$launch" -eq 1 ]; then
    step "Step 4/4 — launching the dev surface"
    if [ ! -d "$DEV_SURFACE/node_modules" ]; then
        echo "node_modules missing in $DEV_SURFACE — running 'pnpm install' first..."
        (cd "$DEV_SURFACE" && pnpm install)
    fi
    cd "$DEV_SURFACE"
    exec pnpm app:dev
else
    step "Done — dev surface launch skipped (--no-launch)"
    echo "To launch manually: cd $DEV_SURFACE && pnpm app:dev"
fi
