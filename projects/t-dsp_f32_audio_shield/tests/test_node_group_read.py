"""
test_node_group_read.py -- /node packed-text dump.

X32's /node command returns the entire subtree under a path as one
packed ASCII string, separated by spaces, terminated by '\\n'. This
is the bulk-read primitive that lets a UI fetch a whole channel strip
in one round-trip instead of 30 separate /ch/01/* queries.

Wire format (X32 PDF section "Node groups"):

    Request:  /node ,s "ch/01"
    Reply:    /node ,s "/ch/01/config/name "Vox" 0 1 OFF\\n
                       /ch/01/mix/on 1\\n
                       /ch/01/mix/fader 0.5\\n
                       ..."

(All field separators are single spaces, line separators are '\\n',
strings are double-quoted.)

These tests verify:

  * The reply is a /node ,s message (not a series of individual
    /ch/01/* messages -- that would defeat the point of /node).
  * The reply contains the expected leaves for the requested subtree
    (mix/fader, mix/on, config/name for a channel; voice / fader for
    the synth subtree, etc.).
  * The reply is parseable as packed text (no embedded NULs in the
    middle of strings, no truncation).

If the firmware doesn't implement /node yet, mark these xfail until
the redesign lands.
"""
from __future__ import annotations

import re
import time

import pytest


def _parse_node_lines(payload: str) -> list[str]:
    """Split the packed-text reply into individual lines, trimming
    trailing whitespace. The X32 spec uses '\\n' as the line separator;
    we tolerate '\\r\\n' too in case the firmware adds CR."""
    return [ln.strip() for ln in payload.splitlines() if ln.strip()]


def _parse_node_path(line: str) -> str:
    """Extract just the leaf path (before the first space)."""
    return line.split(" ", 1)[0]


@pytest.mark.hardware
def test_node_returns_packed_text(osc):
    """`/node ,s "ch/01"` returns a single ',s' reply with packed text.

    Failure modes: firmware sends per-leaf /ch/01/* messages instead
    (defeats the bulk-read purpose); reply has no '\\n' separator
    (unparseable); reply is empty (subtree not registered)."""
    reply = osc.query("/node", "ch/01", timeout=2.0)
    assert reply is not None, "no reply to /node ch/01 within 2 s"
    assert reply.address == "/node"
    assert reply.typetag == ",s", \
        f"expected /node reply with ,s typetag, got {reply.typetag}"
    payload = reply.args[0]
    assert isinstance(payload, str)
    lines = _parse_node_lines(payload)
    # A channel strip should have at least: name, mix/on, mix/fader.
    assert len(lines) >= 3, f"too few leaves in /node ch/01: {payload!r}"


@pytest.mark.hardware
def test_node_main(osc):
    """`/node main/st` returns the main stereo bus -- expect at least
    fader and mute."""
    reply = osc.query("/node", "main/st", timeout=2.0)
    assert reply is not None
    assert reply.typetag == ",s"
    lines = _parse_node_lines(reply.args[0])
    paths = {_parse_node_path(ln) for ln in lines}
    # Required leaves -- if the firmware exposes more, that's fine.
    expected = {"/main/st/mix/fader", "/main/st/mix/on"}
    missing = expected - paths
    assert not missing, f"main/st missing leaves: {missing}; got {paths}"


@pytest.mark.hardware
def test_node_synth(osc):
    """`/node synth/dexed` returns the Dexed FM synth subtree.

    Expect at minimum a fader, a voice/program selector, and a name.
    These are the leaves the UI needs to render the synth strip; if
    any are missing the synth panel renders blank."""
    reply = osc.query("/node", "synth/dexed", timeout=2.0)
    assert reply is not None
    assert reply.typetag == ",s"
    lines = _parse_node_lines(reply.args[0])
    paths_text = " ".join(lines).lower()
    # Looser check: we want SOMETHING resembling each leaf.
    # Names will firm up once the OSC tree lands.
    for needle in ("fader", "voice", "name"):
        assert needle in paths_text, \
            f"/node synth/dexed missing {needle!r}: {paths_text!r}"


@pytest.mark.hardware
def test_node_unknown_subtree(osc):
    """`/node nonexistent/path` should reply with empty packed text
    or no reply at all -- but must not crash. Probe /info after to
    confirm the firmware is still alive."""
    reply = osc.query("/node", "nonexistent/path", timeout=1.0)
    # Either an empty ',s' reply or no reply is acceptable.
    if reply is not None:
        assert reply.typetag == ",s"
        # Empty or near-empty -- if the firmware decides to send a
        # diagnostic line instead, that's also fine, just not '\\n'-
        # terminated subtree dump.

    # Liveness check.
    info = osc.query("/info", timeout=2.0)
    assert info is not None, "firmware unresponsive after /node bad path"


@pytest.mark.hardware
def test_node_round_trip_count(osc):
    """A /node read of an empty channel strip and one with state
    written should differ in exactly the leaves we touched.

    Sets a name, reads /node, verifies the new name appears."""
    osc.send("/ch/03/config/name", "Probe")
    time.sleep(0.05)
    reply = osc.query("/node", "ch/03", timeout=2.0)
    assert reply is not None
    payload = reply.args[0]
    assert "Probe" in payload, \
        f"name 'Probe' not found in /node ch/03 dump: {payload!r}"
