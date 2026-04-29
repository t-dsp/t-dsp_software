"""
loopback_test.py -- CLI loopback measurement for the teensy_usb_audio_tac5212
project. Plays a known signal through the Teensy USB Audio output, records the
result via the matching USB Audio input (which is the TAC5212 ADC seeing the
loopback cable from OUT1/2 to INxP), FFTs it, and prints THD / SNR / channel
separation.

Run:
    python loopback_test.py

Requires: sounddevice, numpy, scipy. The host's USB volume slider for "Teensy
Audio" should be 100% (the firmware applies it as a multiplier, so non-100%
just attenuates the test signal). ASIO/exclusive mode is not required -- the
script uses WASAPI shared mode at 44.1k, matching the firmware.
"""
from __future__ import annotations

import sys
import numpy as np
import sounddevice as sd
from scipy import signal

FS         = 44100
DURATION_S = 4.0
TONE_HZ    = 1000.0
TONE_DBFS  = -6.0          # leave headroom; alex6679 USB volume slider scales further
TRIM_S     = 0.5           # discard front edge to skip USB sync transient
HOST_API_PREFERENCE = "Windows WASAPI"
DEVICE_NAME_HINT    = "teensy"


def find_teensy_devices() -> tuple[int, int]:
    """Return (input_idx, output_idx) for the WASAPI 'Teensy Audio' device."""
    in_idx = out_idx = None
    for i, d in enumerate(sd.query_devices()):
        api = sd.query_hostapis(d["hostapi"])["name"]
        if api != HOST_API_PREFERENCE:
            continue
        if DEVICE_NAME_HINT not in d["name"].lower():
            continue
        if d["max_output_channels"] >= 2 and out_idx is None:
            out_idx = i
        if d["max_input_channels"] >= 2 and in_idx is None:
            in_idx = i
    if in_idx is None or out_idx is None:
        raise RuntimeError("Could not find WASAPI Teensy Audio in+out devices")
    return in_idx, out_idx


def play_record(out_dev: int, in_dev: int, sig: np.ndarray) -> np.ndarray:
    """sd.playrec returns one buffer; both directions share a clock so the
    captured array aligns sample-for-sample with the played one (modulo USB
    endpoint phase, which we trim off)."""
    sd.default.samplerate = FS
    rec = sd.playrec(sig, channels=2, device=(in_dev, out_dev), dtype="float32")
    sd.wait()
    return rec


def metrics(rec: np.ndarray, expected_hz: float, *, label: str) -> None:
    """Compute fundamental power, THD (h2..h10), noise floor."""
    n  = rec.size
    # Hann window keeps spectral leakage from corrupting the harmonic bins.
    w  = signal.windows.hann(n)
    # Coherent gain of a Hann window is 0.5 -- correct fundamental amplitude
    # back to its un-windowed value so dBFS readings are honest.
    cg = 0.5
    spec = np.fft.rfft(rec * w) / (n * cg)
    mag  = np.abs(spec) * 2  # *2 because rfft folds negative freqs
    freqs = np.fft.rfftfreq(n, 1 / FS)

    # Fundamental = peak bin within +/-1 Hz of expected
    band = (freqs > expected_hz - 1) & (freqs < expected_hz + 1)
    if not band.any():
        print(f"  [{label}] no bin near {expected_hz} Hz")
        return
    f0_idx = np.argmax(mag * band)
    f0     = freqs[f0_idx]
    f0_amp = mag[f0_idx]
    f0_dbfs = 20 * np.log10(max(f0_amp, 1e-30))

    # THD = sqrt(sum h2..h10 amp^2) / h1 amp, in dB
    harmonic_amps = []
    for k in range(2, 11):
        target = expected_hz * k
        if target >= FS / 2:
            break
        # +/- 5 bins around target -- handles small frequency drift
        bin_target = int(round(target * n / FS))
        lo = max(0, bin_target - 5)
        hi = min(len(mag), bin_target + 5 + 1)
        harmonic_amps.append(np.max(mag[lo:hi]))
    thd_lin = np.sqrt(np.sum(np.array(harmonic_amps) ** 2)) / max(f0_amp, 1e-30)
    thd_db  = 20 * np.log10(max(thd_lin, 1e-30))

    # Noise floor: median of bins outside the fundamental and its first 10
    # harmonic +/-50 Hz neighborhoods.
    notch = np.ones_like(mag, dtype=bool)
    for k in range(1, 11):
        target = expected_hz * k
        if target >= FS / 2:
            break
        notch &= ~((freqs > target - 50) & (freqs < target + 50))
    # Also remove DC + sub-20 Hz rumble.
    notch &= freqs > 20
    noise_med = np.median(mag[notch])
    noise_dbfs = 20 * np.log10(max(noise_med, 1e-30))

    snr = f0_dbfs - noise_dbfs

    print(f"  [{label}] f0 = {f0:.3f} Hz @ {f0_dbfs:+.2f} dBFS")
    print(f"  [{label}] THD (h2..h10)   = {thd_db:+.2f} dB  ({thd_lin*100:.4f} %)")
    print(f"  [{label}] median noise    = {noise_dbfs:+.2f} dBFS")
    print(f"  [{label}] SNR (f0 vs med) = {snr:+.2f} dB")


def main() -> int:
    in_idx, out_idx = find_teensy_devices()
    print(f"Teensy WASAPI: in={in_idx}, out={out_idx}")
    print(f"Test: {TONE_HZ} Hz sine at {TONE_DBFS} dBFS, fs={FS}, "
          f"{DURATION_S}s, trimming first {TRIM_S}s")
    print()

    n = int(FS * DURATION_S)
    t = np.arange(n) / FS
    amp = 10 ** (TONE_DBFS / 20)
    # Stereo test signal: L = sine, R = silence. Lets us measure
    # channel separation in the same recording.
    sig = np.zeros((n, 2), dtype="float32")
    sig[:, 0] = (amp * np.sin(2 * np.pi * TONE_HZ * t)).astype("float32")

    rec = play_record(out_idx, in_idx, sig)

    # Trim front (USB warm-up + first audio block transients).
    trim = int(FS * TRIM_S)
    L = rec[trim:, 0]
    R = rec[trim:, 1]

    print("LEFT channel (signal):")
    metrics(L, TONE_HZ, label="L")
    print()
    print("RIGHT channel (silent input -> measures channel separation):")
    metrics(R, TONE_HZ, label="R")

    # Channel separation: how loud is the leak vs. the L fundamental?
    L_f0 = np.abs(np.fft.rfft(L * signal.windows.hann(L.size))).max()
    R_f0 = np.abs(np.fft.rfft(R * signal.windows.hann(R.size))).max()
    sep_db = 20 * np.log10(max(L_f0, 1e-30) / max(R_f0, 1e-30))
    print()
    print(f"Channel separation (L drive, R silent input): {sep_db:.1f} dB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
