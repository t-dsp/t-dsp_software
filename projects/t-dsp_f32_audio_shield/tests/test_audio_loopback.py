"""
test_audio_loopback.py -- THD/SNR/channel-separation regression test.

Pytest port of the standalone `loopback_test.py` (lives one project up
in `teensy_usb_audio_tac5212/`). This version:

  * Uses pytest fixtures for device discovery (and skips if no audio).
  * Splits the single end-to-end measurement into named test cases so
    one failure (e.g. THD high) doesn't mask others (e.g. crosstalk).
  * Captures expected ranges that the user has already validated as
    "good audio" on this hardware.

Hardware requirement: a loopback cable from the codec's OUT1/OUT2 to
its IN1+/IN2+ (single-ended). The firmware should be in pass-through
or have ch/01 and ch/02 routed to main with unity gain (the redesign
should make that the default; if it doesn't, set fader to 0 dB / 0.75
in a fixture before each test in this file).

Expected ranges (from the user's known-good measurements):
  * Fundamental level: -7 .. -5 dBFS for a -6 dBFS input
  * THD (h2..h10):     better than -65 dB  (< 0.06 %)
  * Noise floor:       better than -100 dBFS median
  * Channel separation: > 70 dB

If a test fails, the print() output gives you the actual value; copy
that into the assertion bound if the new firmware genuinely shifted
the operating point in an expected direction (and update the docstring).
"""
from __future__ import annotations

import pytest

try:
    import numpy as np
    import sounddevice as sd
    from scipy import signal
    HAVE_AUDIO = True
except ImportError:
    HAVE_AUDIO = False


pytestmark = [
    pytest.mark.hardware,
    pytest.mark.audio,
    pytest.mark.skipif(not HAVE_AUDIO, reason="numpy/scipy/sounddevice not installed"),
]


# Test parameters — match the standalone loopback_test.py defaults.
TONE_HZ    = 1000.0
TONE_DBFS  = -6.0
DURATION_S = 4.0
TRIM_S     = 0.5      # discard front edge to skip USB sync transient


@pytest.fixture(scope="module")
def loopback_capture(teensy_audio):
    """Play a 1 kHz sine on L (R silent), capture both channels.

    Module-scoped: one capture serves all the metric tests below, so
    the suite runs in ~5 seconds instead of one capture per metric."""
    fs = teensy_audio["fs"]
    in_idx = teensy_audio["in"]
    out_idx = teensy_audio["out"]

    n = int(fs * DURATION_S)
    t = np.arange(n) / fs
    amp = 10 ** (TONE_DBFS / 20)
    sig = np.zeros((n, 2), dtype="float32")
    sig[:, 0] = (amp * np.sin(2 * np.pi * TONE_HZ * t)).astype("float32")

    sd.default.samplerate = fs
    rec = sd.playrec(sig, channels=2, device=(in_idx, out_idx),
                     dtype="float32")
    sd.wait()

    trim = int(fs * TRIM_S)
    return {
        "fs": fs,
        "L": rec[trim:, 0].copy(),
        "R": rec[trim:, 1].copy(),
    }


def _spectrum(x: "np.ndarray", fs: int):
    """Hann-windowed FFT, amp-corrected for window coherent gain.
    Returns (freqs, magnitudes_in_linear_amplitude)."""
    n = x.size
    w = signal.windows.hann(n)
    cg = 0.5  # Hann coherent gain
    spec = np.fft.rfft(x * w) / (n * cg)
    mag = np.abs(spec) * 2  # rfft folds negative freqs
    freqs = np.fft.rfftfreq(n, 1 / fs)
    return freqs, mag


def _f0_dbfs(x: "np.ndarray", fs: int, expected_hz: float) -> tuple[float, float]:
    """Return (f0_freq_hz, f0_dbfs) for the peak near expected_hz."""
    freqs, mag = _spectrum(x, fs)
    band = (freqs > expected_hz - 1) & (freqs < expected_hz + 1)
    if not band.any():
        return float("nan"), float("-inf")
    idx = np.argmax(mag * band)
    f0_amp = mag[idx]
    return float(freqs[idx]), float(20 * np.log10(max(f0_amp, 1e-30)))


def _thd_db(x: "np.ndarray", fs: int, expected_hz: float) -> float:
    """THD from h2..h10, in dB relative to fundamental."""
    n = x.size
    freqs, mag = _spectrum(x, fs)
    band = (freqs > expected_hz - 1) & (freqs < expected_hz + 1)
    f0_amp = mag[np.argmax(mag * band)]
    harmonic_amps = []
    for k in range(2, 11):
        target = expected_hz * k
        if target >= fs / 2:
            break
        bin_target = int(round(target * n / fs))
        lo = max(0, bin_target - 5)
        hi = min(len(mag), bin_target + 5 + 1)
        harmonic_amps.append(np.max(mag[lo:hi]))
    thd_lin = np.sqrt(np.sum(np.array(harmonic_amps) ** 2)) / max(f0_amp, 1e-30)
    return 20 * np.log10(max(thd_lin, 1e-30))


