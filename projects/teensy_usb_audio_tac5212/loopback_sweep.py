"""
loopback_sweep.py -- frequency-response sweep + idle-noise-floor measurement
for the teensy_usb_audio_tac5212 project's loopback rig.

Run after loopback_test.py confirms the basic round-trip works:

    python loopback_sweep.py

Tests:
  1. Idle noise floor   -- record while playing silence; characterizes the
                           absolute analog + USB quantization noise floor.
  2. Frequency response -- sweep 20 Hz to 20 kHz at -6 dBFS, print amplitude
                           at each tone (relative to the 1 kHz reference).
  3. THD vs frequency   -- harmonic distortion at 100 Hz / 1 kHz / 10 kHz.

Requires: sounddevice, numpy, scipy. Same WASAPI Teensy device as
loopback_test.py.
"""
from __future__ import annotations

import sys
import numpy as np
import sounddevice as sd
from scipy import signal

FS                  = 44100
TONE_DBFS           = -6.0
TONE_DUR_S          = 1.5
TRIM_S              = 0.4
NOISE_DUR_S         = 4.0
HOST_API_PREFERENCE = "Windows WASAPI"
DEVICE_NAME_HINT    = "teensy"

# Third-octave-ish sweep, plus interesting endpoints.
SWEEP_FREQS_HZ = [
    20, 31.5, 50, 80, 125, 200, 315, 500, 800,
    1000, 1600, 2500, 4000, 6300, 10000, 14000, 17000, 20000,
]

THD_FREQS_HZ = [100.0, 1000.0, 10000.0]


def find_teensy_devices() -> tuple[int, int]:
    in_idx = out_idx = None
    for i, d in enumerate(sd.query_devices()):
        api = sd.query_hostapis(d["hostapi"])["name"]
        if api != HOST_API_PREFERENCE or DEVICE_NAME_HINT not in d["name"].lower():
            continue
        if d["max_output_channels"] >= 2 and out_idx is None: out_idx = i
        if d["max_input_channels"]  >= 2 and in_idx  is None: in_idx  = i
    if in_idx is None or out_idx is None:
        raise RuntimeError("Could not find WASAPI Teensy Audio in+out devices")
    return in_idx, out_idx


def play_record(out_dev: int, in_dev: int, sig: np.ndarray) -> np.ndarray:
    sd.default.samplerate = FS
    rec = sd.playrec(sig, channels=2, device=(in_dev, out_dev), dtype="float32")
    sd.wait()
    return rec


# ---------- analysis helpers ----------

