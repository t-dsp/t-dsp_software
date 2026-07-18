#!/usr/bin/env python3
"""drum_jitter_test.py — measure whether the drum groove actually lands on the beat.

Answers the practical question "are the drums on beat, or are they drifting/jittering?"
by reading the firmware's own timing ground truth over USB serial. It does NOT try to
time the audio from the host (USB + OS latency would swamp a few-ms signal); instead it
turns on the firmware jitter probe (@DRUMJIT) and reads back per-hit dispatch lateness
measured with the Teensy's micros().

Two numbers matter:
  * dispatch lateness  — how late each drum note-ON fires vs its scheduled beat. This is
    loop()-polling latency plus any loop() stall, i.e. the exact thing you hear as "off".
  * max loop() gap     — the worst loop()-to-loop() interval in each 1 s window. A drum
    hit can be delayed by at most one loop() period, so this bounds the jitter and pins
    the blame on loop() stalls (the case for moving dispatch into the audio ISR).

As an independent cross-check it also host-timestamps the once-per-beat @BEAT messages
and reports their interval spread. That path includes USB/OS jitter so it is a COARSE
upper bound, not ground truth — it mainly confirms the firmware numbers are sane and
catches gross stalls.

Usage (from jay-mint, or anywhere the Teensy enumerates):
    python3 drum_jitter_test.py --port /dev/ttyACM0 --groove "gmd funk 100bpm.mid" --seconds 30
    python3 drum_jitter_test.py --port COM4 --no-start          # measure whatever is already playing

The firmware side is the @DRUMJIT command + @JIT emit (see firmware/mix-kit/src/main.cpp
and lib/TDspMidiPlayer setJitterProbe). Requires a build that has them (>= the jitter-probe
firmware); older firmware just won't answer @DRUMJIT and the tool will say so.
"""
import argparse
import re
import sys
import time

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial not found — install with: python3 -m pip install pyserial")

# @JIT n=<count> maxLate=<ms> meanLate=<ms> std=<ms> maxLoopGap=<ms> bpm=<bpm>
JIT_RE = re.compile(
    r"@JIT\s+n=(\d+)\s+maxLate=([-\d.]+)\s+meanLate=([-\d.]+)\s+std=([-\d.]+)"
    r"\s+maxLoopGap=([-\d.]+)\s+bpm=([-\d.]+)"
)
BEAT_RE = re.compile(r"@BEAT=(\d+)/(\d+)")


class Serial:
    def __init__(self, port, baud=115200):
        self.s = serial.Serial(port, baud, timeout=0.05)
        try:
            self.s.dtr = True
        except (OSError, ValueError):
            pass
        self._buf = b""

    def send(self, line):
        self.s.write((line + "\n").encode("latin-1"))
        self.s.flush()

    def readline(self):
        """Return (line, host_monotonic_timestamp) or (None, None) if nothing buffered."""
        nl = self._buf.find(b"\n")
        if nl < 0:
            chunk = self.s.read(4096)
            if chunk:
                self._buf += chunk
            nl = self._buf.find(b"\n")
            if nl < 0:
                return None, None
        ts = time.monotonic()  # stamp on arrival (best the host can do)
        line = self._buf[:nl]
        self._buf = self._buf[nl + 1:]
        return line.rstrip(b"\r").decode("latin-1", "replace"), ts

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass


def main():
    ap = argparse.ArgumentParser(description="Measure drum timing jitter from firmware ground truth.")
    ap.add_argument("--port", default="/dev/ttyACM0", help="serial port (default /dev/ttyACM0; e.g. COM4 on Windows)")
    ap.add_argument("--groove", default="gmd funk 100bpm.mid", help="groove filename to @DRUMF (must be on the SD)")
    ap.add_argument("--seconds", type=float, default=30.0, help="measurement window (default 30 s)")
    ap.add_argument("--no-start", action="store_true", help="don't start the clock/groove — measure what's already playing")
    args = ap.parse_args()

    dev = Serial(args.port)
    time.sleep(0.3)
    dev.s.reset_input_buffer()
    dev._buf = b""

    if not args.no_start:
        print(f"[test] starting transport + groove '{args.groove}' ...")
        dev.send("@METRO=1")
        time.sleep(0.3)
        dev.send(f"@DRUMF={args.groove}")
        time.sleep(1.0)  # let the launch quantize land and the loop get going

    # Enable the firmware probe (resets its accumulators).
    dev.send("@DRUMJIT=1")
    got_ack = False

    jit_last = None            # latest cumulative @JIT tuple
    worst_loop_gap = 0.0       # max of the per-second loop-gap windows
    jit_lines = 0
    beat_ts = []               # host arrival timestamps of @BEAT
    load_failed = False

    t_end = time.monotonic() + args.seconds
    print(f"[test] measuring for {args.seconds:.0f} s (firmware micros() ground truth) ...")
    while time.monotonic() < t_end:
        line, ts = dev.readline()
        if line is None:
            time.sleep(0.005)
            continue
        m = JIT_RE.search(line)
        if m:
            got_ack = True
            jit_lines += 1
            n, maxl, meanl, std, gap, bpm = m.groups()
            jit_last = (int(n), float(maxl), float(meanl), float(std), float(bpm))
            worst_loop_gap = max(worst_loop_gap, float(gap))
            # live one-line progress
            print(f"  t+{args.seconds - (t_end - time.monotonic()):5.1f}s  "
                  f"hits={int(n):4d}  maxLate={float(maxl):5.2f}ms  mean={float(meanl):4.2f}ms  "
                  f"loopGap(1s)={float(gap):5.2f}ms")
            continue
        b = BEAT_RE.search(line)
        if b and ts is not None:
            beat_ts.append(ts)
            continue
        if "SD load FAILED" in line:
            load_failed = True
            print(f"  ! {line}")

    dev.send("@DRUMJIT=0")
    time.sleep(0.2)
    dev.close()

    print("\n===== RESULT =====")
    if load_failed:
        print("Groove failed to load from the SD — is the name right and on the card? Aborting.")
        return 2
    if not got_ack or jit_last is None:
        print("No @JIT data received. Either the firmware predates the jitter probe (@DRUMJIT),")
        print("or no drums were playing. Start a groove (drop --no-start) and retry.")
        return 2

    n, maxl, meanl, std, bpm = jit_last
    beat_ms = 60000.0 / bpm if bpm > 1 else 0.0
    sixteenth = beat_ms / 4.0

    print("Firmware ground truth (micros()-measured dispatch lateness):")
    print(f"  drum note-ons measured : {n}")
    print(f"  mean lateness          : {meanl:.2f} ms")
    print(f"  std  lateness          : {std:.2f} ms")
    print(f"  MAX  lateness          : {maxl:.2f} ms   (worst single hit)")
    print(f"  worst loop() gap       : {worst_loop_gap:.2f} ms   (bounds the jitter)")
    if beat_ms:
        print(f"  tempo                  : {bpm:.1f} BPM  (1 beat={beat_ms:.0f} ms, 1/16={sixteenth:.0f} ms)")
        print(f"  worst hit as % of 1/16 : {100.0 * maxl / sixteenth:.1f}%")

    # Host cross-check (coarse: includes USB/OS latency).
    if len(beat_ts) >= 4 and beat_ms:
        ivals = [(beat_ts[i] - beat_ts[i - 1]) * 1000.0 for i in range(1, len(beat_ts))]
        mean_iv = sum(ivals) / len(ivals)
        dev_iv = [abs(x - beat_ms) for x in ivals]
        print("\nHost @BEAT cross-check (coarse — includes USB/OS jitter, upper bound only):")
        print(f"  beats seen             : {len(beat_ts)}")
        print(f"  mean beat interval     : {mean_iv:.1f} ms  (expected {beat_ms:.1f} ms)")
        print(f"  max interval deviation : {max(dev_iv):.1f} ms")

    # Verdict on the ground-truth number.
    print("\nVerdict:")
    if maxl < 3.0:
        print(f"  TIGHT — worst hit {maxl:.2f} ms late. Drums are landing on the grid; loop() jitter is")
        print("  below what's audible. Any perceived unevenness is groove feel, not the engine.")
    elif maxl < 8.0:
        print(f"  GOOD, minor jitter — worst hit {maxl:.2f} ms late. Occasionally perceptible on sparse")
        print("  grooves. Moving dispatch into the audio ISR would tighten it; likely not urgent.")
    else:
        print(f"  AUDIBLE — worst hit {maxl:.2f} ms late, worst loop() gap {worst_loop_gap:.2f} ms. loop() is")
        print("  stalling enough to push hits off the grid. This is the case for an audio-locked")
        print("  sequencer (dispatch from the audio update so loop() stalls can't delay a hit).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
