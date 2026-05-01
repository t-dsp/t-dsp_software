"""
test_stereo_link.py -- /config/chlink/N-M behavior matches X32.

X32 channel-link contract (from the X32 OSC PDF):

  * `/config/chlink/N-M ,i 1` enables linking of channel pair (N, M).
    Linked pairs MUST be (1,2), (3,4), (5,6), ... -- always odd-even.
  * When linked, fader / mute / EQ / dyn / sends mirror across both
    channels. Writing to either side updates both.
  * Pan mirrors with sign-flip: writing pan = +0.3 to ch 01 sets
    pan = -0.3 on ch 02 (so the stereo image stays sane).
  * Channel name does NOT mirror -- "Vox L" and "Vox R" deliberately
    stay distinct.

Tests in this file should fail loudly if the firmware ever decides to
mirror everything (including names) or drops the pan sign-flip.

If the firmware doesn't implement /pan yet, `test_chlink_pan_sign_flip`
is marked `xfail(strict=False)` so the rest of the suite still runs.
"""
from __future__ import annotations

import time

import pytest

from osc_client import fader_quantize


@pytest.fixture
def chlink_1_2(osc):
    """Enable the 1-2 channel link for the duration of the test;
    disable it again on teardown so subsequent tests start clean.

    Yields the OscClient with the link active."""
    osc.send("/config/chlink/1-2", 1)
    time.sleep(0.05)
    yield osc
    osc.send("/config/chlink/1-2", 0)
    time.sleep(0.05)


@pytest.mark.hardware
def test_chlink_mirror_fader(chlink_1_2):
    """With 1-2 linked, writing ch/01 fader updates ch/02 fader.

    Failure means the link table either doesn't exist or doesn't fan
    writes through to the partner channel."""
    osc = chlink_1_2
    osc.send("/ch/01/mix/fader", 0.5)
    time.sleep(0.05)
    reply = osc.query("/ch/02/mix/fader", timeout=1.0)
    assert reply is not None
    assert abs(reply.args[0] - fader_quantize(0.5)) < 1.5 / 1023.0


@pytest.mark.hardware
def test_chlink_mirror_mute(chlink_1_2):
    """Mute mirrors across linked pairs. /ch/N/mix/on is the X32
    convention -- 1 = unmuted, 0 = muted."""
    osc = chlink_1_2
    osc.send("/ch/01/mix/on", 0)
    time.sleep(0.05)
    reply = osc.query("/ch/02/mix/on", timeout=1.0)
    assert reply is not None
    assert reply.args[0] == 0, f"expected 0 (muted), got {reply.args[0]}"
    # Restore so we don't leave ch 02 muted for the next test.
    osc.send("/ch/01/mix/on", 1)


@pytest.mark.hardware
def test_chlink_unset_no_mirror(osc):
    """With link explicitly OFF, writing L should NOT update R.

    First seed both channels to known-different values, disable the
    link, write L only, verify R unchanged."""
    osc.send("/config/chlink/1-2", 0)
    time.sleep(0.05)
    osc.send("/ch/01/mix/fader", 0.25)
    osc.send("/ch/02/mix/fader", 0.75)
    time.sleep(0.05)
    # Now write only L, expecting R to stay at 0.75.
    osc.send("/ch/01/mix/fader", 0.10)
    time.sleep(0.05)
    reply_r = osc.query("/ch/02/mix/fader", timeout=1.0)
    assert reply_r is not None
    expected_r = fader_quantize(0.75)
    assert abs(reply_r.args[0] - expected_r) < 1.5 / 1023.0, \
        f"R mirrored when it shouldn't have: {reply_r.args[0]} vs {expected_r}"


@pytest.mark.hardware
@pytest.mark.xfail(
    strict=False,
    reason="Firmware may not implement /ch/NN/mix/pan yet -- see project plan",
)
def test_chlink_pan_sign_flip(chlink_1_2):
    """Pan mirrors with sign flip across linked pair (X32 stereo
    image contract): writing pan = 0.3 to L should set pan = -0.3 on R.

    Marked xfail until the firmware grows /pan. Drop the marker once
    the redesign lands."""
    osc = chlink_1_2
    # X32 pan: -1.0 = hard L, 0.0 = center, +1.0 = hard R.
    # When linked, writing +0.3 to L should mirror as -0.3 on R.
    osc.send("/ch/01/mix/pan", 0.3)
    time.sleep(0.05)
    reply = osc.query("/ch/02/mix/pan", timeout=1.0)
    assert reply is not None
    assert abs(reply.args[0] - (-0.3)) < 0.01, \
        f"expected -0.3 on R, got {reply.args[0]}"


@pytest.mark.hardware
def test_chlink_name_does_not_mirror(chlink_1_2):
    """Channel names stay distinct across linked pairs.

    X32 convention: links share level/dynamics/EQ but each side keeps
    its own label so the user can write 'Vox L' / 'Vox R'."""
    osc = chlink_1_2
    osc.send("/ch/01/config/name", "Test_L")
    osc.send("/ch/02/config/name", "Test_R")
    time.sleep(0.1)
    # Now write a new name to L only and verify R retains its label.
    osc.send("/ch/01/config/name", "Vox_L")
    time.sleep(0.05)
    reply = osc.query("/ch/02/config/name", timeout=1.0)
    assert reply is not None
    assert reply.args[0] == "Test_R", \
        f"name mirrored when it shouldn't have: ch02 = {reply.args[0]!r}"
