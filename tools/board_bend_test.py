#!/usr/bin/env python3
"""board_bend_test.py — whole-board health + full pitch-bend check for a T-DSP mix-kit board.

Pairs with the firmware `@BOARDTEST` command (firmware/mix-kit/src/BoardTest.inc.h), which
plays every melodic track (e.g. 2 Dexed + 2 OPLL) and the TSF drum kit, capturing the output
bus at pitch bend 0 / +24 / -24 semitones per melodic track.

The firmware plays MIDI note 72 on each track and captures it at bend 0 / +24 / -24. We get a
robust fundamental for each capture by TIME-DOMAIN autocorrelation (finds the true period even
when an FM patch's loudest partial is a high harmonic — naive FFT-peak/comb-correlation methods
octave-error on rich spectra), then measure the actual bend each way IN SEMITONES relative to
the track's own bend-0 pitch (so a patch that sits in a different octave is handled):

    up = 12*log2(f(+24)/f(0))   and   dn = 12*log2(f(0)/f(-24))   must both be ~= +24

A PASS needs the full +-24 both ways. This catches an engine that clamps: e.g. the synth_dexed
library caps pitch-bend range at 12 semitones (constrain(range,0,12) in dexed.cpp), so the Dexed
pool only bends +-12 (one octave) however wide you set it — the test reports up/dn ~= +12.
Drums are checked for audibility (RMS) only.

Usage
-----
  # Analyze a captured dump (the robust path — capture on the Linux box, analyze anywhere):
  python board_bend_test.py --infile boardtest.txt

  # Drive the board directly over serial (needs pyserial):
  python board_bend_test.py --port /dev/ttyACM0 --save boardtest.txt

To capture over SSH without pyserial (matches tools/linux-flash-host.md):
  stty -F /dev/ttyACM0 clocal 115200 raw -echo
  ( cat /dev/ttyACM0 > boardtest.txt & C=$!; sleep 1; printf '@BOARDTEST\n' > /dev/ttyACM0
    sleep 50; kill $C )

Exit code 0 = all checks passed, 1 = any failure (so it drops into CI/scripts).
"""
import argparse, re, sys
import numpy as np

RMS_AUDIBLE   = 0.003     # below this a capture is "silent"
EXPECTED_SEMI = 24.0      # a full bend each way
SEMI_TOL      = 2.0       # accept +-2 semitones of the ideal +-24


def acf_pitch(samples, rate, fmin=60.0, fmax=3500.0):
    """Fundamental (Hz) via time-domain autocorrelation — robust to which partial is loudest.
    Returns (f0, rms)."""
    x = np.asarray(samples, dtype=np.float64)
    n = len(x)
    x = x - x.mean()
    rms = float(np.sqrt(np.mean(x * x)))
    if n < 128 or rms < 1e-6:
        return 0.0, rms
    c = np.correlate(x * np.hanning(n), x * np.hanning(n), mode='full')[n - 1:]
    lo, hi = int(rate / fmax), min(int(rate / fmin), n - 1)
    if hi <= lo + 1:
        return 0.0, rms
    k = lo + int(np.argmax(c[lo:hi]))
    if 0 < k < len(c) - 1:                       # parabolic interpolation for sub-sample lag
        a, b, cc = c[k - 1], c[k], c[k + 1]
        d = a - 2 * b + cc
        k = k + (0.5 * (a - cc) / d if d else 0.0)
    return (rate / k if k > 0 else 0.0), rms


def dom_freq(samples, rate):
    """Dominant spectral-peak frequency (Hz) and RMS — for the drum audibility line."""
    x = np.asarray(samples, dtype=np.float64) - np.mean(samples)
    n = len(x)
    mag = np.abs(np.fft.rfft(x * np.hanning(n)))
    freqs = np.fft.rfftfreq(n, 1.0 / rate)
    rms = float(np.sqrt(np.mean(x * x)))
    lo = max(1, int(np.searchsorted(freqs, 40.0)))
    if lo >= len(mag):
        return 0.0, rms
    return float(freqs[lo + int(np.argmax(mag[lo:]))]), rms


def semitones(ratio):
    return 12.0 * np.log2(ratio) if ratio > 0 else 0.0


def parse(text):
    """Parse an @BOARDTEST dump into a list of capture dicts (in order)."""
    caps, cur, reading = [], None, False
    for ln in text.splitlines():
        m = re.match(r'\[boardtest\]\s+track=(\d+)\s+eng=(\w+)\s+note=(\d+)\s+bend=(-?\d+)', ln)
        if m:
            cur = dict(kind='track', track=int(m.group(1)), eng=m.group(2),
                       note=int(m.group(3)), bend=int(m.group(4)), samples=[], rate=48000)
            reading = False
            continue
        m = re.match(r'\[boardtest\]\s+drum\s+note=(\d+)', ln)
        if m:
            cur = dict(kind='drum', note=int(m.group(1)), samples=[], rate=48000)
            reading = False
            continue
        m = re.match(r'\[cap\]\s+begin\s+\d+\s+rate\s+(\d+)', ln)
        if m and cur is not None:
            cur['rate'] = int(m.group(1)); reading = True
            continue
        if ln.strip() == '[cap] end':
            if cur is not None and reading:
                caps.append(cur)
            cur, reading = None, False
            continue
        if reading and cur is not None:
            for tok in ln.split():
                try:
                    cur['samples'].append(float(tok))
                except ValueError:
                    pass
    return caps


