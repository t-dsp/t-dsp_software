"""
test_volume_responsiveness.py -- end-to-end fader latency + correctness.

These are the killer regression tests for the user-reported bug:
"volume bars in the UI lag noticeably behind the fader". If the OSC
write -> audio level path is slow or queue-saturated, this catches it.

Three scenarios:

  * test_fader_write_to_echo_latency: pure protocol latency. How long
    from "OSC write hits the wire" to "OSC echo arrives back". <10 ms
    is the goal: ~2.7 ms audio block + ~3 ms USB OUT/IN poll + ~3 ms
    SLIP/queue overhead. >50 ms -> something's broken in the
    firmware's OSC dispatcher or the bridge throttle leaked here.

  * test_continuous_drag_no_queue_buildup: simulates a user dragging
    a fader knob (60 Hz, 100 ticks). All 100 echoes must arrive within
    the test window AND the last one must reflect the last write -- if
    a queue tail builds up, the last echo is from N writes ago.

  * test_loopback_level_follows_fader: drives a 1 kHz tone through
    USB Audio while sweeping /ch/01/mix/fader 0 -> 1; captures the
    loopback and verifies measured RMS tracks the X32 dB law within a
    few dB. This is the integration test that catches "fader writes
    but audio doesn't follow" entirely.

The third test requires a loopback cable from OUT1/2 to the codec's
ADC inputs (same wiring as the existing loopback_test.py).
"""
from __future__ import annotations

import statistics
import time
from typing import List

import pytest

try:
    import numpy as np
    import sounddevice as sd
    HAVE_AUDIO = True
except ImportError:
    HAVE_AUDIO = False

from osc_client import db_to_fader, fader_quantize, fader_to_db


@pytest.mark.hardware
def test_fader_write_to_echo_latency(osc):
    """Latency from OSC write to OSC echo, with /xremote enabled.

    Method: enable /xremote so writes echo back, drain the queue,
    timestamp before the write, wait for the matching echo, timestamp
    again. Repeat 20 times to get a stable median (Windows USB Audio
    ISR jitter can spike a single sample).

    Expected: median < 10 ms, p95 < 20 ms. If median > 20 ms the
    firmware's OSC dispatcher is running in a low-priority loop()
    instead of the audio update tick."""
    osc.send("/xremote")
    time.sleep(0.1)
    osc.drain()

    samples: List[float] = []
    for i in range(20):
        target = 0.3 + 0.001 * i  # slight variation so each echo is unique
        t0 = time.monotonic()
        osc.send("/ch/01/mix/fader", target)
        echo = osc.recv_match(
            lambda m: m.address == "/ch/01/mix/fader",
            timeout=0.5,
        )
        elapsed_ms = (time.monotonic() - t0) * 1000.0
        assert echo is not None, f"sample {i}: no echo within 500 ms"
        samples.append(elapsed_ms)

    median = statistics.median(samples)
    p95 = sorted(samples)[int(0.95 * len(samples))]
    print(f"\nlatency median={median:.1f} ms, p95={p95:.1f} ms, "
          f"min={min(samples):.1f}, max={max(samples):.1f}")
    assert median < 10.0, f"median latency {median:.1f} ms exceeds 10 ms"
    assert p95 < 20.0, f"p95 latency {p95:.1f} ms exceeds 20 ms"


@pytest.mark.hardware
def test_continuous_drag_no_queue_buildup(osc):
    """100 fader writes at 60 Hz -- all echoes arrive in real time.

    A queue buildup would manifest as: the test takes longer than
    100/60 = 1.67 s, the last echo arrives well after the last write,
    and/or echoes arrive in a burst at the end (because the firmware
    can't drain the queue fast enough during the writes)."""
    osc.send("/xremote")
    time.sleep(0.1)
    osc.drain()

    n_writes = 100
    interval = 1.0 / 60.0  # 60 Hz
    writes_t: List[float] = []
    targets: List[float] = []

    t_start = time.monotonic()
    for i in range(n_writes):
        target = 0.5 + 0.4 * (i / (n_writes - 1))  # 0.5 .. 0.9 ramp
        targets.append(target)
        deadline = t_start + i * interval
        sleep_for = deadline - time.monotonic()
        if sleep_for > 0:
            time.sleep(sleep_for)
        writes_t.append(time.monotonic())
        osc.send("/ch/01/mix/fader", target)

    # Now collect echoes within a generous window.
    echoes: List[tuple] = []
    deadline = time.monotonic() + 1.0
    while time.monotonic() < deadline and len(echoes) < n_writes:
        msg = osc.recv_match(
            lambda m: m.address == "/ch/01/mix/fader",
            timeout=max(0.0, deadline - time.monotonic()),
        )
        if msg is None:
            break
        echoes.append((time.monotonic(), msg.args[0]))

    assert len(echoes) >= int(0.95 * n_writes), \
        f"only {len(echoes)} / {n_writes} echoes received"

    # The last echo should match the last write (modulo quantization).
    last_value = echoes[-1][1]
    expected_last = fader_quantize(targets[-1])
    assert abs(last_value - expected_last) <= 2.0 / 1023.0, \
        f"last echo {last_value} doesn't match last write {expected_last}"

    # The last echo should have arrived within ~50 ms of the last write
    # -- not buffered up at the end.
    lag_last_ms = (echoes[-1][0] - writes_t[-1]) * 1000.0
    assert lag_last_ms < 50.0, \
        f"last echo arrived {lag_last_ms:.1f} ms after last write -- queue tail"


