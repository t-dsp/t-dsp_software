"""
test_subscribe.py -- /xremote and /subscribe push semantics.

Two distinct subscription mechanisms in X32 (and in this firmware):

  * `/xremote` -- "subscribe to ALL state changes". The firmware fans
    every /ch/N/* and /main/* and /-stat/* update to /xremote
    subscribers in real time. Used by mixer surfaces (X32-Edit,
    X-Touch). 10-second TTL: if you don't re-send /xremote, the
    firmware drops your subscription.

  * `/subscribe ,sii "<path>" <fmt> <tf>` -- "subscribe to ONE path
    at <tf> rate". Used for meters and CPU stats where a fixed update
    rate is wanted regardless of state-change traffic. tf is a rate
    code: 0=1Hz, 1=10Hz, 2=20Hz... (X32 PDF: actual rates documented
    per address).

Tests in this file:

  * test_xremote_push -- after /xremote, an external state change
    arrives unsolicited.
  * test_xremote_ttl_renew -- the 10 s TTL: stops without renew,
    resumes with renew. Marked `slow` (>10 s).
  * test_subscribe_rate_codes -- subscribe to /-stat/cpu at tf=2,
    count messages over 10 s, expect within +/-20% of nominal.

If the firmware uses different rate codes or TTL values, update the
constants at the top.
"""
from __future__ import annotations

import time
from collections import Counter

import pytest


XREMOTE_TTL_SEC = 10.0   # firmware drops /xremote subs after 10 s of silence
SUBSCRIBE_TF2_HZ = 10.0  # tf=2 nominal rate (firmware-dependent; adjust if it
                         # turns out to be 20 Hz on this build)


@pytest.mark.hardware
def test_xremote_push(osc):
    """After /xremote, a fader write produces an unsolicited push.

    We're the only client, so the push goes to us. Different from
    test_echo_on_xremote in test_osc_basics: that test verifies the
    write -> echo loop; this one verifies that pushes happen for
    state changes the test client *didn't* originate. We fake an
    "external" change by sending a CLI text command that doesn't go
    through OSC, so any OSC reply is purely a state-change push, not
    an OSC echo.

    NOTE: if the firmware's CLI doesn't expose a "set fader" command,
    fall back to sending an OSC write -- a writer-echoes-self
    architecture is also acceptable here, just less rigorous."""
    osc.send("/xremote")
    time.sleep(0.1)
    osc.drain()

    # First try a CLI text command if available. If the firmware
    # doesn't have one, this is a no-op (the firmware ignores the
    # garbage line) and we fall back to an OSC write below.
    osc.send_text("set ch01_fader 0.42")
    time.sleep(0.1)
    msg = osc.recv_match(
        lambda m: m.address == "/ch/01/mix/fader",
        timeout=0.5,
    )
    if msg is None:
        # Fallback: write via OSC (writer-echoes-self path).
        osc.send("/ch/01/mix/fader", 0.42)
        msg = osc.recv_match(
            lambda m: m.address == "/ch/01/mix/fader",
            timeout=0.5,
        )

    assert msg is not None, "no push received after /xremote subscribe"
    assert msg.typetag == ",f"


