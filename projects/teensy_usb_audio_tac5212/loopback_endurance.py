"""
loopback_endurance.py -- 1-minute chord stress test for the F32 24-bit
loopback rig. Detects clicks, dropouts, RMS drift, IMD products, and any
weirdness that only shows up on long captures.

Run:
    python loopback_endurance.py

Output:
  - capture.wav  saved alongside this script (open in any audio editor to
    listen for clicks / dropouts the analysis might miss)
  - per-second RMS table (flat number = stable; drift indicates buffer
    issues or thermal effects)
  - sample-to-sample slew anomaly count (clicks = sudden sample jumps)
  - IMD spectrum check (sum / difference frequencies between chord tones)
  - DC offset trend per second (should stay near zero)

Default signal: C major chord (C4 / E4 / G4 / C5) at -12 dBFS each, summed.
Pick `IMD_MODE = True` at the top of the script to swap in CCIF IMD test
(19 Hz + 20 kHz at 4:1 ratio) instead -- that's a stricter engineering
test of nonlinearity but less interesting to listen to.
"""
from __future__ import annotations

import os
import sys
import numpy as np
import sounddevice as sd
from scipy import signal
from scipy.io import wavfile

FS                  = 48000
DURATION_S          = 60.0
TRIM_S              = 1.0           # discard front + back to skip USB sync transient
HOST_API_PREFERENCE = "Windows WASAPI"
DEVICE_NAME_HINT    = "teensy"
WAV_OUT             = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                   "capture.wav")

# Pick one signal mode.
IMD_MODE = False

# C major chord (C4, E4, G4, C5) at -12 dBFS each. Peak instantaneous level
# in the worst case is 4 * 0.25 = 1.0, but with arbitrary phase relationships
# the actual peak rarely exceeds ~0.6 -- safe headroom against clipping.
CHORD_HZ        = [261.63, 329.63, 392.00, 523.25]
CHORD_TONE_DBFS = -12.0

# CCIF IMD: 19 Hz + 20 kHz at 4:1 ratio. Lights up any low-order
# nonlinearity (sum and difference products at 19 Hz +/- N*20 kHz).
IMD_LO_HZ       = 19.0
IMD_HI_HZ       = 20000.0
IMD_LO_DBFS     = -6.0
IMD_HI_DBFS     = -18.0    # 12 dB below = 4:1 amplitude ratio


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
    """Try WASAPI exclusive first (bypasses the Windows shared-mode mixer
    for true 24-bit transport). If something else has the device open,
    fall back to shared mode so the test still runs -- shared mode caps
    us at 16-bit float internally, which is plenty for click/dropout
    detection but limits the THD/noise-floor precision."""
    sd.default.samplerate = FS
    try:
        excl = sd.WasapiSettings(exclusive=True)
        rec = sd.playrec(sig, channels=2, device=(in_dev, out_dev),
                         dtype="float32",
                         extra_settings=(excl, excl))
        sd.wait()
        print("(WASAPI exclusive mode)")
    except sd.PortAudioError:
        print("(WASAPI exclusive busy; falling back to shared mode --"
              " device probably has another stream open)")
        rec = sd.playrec(sig, channels=2, device=(in_dev, out_dev),
                         dtype="float32")
        sd.wait()
    return rec


def build_signal(n: int) -> np.ndarray:
    t = np.arange(n) / FS
    sig = np.zeros((n, 2), dtype="float32")
    if IMD_MODE:
        amp_lo = 10 ** (IMD_LO_DBFS / 20)
        amp_hi = 10 ** (IMD_HI_DBFS / 20)
        wave = (amp_lo * np.sin(2 * np.pi * IMD_LO_HZ * t) +
                amp_hi * np.sin(2 * np.pi * IMD_HI_HZ * t))
        sig[:, 0] = wave.astype("float32")
        sig[:, 1] = wave.astype("float32")
        print(f"Signal: CCIF IMD ({IMD_LO_HZ:.0f} Hz @ {IMD_LO_DBFS:+.0f} dBFS"
              f" + {IMD_HI_HZ:.0f} Hz @ {IMD_HI_DBFS:+.0f} dBFS), both channels")
    else:
        amp = 10 ** (CHORD_TONE_DBFS / 20)
        wave = np.zeros(n, dtype="float64")
        for f in CHORD_HZ:
            wave += amp * np.sin(2 * np.pi * f * t)
        sig[:, 0] = wave.astype("float32")
        sig[:, 1] = wave.astype("float32")
        print(f"Signal: C major chord ({len(CHORD_HZ)} tones at"
              f" {CHORD_TONE_DBFS:+.0f} dBFS each, summed), both channels")


    peak = float(np.abs(sig).max())
    print(f"Signal peak amplitude: {peak:.4f} ({20*np.log10(peak):+.2f} dBFS)")
    return sig