def analyze(caps):
    """Print a per-track / per-drum report; return True if every check passed."""
    # --- melodic tracks: group the three bend captures per track ---------------
    tracks = {}
    for c in caps:
        if c['kind'] == 'track':
            tracks.setdefault(c['track'], {})[c['bend']] = c

    ok_all = True
    print(f"{'track':<7}{'eng':<7}{'f(-24)':>9}{'f(0)':>9}{'f(+24)':>9}"
          f"{'up semi':>9}{'dn semi':>9}{'rms':>8}  result   (want +24 / +24)")
    print('-' * 82)
    for t in sorted(tracks):
        b = tracks[t]
        if not all(k in b for k in (0, 24, -24)):
            print(f"{t:<7}{'?':<7}{'':>44}  FAIL (missing bend captures)")
            ok_all = False
            continue
        eng = b[0]['eng']
        f0, rms0 = acf_pitch(b[0]['samples'], b[0]['rate'])
        fup, _ = acf_pitch(b[24]['samples'], b[24]['rate'])
        fdn, _ = acf_pitch(b[-24]['samples'], b[-24]['rate'])
        up = semitones(fup / f0) if f0 > 0 else 0.0
        dn = semitones(f0 / fdn) if fdn > 0 else 0.0
        audible = rms0 >= RMS_AUDIBLE
        up_ok = abs(up - EXPECTED_SEMI) <= SEMI_TOL
        dn_ok = abs(dn - EXPECTED_SEMI) <= SEMI_TOL
        passed = audible and up_ok and dn_ok
        ok_all &= passed
        bad = ','.join(w for w, c in (('silent', not audible), ('bend+', not up_ok), ('bend-', not dn_ok)) if c)
        print(f"{t:<7}{eng:<7}{fdn:>9.1f}{f0:>9.1f}{fup:>9.1f}{up:>+9.1f}{dn:>+9.1f}"
              f"{rms0:>8.4f}  {'OK' if passed else 'FAIL(' + bad + ')'}")

    # --- drums: audibility only ------------------------------------------------
    drums = [c for c in caps if c['kind'] == 'drum']
    if drums:
        print()
        print(f"{'drum':<7}{'note':>6}{'domHz':>10}{'rms':>10}  result")
        print('-' * 40)
        for c in drums:
            _, rms = dom_freq(c['samples'], c['rate'])
            f, _ = dom_freq(c['samples'], c['rate'])
            audible = rms >= RMS_AUDIBLE
            ok_all &= audible
            print(f"{'':<7}{c['note']:>6}{f:>10.1f}{rms:>10.4f}  {'OK' if audible else 'FAIL(silent)'}")

    return ok_all


def drive_serial(port, baud, timeout):
    """Send @BOARDTEST and return the captured text (needs pyserial)."""
    import serial
    s = serial.Serial(port, baud, timeout=0.3)
    try:
        s.reset_input_buffer()
        s.write(b'@BOARDTEST\n')
        buf, saw_begin = [], False
        import time
        t0 = time.time()
        while time.time() - t0 < timeout:
            line = s.readline().decode('ascii', 'ignore')
            if not line:
                continue
            buf.append(line)
            if '[boardtest] BEGIN' in line:
                saw_begin = True
            if saw_begin and '[boardtest] END' in line:
                break
        return ''.join(buf)
    finally:
        s.close()


def main():
    ap = argparse.ArgumentParser(description="T-DSP board health + full-bend self-test analyzer.")
    ap.add_argument('--infile', help="analyze a saved @BOARDTEST dump")
    ap.add_argument('--port', help="serial port to drive @BOARDTEST on (needs pyserial)")
    ap.add_argument('--baud', type=int, default=115200)
    ap.add_argument('--timeout', type=float, default=60.0, help="seconds to wait for the dump")
    ap.add_argument('--save', help="when using --port, also write the raw dump here")
    args = ap.parse_args()

    if args.port:
        text = drive_serial(args.port, args.baud, args.timeout)
        if args.save:
            with open(args.save, 'w') as f:
                f.write(text)
    elif args.infile:
        with open(args.infile) as f:
            text = f.read()
    else:
        text = sys.stdin.read()

    caps = parse(text)
    if not caps:
        print("no @BOARDTEST captures found — did the board run @BOARDTEST? "
              "(is this the *_test build with TDSP_BOARDTEST + TDSP_CAP_FULL?)", file=sys.stderr)
        sys.exit(2)

    passed = analyze(caps)
    print()
    print("RESULT:", "PASS — board OK, full +-24 (2-octave) bend on every engine"
          if passed else "FAIL — see rows above")
    sys.exit(0 if passed else 1)


if __name__ == '__main__':
    main()