@pytest.mark.hardware
@pytest.mark.audio
@pytest.mark.slow
@pytest.mark.skipif(not HAVE_AUDIO, reason="numpy/sounddevice not installed")
def test_loopback_level_follows_fader(osc, teensy_audio):
    """Drive a 1 kHz tone, sweep the fader, captured RMS should follow
    the X32 dB law.

    Setup:
      * Loopback cable from the codec OUT1/OUT2 to IN1+/IN2+.
      * USB Audio binding the firmware's input stream to the codec ADC
        (same as the standalone loopback_test.py).
      * /ch/01 must route to OUT1 with the fader as its level control.

    Method:
      1. Pre-set the fader to 0.0 (full attenuation, audio silent).
      2. Start a continuous duplex stream playing a 1 kHz sine on L.
      3. Step the fader through {0.25, 0.5, 0.75, 1.0} = {-30, -10,
         0, +10} dB; capture ~250 ms of audio at each step.
      4. Compute RMS in dBFS for each capture.
      5. Verify successive captures track the expected dB step within
         3 dB tolerance (ADC noise floor + USB jitter).

    Failure means the OSC write isn't reaching the audio engine, or
    the engine's gain stage doesn't match the X32 law. Either is the
    "volume bars moving slowly" bug, just at the audio layer."""
    fs = teensy_audio["fs"]
    in_idx = teensy_audio["in"]
    out_idx = teensy_audio["out"]

    # Make sure ch1 is unmuted, sends to main, etc. The exact wiring
    # depends on the firmware's routing defaults; if the redesign lands
    # with different routing, update these setup messages.
    osc.send("/ch/01/mix/on", 1)
    osc.send("/ch/01/mix/fader", 0.0)
    time.sleep(0.1)

    # 1 kHz tone, -6 dBFS, mono on left
    duration = 6.0  # seconds total stream
    n_samples = int(fs * duration)
    t = np.arange(n_samples) / fs
    sig = np.zeros((n_samples, 2), dtype="float32")
    sig[:, 0] = (10 ** (-6.0 / 20)) * np.sin(2 * np.pi * 1000 * t).astype("float32")

    # Sweep schedule: (start_time_s, fader_value)
    schedule = [
        (0.5, 0.25),  # -30 dB
        (1.5, 0.5),   # -10 dB
        (2.5, 0.75),  #   0 dB
        (3.5, 1.0),   # +10 dB
    ]

    # Use playrec for a synchronous duplex capture; schedule fader
    # writes from a worker thread.
    import threading
    captured = {"rec": None}

    def writer():
        for delay, fval in schedule:
            time.sleep(max(0.0, delay - (time.monotonic() - t_stream_start)))
            osc.send("/ch/01/mix/fader", fval)

    sd.default.samplerate = fs
    t_stream_start = time.monotonic()
    th = threading.Thread(target=writer, daemon=True)
    th.start()
    rec = sd.playrec(sig, channels=2, device=(in_idx, out_idx),
                     dtype="float32")
    sd.wait()
    th.join(timeout=1.0)

    # Restore unity for cleanup
    osc.send("/ch/01/mix/fader", fader_quantize(db_to_fader(0.0)))

    # For each scheduled step, take a 250 ms window starting 100 ms
    # after the write (USB out + audio block latency) and compute RMS.
    rms_db: List[float] = []
    for delay, fval in schedule:
        start = int((delay + 0.1) * fs)
        end = start + int(0.25 * fs)
        window = rec[start:end, 0]
        rms = float(np.sqrt(np.mean(window ** 2)) + 1e-30)
        rms_db.append(20 * np.log10(rms))

    # Expected dB at each step = source -6 dBFS + fader dB
    expected = [-6.0 + fader_to_db(fval) for _, fval in schedule]

    print()
    for (delay, fval), got, exp in zip(schedule, rms_db, expected):
        print(f"  fader={fval:.2f}  expected={exp:+6.1f} dBFS  got={got:+6.1f} dBFS")

    # Each measured RMS should be within 3 dB of the expected level.
    # Larger tolerance at -30 dB where ambient noise floor matters.
    for (delay, fval), got, exp in zip(schedule, rms_db, expected):
        tol = 3.0 if exp > -25 else 5.0
        assert abs(got - exp) < tol, \
            f"fader={fval}: got {got:.1f} dBFS, expected {exp:.1f} dBFS (tol {tol})"

    # Also verify monotonicity: each step is louder than the last.
    for i in range(1, len(rms_db)):
        assert rms_db[i] > rms_db[i - 1], \
            f"step {i} ({rms_db[i]:.1f}) not louder than step {i-1} ({rms_db[i-1]:.1f})"