# ---------- analysis ----------

def per_second_stats(rec: np.ndarray) -> None:
    """Slice the capture into 1-second windows and report RMS + DC offset
    per channel per second. Flat RMS = no drift / no buffer underruns
    that would have caused level changes. DC offset trending = analog DC
    coupling problem somewhere."""
    n_per_sec = FS
    n_secs    = rec.shape[0] // n_per_sec
    print(f"\nPER-SECOND STATS ({n_secs} seconds, both channels):")
    print(f"  {'sec':>4s}  {'L RMS dBFS':>11s}  {'R RMS dBFS':>11s}"
          f"  {'L DC':>9s}  {'R DC':>9s}")
    rms_l_history = []
    rms_r_history = []
    for i in range(n_secs):
        chunk = rec[i * n_per_sec : (i + 1) * n_per_sec]
        L = chunk[:, 0]
        R = chunk[:, 1]
        rms_l = np.sqrt(np.mean(L * L))
        rms_r = np.sqrt(np.mean(R * R))
        dc_l  = float(np.mean(L))
        dc_r  = float(np.mean(R))
        rms_l_history.append(rms_l)
        rms_r_history.append(rms_r)
        # Print every 5th second to keep output short
        if i % 5 == 0 or i == n_secs - 1:
            rms_l_db = 20 * np.log10(max(rms_l, 1e-30))
            rms_r_db = 20 * np.log10(max(rms_r, 1e-30))
            print(f"  {i:>4d}  {rms_l_db:>+11.3f}  {rms_r_db:>+11.3f}"
                  f"  {dc_l:>+9.5f}  {dc_r:>+9.5f}")

    rms_l_arr = np.array(rms_l_history)
    rms_r_arr = np.array(rms_r_history)
    print(f"\n  L RMS range over {n_secs}s: "
          f"{20*np.log10(rms_l_arr.min()):+.3f} dBFS .. "
          f"{20*np.log10(rms_l_arr.max()):+.3f} dBFS"
          f"  (delta {20*np.log10(rms_l_arr.max()/rms_l_arr.min()):.4f} dB)")
    print(f"  R RMS range over {n_secs}s: "
          f"{20*np.log10(rms_r_arr.min()):+.3f} dBFS .. "
          f"{20*np.log10(rms_r_arr.max()):+.3f} dBFS"
          f"  (delta {20*np.log10(rms_r_arr.max()/rms_r_arr.min()):.4f} dB)")


def detect_clicks(rec: np.ndarray, label: str) -> None:
    """Click = sample-to-sample jump exceeding what continuous audio could
    produce. For a band-limited signal at fs=48k the worst-case slew is
    bounded by the highest-frequency component's peak slope. Anything
    sharper is non-musical (USB packet boundary, codec underrun, ESD spike).

    We compute the slew distribution, then flag any sample where the slew
    exceeds 6x the 99.9th-percentile slew. That's an aggressive threshold
    that should miss real audio but catch obvious clicks."""
    diff = np.abs(np.diff(rec))
    p999 = np.percentile(diff, 99.9)
    threshold = 6.0 * p999
    clicks = np.where(diff > threshold)[0]
    print(f"\n  [{label}] slew p99.9 = {p999:.6f}, "
          f"threshold = {threshold:.6f}")
    print(f"  [{label}] anomalous-slew samples: {clicks.size}"
          f" (out of {rec.size}, "
          f"{100 * clicks.size / max(rec.size, 1):.6f}%)")
    if clicks.size and clicks.size <= 20:
        print(f"  [{label}] timestamps (s): "
              + ", ".join(f"{c/FS:.3f}" for c in clicks[:20]))
    elif clicks.size:
        # Print first 10 + sample times of clusters
        print(f"  [{label}] first 10 timestamps (s): "
              + ", ".join(f"{c/FS:.3f}" for c in clicks[:10]))


