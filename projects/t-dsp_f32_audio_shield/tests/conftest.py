"""
conftest.py -- pytest fixtures for the T-DSP F32 audio shield OSC harness.

Two layers of fixtures:

1. `teensy_port` (session-scoped) -- finds the Teensy COM port once. If
   nothing is found, all tests marked `@pytest.mark.hardware` are
   skipped via the `_require_hardware` autouse fixture below. Tests
   that don't use this fixture run unconditionally (the fader-law unit
   tests, for example, don't need a device).

2. `osc` (function-scoped) -- opens a fresh OscClient per test. Doing
   this per-function (instead of session-scoped) means an exception in
   one test can't leave the queue stuffed for the next test, and a
   serial-port hang in one test is contained.

Hardware tests must be marked:

    @pytest.mark.hardware
    def test_thing(osc): ...

The `hardware` marker is registered in pytest.ini below; tests without
it run regardless of whether a Teensy is attached.

Override the COM port discovery via env var:

    set TDSP_TEST_PORT=COM9
    pytest

Override the audio device hint similarly:

    set TDSP_TEST_AUDIO_HINT=teensy
"""
from __future__ import annotations

import os
import time
from typing import Optional

import pytest

# osc_client is a sibling module; pytest's rootdir is `tests/` so this
# import works as long as pytest is invoked from `tests/` or via
# `pyproject.toml`/`pytest.ini` rootdir config.
from osc_client import OscClient, find_teensy_port


def pytest_configure(config: pytest.Config) -> None:
    """Register custom markers so `pytest --strict-markers` is happy."""
    config.addinivalue_line(
        "markers",
        "hardware: requires a connected Teensy 4.1 running the f32 firmware "
        "(skipped if no device is enumerated).",
    )
    config.addinivalue_line(
        "markers",
        "audio: requires a USB audio loopback cable from OUT1/2 to IN1+/IN2+ "
        "and the Teensy USB Audio device available via WASAPI.",
    )
    config.addinivalue_line(
        "markers",
        "slow: takes more than 5 seconds (e.g. xremote TTL renew test).",
    )


# ---------------------------------------------------------------------------
# Hardware discovery
# ---------------------------------------------------------------------------

@pytest.fixture(scope="session")
def teensy_port() -> Optional[str]:
    """Resolved Teensy COM port, or None if not connected.

    Environment override: TDSP_TEST_PORT skips auto-detection."""
    override = os.environ.get("TDSP_TEST_PORT")
    if override:
        return override
    return find_teensy_port()


@pytest.fixture(autouse=True)
def _require_hardware(request: pytest.FixtureRequest, teensy_port: Optional[str]) -> None:
    """Auto-applied: any test marked `hardware` skips when no Teensy is
    found. Tests without the marker are unaffected."""
    if "hardware" in request.keywords and teensy_port is None:
        pytest.skip("no Teensy detected (set TDSP_TEST_PORT to override)")


# ---------------------------------------------------------------------------
# OSC client
# ---------------------------------------------------------------------------

@pytest.fixture
def osc(teensy_port: Optional[str]) -> OscClient:
    """Open a fresh OscClient per test. Closed on teardown.

    If your test enables /xremote, /subscribe, etc., the client is
    discarded at end-of-test so the firmware's subscription tables
    aren't polluted across tests. (The firmware's TTL on /xremote is
    10 s; if a test failure leaves a subscription hanging, it'll expire
    on its own before the next test that cares about clean state.)"""
    if teensy_port is None:
        pytest.skip("no Teensy port available")
    cli = OscClient.open(teensy_port)
    # Give the OS a beat to settle the port + drop any boot-banner text
    # the firmware spat out on enumeration.
    time.sleep(0.1)
    cli.drain()
    try:
        yield cli
    finally:
        # Best-effort: tell the firmware to drop any subscriptions this
        # test may have set, so the next test starts clean.
        try:
            cli.send("/unsubscribe")
        except Exception:
            pass
        cli.close()


# ---------------------------------------------------------------------------
# Audio device discovery (used by audio loopback / volume responsiveness)
# ---------------------------------------------------------------------------

@pytest.fixture(scope="session")
def teensy_audio() -> dict:
    """Locate the WASAPI Teensy Audio input + output device indices.
    Returns {'in': idx, 'out': idx, 'fs': 48000}. Skips if not found
    or sounddevice/scipy aren't installed.

    The device is the same one the firmware exposes via USB Audio
    (Teensy alex6679 USB Audio Float32). The audio fixture is needed
    for the THD/SNR loopback test and the volume-tracking test."""
    try:
        import sounddevice as sd  # noqa: F401
    except ImportError:
        pytest.skip("sounddevice not installed (pip install sounddevice scipy numpy)")

    import sounddevice as sd
    hint = os.environ.get("TDSP_TEST_AUDIO_HINT", "teensy").lower()
    api_pref = "Windows WASAPI"
    in_idx = out_idx = None
    for i, d in enumerate(sd.query_devices()):
        api_name = sd.query_hostapis(d["hostapi"])["name"]
        if api_name != api_pref:
            continue
        if hint not in d["name"].lower():
            continue
        if d["max_output_channels"] >= 2 and out_idx is None:
            out_idx = i
        if d["max_input_channels"] >= 2 and in_idx is None:
            in_idx = i
    if in_idx is None or out_idx is None:
        pytest.skip(f"WASAPI Teensy audio device not found (hint={hint!r})")
    return {"in": in_idx, "out": out_idx, "fs": 48000}