def _noise_floor_dbfs(x: "np.ndarray", fs: int, expected_hz: float) -> float:
    """Median spectrum bin outside the fundamental + first 10 harmonics
    (+/-50 Hz neighborhood) and below 20 Hz rumble."""
    n = x.size
    freqs, mag = _spectrum(x, fs)
    notch = np.ones_like(mag, dtype=bool)
    for k in range(1, 11):
        target = expected_hz * k
        if target >= fs / 2:
            break
        notch &= ~((freqs > target - 50) & (freqs < target + 50))
    notch &= freqs > 20
    return float(20 * np.log10(max(np.median(mag[notch]), 1e-30)))


# ---------------------------------------------------------------------------
# Test cases
# ---------------------------------------------------------------------------

def test_left_fundamental_level(loopback_capture):
    """L channel fundamental should land within +/-1 dB of -6 dBFS.

    USB Audio shared-mode adds the OS volume slider and the firmware's
    own gain stage. The user reported -6 dBFS round-trip on
    known-good hardware; deviation > 1 dB means a gain stage changed."""
    f0_hz, f0_dbfs = _f0_dbfs(loopback_capture["L"], loopback_capture["fs"], TONE_HZ)
    print(f"\nL fundamental: {f0_hz:.2f} Hz @ {f0_dbfs:+.2f} dBFS")
    assert abs(f0_hz - TONE_HZ) < 1.0, f"fundamental at {f0_hz} Hz, expected {TONE_HZ}"
    assert abs(f0_dbfs - TONE_DBFS) < 1.0, \
        f"fundamental {f0_dbfs:+.2f} dBFS deviates > 1 dB from {TONE_DBFS:+.2f}"


def test_left_thd(loopback_capture):
    """THD (h2..h10) better than -65 dB.

    User-validated baseline. -65 dB ~ 0.06% which is consistent with
    a TAC5212 + alex6679 USB float32 path running shared-mode."""
    thd = _thd_db(loopback_capture["L"], loopback_capture["fs"], TONE_HZ)
    print(f"\nL THD (h2..h10): {thd:+.2f} dB")
    assert thd < -65.0, f"THD {thd:+.2f} dB worse than -65 dB threshold"


def test_left_noise_floor(loopback_capture):
    """Median noise floor better than -100 dBFS.

    Loose bound -- the analog input stage and shared-mode dithering
    both contribute. < -100 dBFS is the user's known-good baseline."""
    nf = _noise_floor_dbfs(loopback_capture["L"], loopback_capture["fs"], TONE_HZ)
    print(f"\nL noise floor (median): {nf:+.2f} dBFS")
    assert nf < -100.0, f"noise floor {nf:+.2f} dBFS worse than -100 dBFS"


def test_left_snr(loopback_capture):
    """Fundamental vs median noise -- SNR > 90 dB is the bar."""
    fs = loopback_capture["fs"]
    _, f0_dbfs = _f0_dbfs(loopback_capture["L"], fs, TONE_HZ)
    nf = _noise_floor_dbfs(loopback_capture["L"], fs, TONE_HZ)
    snr = f0_dbfs - nf
    print(f"\nL SNR: {snr:+.2f} dB")
    assert snr > 90.0, f"SNR {snr:+.2f} dB below 90 dB target"


def test_channel_separation(loopback_capture):
    """L drives a tone; R input is open. R should be at least 70 dB
    quieter at the fundamental than L.

    User-validated baseline. < 70 dB usually points at PCB crosstalk
    or a bad ground return on the loopback cable, NOT a firmware bug
    -- but we want to catch any regressions either way."""
    fs = loopback_capture["fs"]
    L = loopback_capture["L"]
    R = loopback_capture["R"]
    L_f0 = np.abs(np.fft.rfft(L * signal.windows.hann(L.size))).max()
    R_f0 = np.abs(np.fft.rfft(R * signal.windows.hann(R.size))).max()
    sep_db = 20 * np.log10(max(L_f0, 1e-30) / max(R_f0, 1e-30))
    print(f"\nChannel separation (L drive, R silent): {sep_db:.1f} dB")
    assert sep_db > 70.0, f"channel separation only {sep_db:.1f} dB"