@pytest.mark.hardware
@pytest.mark.slow
def test_xremote_ttl_renew(osc):
    """TTL: pushes stop after 10 s, resume after re-sending /xremote.

    Method:
      1. /xremote, write, see push.
      2. Wait 11 s (> TTL) without sending anything.
      3. Write again from outside, expect NO push (TTL expired).
      4. /xremote again, write, expect pushes resume.

    Failure: pushes never stop (TTL not implemented), or never
    resume after renew (subscription table corrupted)."""
    osc.send("/xremote")
    time.sleep(0.2)
    osc.drain()

    # Phase 1: confirm pushes work right now.
    osc.send("/ch/01/mix/fader", 0.10)
    msg = osc.recv_match(
        lambda m: m.address == "/ch/01/mix/fader",
        timeout=0.5,
    )
    assert msg is not None, "no initial push after /xremote"

    # Phase 2: wait past TTL.
    time.sleep(XREMOTE_TTL_SEC + 1.0)
    osc.drain()

    # Phase 3: write -- expect NO push (subscription expired). The
    # firmware will still ACK the write itself (writer-echoes-self
    # is a separate path from /xremote), so we look for the second
    # message of two: the first is the writer's own echo, the second
    # would be the /xremote push. With TTL expired, only the first
    # arrives. To distinguish, we use the bare write semantics: a
    # non-/xremote firmware should not produce ANY echo to a write.
    # If your firmware has writer-echoes-self even without /xremote,
    # this assertion needs adjusting.
    osc.send("/ch/01/mix/fader", 0.20)
    expired_msg = osc.recv_match(
        lambda m: m.address == "/ch/01/mix/fader",
        timeout=0.5,
    )
    # Accept either: no push at all (strict), OR exactly one (the
    # writer-echo) but no further pushes within the next 200 ms.
    if expired_msg is not None:
        # Drain extra pushes and confirm there are none.
        time.sleep(0.2)
        extra = []
        while True:
            m = osc.recv_match(
                lambda m: m.address == "/ch/01/mix/fader",
                timeout=0.05,
            )
            if m is None:
                break
            extra.append(m)
        assert len(extra) == 0, \
            f"got {len(extra)} extra pushes after TTL expired -- TTL not enforced"

    # Phase 4: renew + verify pushes resume.
    osc.send("/xremote")
    time.sleep(0.2)
    osc.drain()
    osc.send("/ch/01/mix/fader", 0.30)
    renewed = osc.recv_match(
        lambda m: m.address == "/ch/01/mix/fader",
        timeout=0.5,
    )
    assert renewed is not None, "pushes did not resume after /xremote renew"


@pytest.mark.hardware
@pytest.mark.slow
def test_subscribe_rate_codes(osc):
    """Subscribe to /-stat/cpu at tf=2 (10 Hz), count for 10 s.

    Expected: 80-120 messages (nominal 100, 20% tolerance for USB
    jitter and the firmware's update tick alignment). Way fewer ->
    the rate code is being interpreted as a slower bin (1 Hz?) or
    the path doesn't update at all. Way more -> the rate code is
    being ignored and the firmware fires every audio block."""
    osc.drain()
    # /subscribe ,sii path "fmt" tf
    # The "fmt" string for a single-float metric is typically "f"; if
    # the firmware uses a different convention (e.g. "F" or the empty
    # string) update here.
    osc.send("/subscribe", "/-stat/cpu", "f", 2)
    time.sleep(0.2)
    osc.drain()

    duration_s = 10.0
    deadline = time.monotonic() + duration_s
    count = 0
    while time.monotonic() < deadline:
        msg = osc.recv_match(
            lambda m: m.address == "/-stat/cpu",
            timeout=max(0.0, deadline - time.monotonic()),
        )
        if msg is None:
            break
        count += 1

    # Cleanup.
    osc.send("/unsubscribe", "/-stat/cpu")

    nominal = SUBSCRIBE_TF2_HZ * duration_s  # 100 if tf=2 means 10 Hz
    lo, hi = 0.8 * nominal, 1.2 * nominal
    print(f"\n/-stat/cpu @ tf=2: got {count} msgs in {duration_s} s "
          f"(expected {nominal} +/- 20%)")
    assert lo <= count <= hi, \
        f"got {count} messages in {duration_s} s; expected {lo:.0f}..{hi:.0f}"


@pytest.mark.hardware
def test_subscribe_unknown_path_no_crash(osc):
    """Subscribing to a non-existent path must not crash the firmware.

    /info still works after we send the bad subscribe."""
    osc.send("/subscribe", "/no/such/path", "f", 2)
    time.sleep(0.2)
    info = osc.query("/info", timeout=2.0)
    assert info is not None, "firmware crashed after bad /subscribe"


@pytest.mark.hardware
def test_xremote_distinct_paths(osc):
    """Multiple writes to different paths each produce their own push.

    Paranoia: confirm /xremote isn't deduping by some shared key."""
    osc.send("/xremote")
    time.sleep(0.1)
    osc.drain()

    osc.send("/ch/01/mix/fader", 0.11)
    osc.send("/ch/02/mix/fader", 0.22)
    osc.send("/main/st/mix/fader", 0.33)

    seen = Counter()
    deadline = time.monotonic() + 1.0
    while time.monotonic() < deadline:
        m = osc.recv(timeout=max(0.0, deadline - time.monotonic()))
        if m is None:
            break
        if m.address.endswith("/mix/fader"):
            seen[m.address] += 1

    expected = {"/ch/01/mix/fader", "/ch/02/mix/fader", "/main/st/mix/fader"}
    missing = expected - set(seen.keys())
    assert not missing, f"missing pushes for {missing}; saw {dict(seen)}"