def imd_check(rec: np.ndarray, label: str) -> None:
    """If CHORD_HZ has multiple tones, the loopback should produce only
    those tones in the spectrum. Any tone at frequencies like (f1+f2),
    (f1-f2), (2*f1-f2) etc. is intermodulation distortion -- a strong
    indicator of analog stage nonlinearity (or upstream clock issues
    that mix tones together)."""
    n = rec.size
    w = signal.windows.hann(n)
    cg = 0.5
    spec = np.fft.rfft(rec * w) / (n * cg)
    mag  = np.abs(spec) * 2
    freqs = np.fft.rfftfreq(n, 1 / FS)

    # Reference: highest fundamental amplitude
    fund_amps = []
    sources = CHORD_HZ if not IMD_MODE else [IMD_LO_HZ, IMD_HI_HZ]
    for f in sources:
        bin_target = int(round(f * n / FS))
        lo = max(0, bin_target - 5)
        hi = min(len(mag), bin_target + 5 + 1)
        fund_amps.append(np.max(mag[lo:hi]))
    f0_amp = max(fund_amps)
    f0_dbfs = 20 * np.log10(max(f0_amp, 1e-30))

    # IMD products of interest (sums, differences, second-order combinations)
    imd_targets = []
    for i, fa in enumerate(sources):
        for j, fb in enumerate(sources):
            if j <= i: continue
            imd_targets.append(("sum",  fa + fb))
            imd_targets.append(("diff", abs(fa - fb)))
            imd_targets.append(("2a-b", abs(2 * fa - fb)))
            imd_targets.append(("a+2b", fa + 2 * fb))
    print(f"\n  [{label}] strongest fundamental: {f0_dbfs:+.2f} dBFS")
    print(f"  [{label}] IMD products (relative to strongest fundamental):")
    seen = set()
    for kind, f in sorted(imd_targets, key=lambda x: x[1]):
        if f <= 0 or f >= FS / 2: continue
        if f in seen: continue
        seen.add(f)
        # Skip if exactly equal to a fundamental (not IMD, just the source)
        if any(abs(f - s) < 1.0 for s in sources): continue
        bin_target = int(round(f * n / FS))
        lo = max(0, bin_target - 5)
        hi = min(len(mag), bin_target + 5 + 1)
        a = np.max(mag[lo:hi])
        a_db = 20 * np.log10(max(a, 1e-30))
        rel = a_db - f0_dbfs
        marker = " ***" if rel > -80 else ""
        print(f"    {kind:>5s}  {f:8.1f} Hz: {a_db:+8.2f} dBFS"
              f"  ({rel:+7.2f} dB rel){marker}")


def main() -> int:
    in_idx, out_idx = find_teensy_devices()
    print(f"Teensy WASAPI: in={in_idx}, out={out_idx}")
    print(f"Test: {DURATION_S}s capture at fs={FS} (24-bit firmware path)")
    print(f"WAV output: {WAV_OUT}")
    print()

    n = int(FS * DURATION_S)
    sig = build_signal(n)

    print(f"\nPlaying {DURATION_S}s. Sit tight...")
    rec = play_record(out_idx, in_idx, sig)
    print("Capture complete.")

    # Save WAV for the user to listen to. Keep it as float32 so any
    # quantization is purely from the loopback chain (not from us
    # rounding back to int16).
    print(f"\nWriting {WAV_OUT}")
    wavfile.write(WAV_OUT, FS, rec.astype("float32"))

    # Trim front + back to skip stream warm-up / shutdown transients.
    trim = int(FS * TRIM_S)
    rec_t = rec[trim:-trim] if rec.shape[0] > 2 * trim else rec

    per_second_stats(rec_t)

    print("\nCLICK / GLITCH DETECTION (per channel):")
    detect_clicks(rec_t[:, 0], "L")
    detect_clicks(rec_t[:, 1], "R")

    print("\nIMD ANALYSIS (per channel):")
    imd_check(rec_t[:, 0], "L")
    imd_check(rec_t[:, 1], "R")

    print()
    print("Done. Open capture.wav in an audio editor and listen end-to-end")
    print("for clicks/pops the analysis might have missed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
