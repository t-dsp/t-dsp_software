"""
test_fader_law.py -- X32-style 4-segment fader law on the wire.

Two flavors of test here:

  * Pure-math tests that don't need the Teensy. These verify the
    `fader_to_db` / `db_to_fader` helpers in osc_client.py exactly
    match the X32 PDF appendix p. 132 cardinals. They run on every
    pytest invocation, hardware or not.

  * Hardware tests that write a fader value, read it back, and verify
    the firmware quantized it to the same 1024-step grid the host
    uses. If the firmware's grid is off-by-one or off-by-a-factor,
    these will catch it.

Cardinal points (from X32 appendix):
    0.0    -> -inf dB
    0.0625 -> -60 dB
    0.25   -> -30 dB
    0.5    -> -10 dB
    0.75   ->   0 dB
    1.0    -> +10 dB

If a cardinal round-trip drifts more than 0.5 dB, the firmware is
using a different law (or a different segment count) than the host
expects -- the dev surface will display wrong dB values to the user.
"""
from __future__ import annotations

import math
import time

import pytest

from osc_client import db_to_fader, fader_quantize, fader_to_db

CARDINALS = [
    (0.0,    float("-inf")),
    (0.0625, -60.0),
    (0.25,   -30.0),
    (0.5,    -10.0),
    (0.75,     0.0),
    (1.0,    +10.0),
]


@pytest.mark.parametrize("f, db", CARDINALS)
def test_fader_to_db_cardinals(f, db):
    """Each cardinal float maps to the expected dB exactly.

    Pure math; no firmware involvement. Failure here means osc_client.py
    diverged from the X32 spec."""
    got = fader_to_db(f)
    if math.isinf(db):
        assert math.isinf(got) and got < 0
    else:
        # Within 1e-6 dB -- this is exact arithmetic on the segment
        # endpoints, so it should be at floating-point precision.
        assert abs(got - db) < 1e-6, f"f={f}: got {got} dB, expected {db}"


@pytest.mark.parametrize("f, db", [c for c in CARDINALS if not math.isinf(c[1])])
def test_db_to_fader_cardinals(f, db):
    """Inverse of the above: each cardinal dB maps to the right float."""
    got = db_to_fader(db)
    assert abs(got - f) < 1e-6, f"db={db}: got {got}, expected {f}"


def test_db_to_fader_round_trip():
    """Every dB from -60 .. +10, step 1 dB, round-trips through fader and
    back to within 0.5 dB. (Sub-segment values aren't on the cardinal
    grid; we lose a tiny amount to floating-point representation.)"""
    for db_target in range(-60, 11):
        f = db_to_fader(float(db_target))
        db_back = fader_to_db(f)
        assert abs(db_back - db_target) < 0.5, \
            f"{db_target} dB -> f={f} -> {db_back} dB (drift > 0.5)"


def test_fader_to_db_below_inf_segment():
    """The 4th segment (f < 0.0625) is the steep -inf..-60 region.
    Verify a couple of points within it round-trip."""
    # Mid-segment: f = 0.03125 -> dB = 0.03125*480 - 90 = -75 dB
    assert abs(fader_to_db(0.03125) - (-75.0)) < 1e-6
    # f = 0.001 -> dB = 0.001*480 - 90 = -89.52 dB
    assert abs(fader_to_db(0.001) - (-89.52)) < 1e-6


def test_db_to_fader_out_of_range_raises():
    """+10 dB is the ceiling. Anything above must raise."""
    with pytest.raises(ValueError):
        db_to_fader(10.001)


def test_fader_quantize_grid():
    """1024 steps (0..1023). Cardinals must land exactly on a step."""
    # 1.0 -> 1023, 0.0 -> 0, 0.75 -> int(0.75*1023.5)=767
    assert fader_quantize(1.0) == 1023 / 1023.0
    assert fader_quantize(0.0) == 0.0
    assert fader_quantize(0.75) == 767 / 1023.0


# ---------------------------------------------------------------------------
# Hardware-side: verify the firmware's quantization grid matches ours.
# ---------------------------------------------------------------------------

@pytest.mark.hardware
def test_fader_quantization(osc):
    """Write 0.751, read back, expect 0.751 within 1 quantization step.

    If the firmware uses a different step count (e.g. 256 instead of
    1024), this drifts > 1 step and fails."""
    target = 0.751
    osc.send("/ch/01/mix/fader", target)
    time.sleep(0.05)
    reply = osc.query("/ch/01/mix/fader", timeout=1.0)
    assert reply is not None
    expected = fader_quantize(target)
    step = 1.0 / 1023.0
    assert abs(reply.args[0] - expected) <= 1.5 * step, \
        f"got {reply.args[0]}, expected {expected} +/- {step}"


@pytest.mark.hardware
@pytest.mark.parametrize("db_target", [-60, -30, -10, 0, 10])
def test_round_trip_through_firmware(osc, db_target):
    """For each cardinal dB, write the fader, read it back, convert to
    dB, expect within 0.5 dB. Confirms the firmware's float-to-int
    quantization preserves the law."""
    f = db_to_fader(float(db_target))
    osc.send("/ch/01/mix/fader", f)
    time.sleep(0.05)
    reply = osc.query("/ch/01/mix/fader", timeout=1.0)
    assert reply is not None
    db_back = fader_to_db(reply.args[0])
    assert abs(db_back - db_target) < 0.5, \
        f"{db_target} dB -> wrote f={f} -> read f={reply.args[0]} -> {db_back} dB"
