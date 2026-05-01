"""
test_osc_basics.py -- baseline OSC protocol sanity over the SLIP/CDC link.

These tests verify the firmware can encode/decode OSC frames at all.
If any of these fail, every other test in the suite is meaningless:

  * test_info: the firmware identifies itself (round-trip ASCII).
  * test_read_default_fader: the firmware accepts an args-less read and
    returns the current value.
  * test_write_then_read: the firmware accepts a write, persists it,
    and the readback matches (modulo 1024-step quantization).
  * test_echo_on_xremote: with /xremote enabled, the firmware echoes
    every change back to subscribed clients within one audio block.
  * test_unknown_address: the firmware tolerates unknown addresses
    without crashing -- /info still works after we send garbage.

Failure modes:
  * SLIP framing wrong -> all tests time out (no frame ever decodes).
  * Endianness wrong -> floats look like garbage (e.g. 1.5e-41).
  * Quantization off -> write_then_read drifts more than 1 step.
  * /xremote not implemented -> echo test times out.
"""
from __future__ import annotations

import time

import pytest

from osc_client import fader_quantize


# Tolerance for fader round-trip: within one 1024-step quantization
# bucket. (Slightly larger than 1/1023 to absorb float comparison fuzz.)
QUANT_STEP = 1.0 / 1023.0
QUANT_TOL = 1.5 * QUANT_STEP


@pytest.mark.hardware
def test_info(osc):
    """/info should return ',ssss' = name, version, mac, firmware-tag.

    This is the cheapest "is the firmware alive on OSC" probe. If this
    fails, nothing else in this file will pass."""
    reply = osc.query("/info", timeout=1.0)
    assert reply is not None, "no reply to /info within 1 s"
    assert reply.address == "/info"
    assert reply.typetag.startswith(",s"), \
        f"expected ,s* typetag from /info, got {reply.typetag!r}"
    assert len(reply.args) >= 1, "expected at least 1 string in /info reply"
    # Reasonable identity check: the mixer name should be non-empty
    # ASCII. Don't pin to "T-DSP" exactly — leave room for renames.
    name = reply.args[0]
    assert isinstance(name, str) and len(name) > 0


@pytest.mark.hardware
def test_read_default_fader(osc):
    """Args-less read of /main/st/mix/fader returns current value as ',f'.

    X32-style: sending an address with no args is a "GET" request; the
    firmware replies with the same address + current value. Default
    after boot is unspecified -- could be 0.0, could be 0.75 -- so we
    only check the type and range, not the value."""
    reply = osc.query("/main/st/mix/fader", timeout=1.0)
    assert reply is not None, "no reply to /main/st/mix/fader read"
    assert reply.address == "/main/st/mix/fader"
    assert reply.typetag == ",f"
    f = reply.args[0]
    assert 0.0 <= f <= 1.0, f"fader out of [0,1]: {f}"


@pytest.mark.hardware
def test_write_then_read(osc):
    """Write 0.75 -> read back -> expect quantized 0.75 within 1 step.

    The firmware quantizes incoming floats to its 1024-step internal
    representation before storing. A perfectly-correct firmware will
    return fader_quantize(0.75) = 0.7497..., which is exactly 1 step
    below 0.75 due to int(0.75*1023.5)=767 and 767/1023=0.7497..."""
    target = 0.75
    osc.send("/main/st/mix/fader", target)
    # Settle: one audio block at 48 kHz/128 = 2.7 ms is enough; give
    # 50 ms slack for the OSC reply round-trip.
    time.sleep(0.05)
    reply = osc.query("/main/st/mix/fader", timeout=1.0)
    assert reply is not None
    f = reply.args[0]
    expected = fader_quantize(target)
    assert abs(f - expected) <= QUANT_TOL, \
        f"readback {f} vs expected {expected} (tol {QUANT_TOL})"


@pytest.mark.hardware
def test_echo_on_xremote(osc):
    """With /xremote on, every write should echo back to the client.

    The firmware fans every state change to all subscribed /xremote
    clients. We're the only client, but our write should still echo
    (X32 behavior: echoes go to ALL subscribers including the writer).
    100 ms ceiling: USB CDC round-trip on Windows includes a ~16 ms
    latency-timer chunk delay on top of the main-loop poll + SLIP
    encode. 100 ms is comfortably below human-perceptible UI lag while
    still catching genuine queue-tail buildup."""
    osc.send("/xremote")
    osc.drain()  # discard any state-dump that /xremote triggers
    target = 0.42
    t0 = time.monotonic()
    osc.send("/main/st/mix/fader", target)
    msg = osc.recv_match(
        lambda m: m.address == "/main/st/mix/fader",
        timeout=0.5,
    )
    elapsed = (time.monotonic() - t0) * 1000.0
    assert msg is not None, "no echo of /main/st/mix/fader within 500 ms"
    assert elapsed < 100.0, f"echo took {elapsed:.1f} ms (>100 ms ceiling)"
    assert abs(msg.args[0] - fader_quantize(target)) <= QUANT_TOL


@pytest.mark.hardware
def test_unknown_address(osc):
    """Sending /foo/bar/baz must not crash the firmware.

    Verify by: send garbage, then send /info, expect /info to reply
    normally. If the firmware crashed, the reply will time out (USB
    re-enumerates after a Teensy crashloop, which takes >1 s)."""
    osc.send("/foo/bar/baz", 1.0)
    # No assertion on the response — the firmware can choose to reply
    # with an error, or silently drop. Either is acceptable; what we
    # care about is "did it survive".
    osc.drain()
    # Probe /info to confirm we're still up.
    reply = osc.query("/info", timeout=2.0)
    assert reply is not None, "firmware appears to have crashed after /foo/bar/baz"
    assert reply.address == "/info"
