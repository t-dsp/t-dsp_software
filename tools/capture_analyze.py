#!/usr/bin/env python3
"""capture_analyze.py — capture the T-DSP digital output over USB and analyze it on the PC.

Pairs with the firmware `@CAP[=<n>]` command (see main.cpp OutCaptureProbe_F32), which
dumps the final DAC-bound F32 samples as:

    [cap] begin <N> rate <R>
    <space-separated floats, 16/line>
    ...
    [cap] end

Reusable dev instrument — works on ANY build (it taps the output bus, not an engine).

As a library:
    from capture_analyze import grab, analyze, save_wav
    x, rate = grab("COM4")                 # capture whatever is currently sounding
    m = analyze(x, rate)                   # dict: rms, peak, centroid_hz, dom_hz, ...
    save_wav(x, rate, "cap.wav")

As a CLI:
    python tools/capture_analyze.py --port COM4 --wav cap.wav --plot cap.png --label organ
    python tools/capture_analyze.py --port COM4 --n 8192        # shorter capture

Timbre check (brightness = spectral centroid): capture a dark note and a bright note,
compare `centroid_hz` — higher = brighter. That's how you verify the OPLL pool's CC#74
(Y) axis, which the outPeak amplitude meter can't see.
"""
import argparse, re, sys, time, wave, struct

try:
    import numpy as np
except ImportError:
    sys.exit("numpy required:  python -m pip install numpy")


def grab(port, n=None, baud=115200, pre=None, settle=0.0, timeout=8.0):
    """Send @CAP (optionally after `pre` commands) and return (samples: np.float32, rate: int)."""
    import serial  # local import so `analyze`/`save_wav` work without pyserial
    s = serial.Serial(port, baud, timeout=0.2)
    try:
        time.sleep(1.0)
        # drain any in-progress output (heartbeat prevents true quiet, so drain a fixed slice)
        t0 = time.time()
        while time.time() - t0 < 1.0:
            s.read(4096)
        for cmd in (pre or []):
            s.write((cmd + "\n").encode())
            time.sleep(0.15)
        if settle:
            time.sleep(settle)
        s.reset_input_buffer()
        s.write((f"@CAP={n}" if n else "@CAP").encode() + b"\n")

        rate, vals, capturing = 48000, [], False
        buf = b""
        end = time.time() + timeout
        while time.time() < end:
            d = s.read(4096)
            if not d:
                continue
            buf += d
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                t = line.decode(errors="replace").strip()
                if t.startswith("[cap] begin"):
                    mrate = re.search(r"rate\s+(\d+)", t)
                    if mrate:
                        rate = int(mrate.group(1))
                    capturing = True
                elif t.startswith("[cap] end"):
                    return np.asarray(vals, dtype=np.float32), rate
                elif capturing and not t.startswith(("alive", "[")):
                    vals.extend(float(x) for x in t.split() if _isnum(x))
        raise TimeoutError("no [cap] end seen — is @CAP supported by this firmware? is sound playing?")
    finally:
        s.close()


def _isnum(tok):
    try:
        float(tok); return True
    except ValueError:
        return False


def analyze(x, rate, verbose=True):
    """Return a metrics dict and (optionally) print a summary. Brightness = centroid_hz."""
    x = np.asarray(x, dtype=np.float64)
    n = len(x)
    if n == 0:
        raise ValueError("empty capture")
    x0 = x - x.mean()                                   # DC-remove for spectral metrics
    rms = float(np.sqrt(np.mean(x0 ** 2)))
    peak = float(np.max(np.abs(x)))
    crest_db = 20 * np.log10(peak / rms) if rms > 0 else float("inf")
    # spectrum
    win = np.hanning(n)
    mag = np.abs(np.fft.rfft(x0 * win))
    freqs = np.fft.rfftfreq(n, 1.0 / rate)
    centroid = float(np.sum(freqs * mag) / np.sum(mag)) if mag.sum() > 0 else 0.0
    dom = float(freqs[int(np.argmax(mag))])
    # rolloff85: freq below which 85% of spectral energy lies (another brightness proxy)
    e = np.cumsum(mag ** 2)
    roll = float(freqs[int(np.searchsorted(e, 0.85 * e[-1]))]) if e[-1] > 0 else 0.0
    m = dict(n=n, rate=rate, dur_ms=1000.0 * n / rate, rms=rms,
             peak=peak, peak_dbfs=20 * np.log10(peak) if peak > 0 else -999.0,
             crest_db=crest_db, centroid_hz=centroid, dom_hz=dom, rolloff85_hz=roll)
    if verbose:
        print(f"  samples   : {n}  ({m['dur_ms']:.0f} ms @ {rate} Hz)")
        print(f"  peak      : {peak:.4f}  ({m['peak_dbfs']:+.1f} dBFS)")
        print(f"  rms       : {rms:.4f}   crest {crest_db:.1f} dB")
        print(f"  dom freq  : {dom:.1f} Hz")
        print(f"  centroid  : {centroid:.1f} Hz   <- brightness (higher = brighter)")
        print(f"  rolloff85 : {roll:.1f} Hz")
    return m


def save_wav(x, rate, path, normalize=True):
    x = np.asarray(x, dtype=np.float64)
    peak = np.max(np.abs(x)) or 1.0
    y = x / peak if normalize else np.clip(x, -1, 1)
    pcm = (np.clip(y, -1, 1) * 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(int(rate))
        w.writeframes(pcm.tobytes())
    print(f"  wrote {path}  ({len(x)} samples, {'normalized' if normalize else 'raw'})")


def plot(x, rate, path):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("  (matplotlib not installed — skipping plot)")
        return
    x = np.asarray(x, dtype=np.float64)
    x0 = x - x.mean()
    mag = np.abs(np.fft.rfft(x0 * np.hanning(len(x))))
    freqs = np.fft.rfftfreq(len(x), 1.0 / rate)
    fig, (a, b) = plt.subplots(2, 1, figsize=(9, 6))
    t = np.arange(len(x)) / rate * 1000.0
    a.plot(t[:2000], x[:2000], lw=0.6); a.set_title("waveform (first ~45 ms)"); a.set_xlabel("ms")
    b.semilogx(freqs[1:], 20 * np.log10(mag[1:] + 1e-9), lw=0.6)
    b.set_title("spectrum"); b.set_xlabel("Hz"); b.set_ylabel("dB"); b.set_xlim(20, rate / 2)
    fig.tight_layout(); fig.savefig(path, dpi=110)
    print(f"  wrote {path}")


def main():
    ap = argparse.ArgumentParser(description="Capture + analyze the T-DSP digital output.")
    ap.add_argument("--port", default="COM4")
    ap.add_argument("--n", type=int, default=None, help="sample count (default = full buffer)")
    ap.add_argument("--pre", action="append", default=[], help="serial command to send before capture (repeatable)")
    ap.add_argument("--settle", type=float, default=0.0, help="seconds to wait after --pre before capturing")
    ap.add_argument("--wav", help="save capture to this WAV path")
    ap.add_argument("--plot", help="save waveform+spectrum PNG to this path")
    ap.add_argument("--label", default="", help="label printed with the summary")
    args = ap.parse_args()

    x, rate = grab(args.port, n=args.n, pre=args.pre, settle=args.settle)
    print(f"[capture{' ' + args.label if args.label else ''}]")
    analyze(x, rate)
    if args.wav:
        save_wav(x, rate, args.wav)
    if args.plot:
        plot(x, rate, args.plot)


if __name__ == "__main__":
    main()