def windowed_fft(x: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Returns (freqs, mag) with Hann window + amplitude correction so a
    pure tone at amp A reads back as A in the magnitude spectrum."""
    n = x.size
    w = signal.windows.hann(n)
    cg = 0.5  # Hann coherent gain
    spec = np.fft.rfft(x * w) / (n * cg)
    return np.fft.rfftfreq(n, 1 / FS), np.abs(spec) * 2


def fundamental_amp_dbfs(rec: np.ndarray, expected_hz: float) -> float:
    freqs, mag = windowed_fft(rec)
    band = (freqs > expected_hz * 0.98) & (freqs < expected_hz * 1.02)
    if not band.any():
        return float("-inf")
    a = mag[band].max()
    return 20 * np.log10(max(a, 1e-30))


def thd_db(rec: np.ndarray, expected_hz: float) -> tuple[float, float]:
    """Returns (THD in dB, fundamental in dBFS)."""
    n = rec.size
    freqs, mag = windowed_fft(rec)
    band = (freqs > expected_hz * 0.98) & (freqs < expected_hz * 1.02)
    if not band.any():
        return float("-inf"), float("-inf")
    f0_amp = mag[band].max()
    f0_dbfs = 20 * np.log10(max(f0_amp, 1e-30))
    harmonic_amps = []
    for k in range(2, 11):
        target = expected_hz * k
        if target >= FS / 2: break
        bin_target = int(round(target * n / FS))
        lo = max(0, bin_target - 5)
        hi = min(len(mag), bin_target + 5 + 1)
        harmonic_amps.append(np.max(mag[lo:hi]))
    thd_lin = np.sqrt(np.sum(np.array(harmonic_amps) ** 2)) / max(f0_amp, 1e-30)
    return 20 * np.log10(max(thd_lin, 1e-30)), f0_dbfs


def broadband_noise_dbfs(rec: np.ndarray, exclude_hz: list[float] | None = None) -> float:
    """RMS noise in dBFS over the full audio band (20 Hz - Nyquist), excluding
    +/-50 Hz around any frequencies in exclude_hz."""
    freqs, mag = windowed_fft(rec)
    # Hann ENBW correction: 1.5 bins of equivalent noise bandwidth per FFT bin.
    # Convert magnitude (peak) -> RMS power per bin: (mag/sqrt(2))^2 / 1.5
    psd = (mag / np.sqrt(2)) ** 2 / 1.5
    keep = freqs > 20
    for f in exclude_hz or []:
        keep &= ~((freqs > f - 50) & (freqs < f + 50))
        # also exclude harmonics
        for k in range(2, 11):
            if f * k >= FS / 2: break
            keep &= ~((freqs > f * k - 50) & (freqs < f * k + 50))
    total_power = psd[keep].sum()
    rms = np.sqrt(total_power)
    return 20 * np.log10(max(rms, 1e-30))


# ---------- tests ----------

def test_idle_noise(out_dev: int, in_dev: int) -> None:
    print("-" * 72)
    print(f"IDLE NOISE FLOOR ({NOISE_DUR_S}s of host-side silence)")
    print("-" * 72)
    n = int(FS * NOISE_DUR_S)
    sig = np.zeros((n, 2), dtype="float32")
    rec = play_record(out_dev, in_dev, sig)
    trim = int(FS * TRIM_S)
    L = rec[trim:, 0]
    R = rec[trim:, 1]
    L_dbfs = broadband_noise_dbfs(L)
    R_dbfs = broadband_noise_dbfs(R)
    print(f"  L broadband noise floor: {L_dbfs:+7.2f} dBFS")
    print(f"  R broadband noise floor: {R_dbfs:+7.2f} dBFS")

    # Look for any spectral lines sticking above the noise floor (mains hum,
    # USB-related artifacts, codec-clock leakage, etc.).
    freqs, mag = windowed_fft(L)
    keep = freqs > 20
    median = np.median(mag[keep])
    threshold = median * 30  # 30x = ~30 dB above median
    spikes = np.where((mag > threshold) & keep)[0]
    if spikes.size:
        print("  Spectral spikes >30 dB above L noise floor:")
        # cluster nearby bins
        seen = set()
        for idx in spikes:
            f = freqs[idx]
            bucket = round(f / 5) * 5  # 5 Hz buckets
            if bucket in seen: continue
            seen.add(bucket)
            print(f"    {f:8.1f} Hz @ {20 * np.log10(mag[idx]):+7.2f} dBFS")
            if len(seen) > 12:
                print("    (more spikes truncated)")
                break
    else:
        print("  No spectral spikes above ~30 dB threshold (clean spectrum)")
    print()


def test_freq_response(out_dev: int, in_dev: int) -> None:
    print("-" * 72)
    print(f"FREQUENCY RESPONSE -- L at {TONE_DBFS} dBFS, {TONE_DUR_S}s per tone")
    print("-" * 72)
    print(f"  {'Freq (Hz)':>10s}  {'L amp (dBFS)':>14s}  {'dB vs 1 kHz':>12s}")
    results = {}
    for f in SWEEP_FREQS_HZ:
        n = int(FS * TONE_DUR_S)
        t = np.arange(n) / FS
        amp = 10 ** (TONE_DBFS / 20)
        sig = np.zeros((n, 2), dtype="float32")
        sig[:, 0] = (amp * np.sin(2 * np.pi * f * t)).astype("float32")
        rec = play_record(out_dev, in_dev, sig)
        trim = int(FS * TRIM_S)
        L = rec[trim:, 0]
        amp_dbfs = fundamental_amp_dbfs(L, f)
        results[f] = amp_dbfs
    ref = results.get(1000, TONE_DBFS)
    for f, dbfs in results.items():
        delta = dbfs - ref
        print(f"  {f:>10.1f}  {dbfs:>+14.2f}  {delta:>+12.2f}")
    print()


def test_thd_vs_freq(out_dev: int, in_dev: int) -> None:
    print("-" * 72)
    print(f"THD vs FREQUENCY -- L at {TONE_DBFS} dBFS, {TONE_DUR_S}s per tone")
    print("-" * 72)
    print(f"  {'Freq (Hz)':>10s}  {'f0 (dBFS)':>12s}  {'THD (dB)':>10s}  {'%':>10s}")
    for f in THD_FREQS_HZ:
        n = int(FS * TONE_DUR_S)
        t = np.arange(n) / FS
        amp = 10 ** (TONE_DBFS / 20)
        sig = np.zeros((n, 2), dtype="float32")
        sig[:, 0] = (amp * np.sin(2 * np.pi * f * t)).astype("float32")
        rec = play_record(out_dev, in_dev, sig)
        trim = int(FS * TRIM_S)
        L = rec[trim:, 0]
        thd, f0 = thd_db(L, f)
        print(f"  {f:>10.1f}  {f0:>+12.2f}  {thd:>+10.2f}  {10**(thd/20)*100:>10.4f}")
    print()


def main() -> int:
    in_idx, out_idx = find_teensy_devices()
    print(f"Teensy WASAPI: in={in_idx}, out={out_idx}")
    print(f"fs={FS}, host volume slider should be at 100% for full-scale tests")
    print()

    test_idle_noise(out_idx, in_idx)
    test_freq_response(out_idx, in_idx)
    test_thd_vs_freq(out_idx, in_idx)
    return 0


if __name__ == "__main__":
    sys.exit(main())
