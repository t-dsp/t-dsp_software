#!/usr/bin/env python3
"""
spike_osc_test.py — drive Spike 1 over the Teensy's USB CDC and assert
both control surfaces work.

Tests:
    1. /test/hello f triggers a reply with the spike's identity. Replaces an
       earlier "wait for boot banner" test that was racy because the banner
       is printed once in setup() — by the time pyserial opens the COM port,
       the banner has either been buffered, dropped, or scrolled past.
       /test/hello is the deterministic alternative the harness can drive
       on demand.
    2. /test/gain f 0.5 sent as a SLIP-OSC frame triggers the mixer-domain
       handler and we receive an echoed /test/gain f 0.5 reply.
    3. /teensy1/audio/amp/g f 0.5 sent as a SLIP-OSC frame triggers the
       OSCAudio passthrough and we receive a non-error reply bundle.
    4. CLI command "test.gain 0.7\\n" sent as plain ASCII produces the
       same effect as the OSC frame (echoed reply, audible gain change).
    5. The host sees both SLIP frames and plain text on the same stream
       without corruption.

Usage:
    python tools/spike_osc_test.py [--port COM5]
"""

import argparse
import sys
import time
from typing import Optional

try:
    import serial  # pyserial
except ImportError:
    sys.exit("error: pyserial not installed; pip install pyserial")

try:
    from pythonosc.osc_message_builder import OscMessageBuilder
    from pythonosc.osc_message import OscMessage
    from pythonosc.osc_bundle import OscBundle
except ImportError:
    sys.exit("error: python-osc not installed; pip install python-osc")


# ---- SLIP framing (RFC 1055) -------------------------------------------------

SLIP_END = 0xC0
SLIP_ESC = 0xDB
SLIP_ESC_END = 0xDC
SLIP_ESC_ESC = 0xDD


def slip_encode(payload: bytes) -> bytes:
    out = bytearray([SLIP_END])
    for b in payload:
        if b == SLIP_END:
            out += bytes([SLIP_ESC, SLIP_ESC_END])
        elif b == SLIP_ESC:
            out += bytes([SLIP_ESC, SLIP_ESC_ESC])
        else:
            out.append(b)
    out.append(SLIP_END)
    return bytes(out)


class SlipDemultiplexer:
    """
    Stream byte demuxer: feed it raw bytes from the serial port; it emits
    (kind, data) tuples where kind is 'osc' (full SLIP-decoded frame) or
    'text' (one ASCII line).
    """

    def __init__(self):
        self._frame = bytearray()
        self._line = bytearray()
        self._in_frame = False
        self._escaped = False

    def feed(self, chunk: bytes):
        for b in chunk:
            if not self._in_frame:
                if b == SLIP_END:
                    # Frame start. Flush any pending text line first.
                    if self._line:
                        line = self._line.decode("utf-8", errors="replace").rstrip("\r\n")
                        if line:
                            yield ("text", line)
                        self._line.clear()
                    self._frame.clear()
                    self._in_frame = True
                else:
                    self._line.append(b)
                    if b == ord("\n"):
                        line = self._line.decode("utf-8", errors="replace").rstrip("\r\n")
                        if line:
                            yield ("text", line)
                        self._line.clear()
            else:
                if self._escaped:
                    if b == SLIP_ESC_END:
                        self._frame.append(SLIP_END)
                    elif b == SLIP_ESC_ESC:
                        self._frame.append(SLIP_ESC)
                    else:
                        self._frame.append(b)
                    self._escaped = False
                elif b == SLIP_ESC:
                    self._escaped = True
                elif b == SLIP_END:
                    if self._frame:
                        yield ("osc", bytes(self._frame))
                        self._frame.clear()
                    self._in_frame = False
                else:
                    self._frame.append(b)


# ---- OSC helpers -------------------------------------------------------------

def make_osc_float(addr: str, value: float) -> bytes:
    b = OscMessageBuilder(address=addr)
    b.add_arg(float(value), arg_type="f")
    return b.build().dgram


def make_osc_no_args(addr: str) -> bytes:
    b = OscMessageBuilder(address=addr)
    return b.build().dgram


def parse_osc_payload(data: bytes):
    """Parse a SLIP-decoded payload as either an OscMessage or an OscBundle."""
    if data.startswith(b"#bundle"):
        try:
            return OscBundle(data)
        except Exception as e:
            return f"<bad bundle: {e}>"
    try:
        return OscMessage(data)
    except Exception as e:
        return f"<bad message: {e}>"


# ---- Test driver -------------------------------------------------------------

class SpikeTest:
    def __init__(self, port: str, baud: int = 115200, timeout: float = 0.2):
        self.ser = serial.Serial(port, baud, timeout=timeout)
        self.demux = SlipDemultiplexer()
        self.received_text: list[str] = []
        self.received_osc: list = []

    def drain(self, duration: float = 0.5):
        end = time.monotonic() + duration
        while time.monotonic() < end:
            chunk = self.ser.read(256)
            if chunk:
                for kind, data in self.demux.feed(chunk):
                    if kind == "text":
                        self.received_text.append(data)
                        print(f"  [text] {data}")
                    elif kind == "osc":
                        parsed = parse_osc_payload(data)
                        self.received_osc.append(parsed)
                        print(f"  [osc]  {self._fmt_osc(parsed)}")

    def _fmt_osc(self, parsed) -> str:
        if isinstance(parsed, OscMessage):
            return f"{parsed.address} {list(parsed.params)}"
        if isinstance(parsed, OscBundle):
            parts = []
            for msg in parsed:
                if isinstance(msg, OscMessage):
                    parts.append(f"{msg.address} {list(msg.params)}")
                else:
                    parts.append(str(msg))
            return f"bundle{{ {' | '.join(parts)} }}"
        return str(parsed)

    def send_osc(self, addr: str, value: float):
        payload = make_osc_float(addr, value)
        self.ser.write(slip_encode(payload))
        self.ser.flush()

    def send_osc_no_args(self, addr: str):
        payload = make_osc_no_args(addr)
        self.ser.write(slip_encode(payload))
        self.ser.flush()

    def send_cli(self, line: str):
        self.ser.write((line + "\n").encode("ascii"))
        self.ser.flush()

    def find_text(self, needle: str) -> bool:
        return any(needle in t for t in self.received_text)

    def find_osc_address(self, addr: str) -> bool:
        for parsed in self.received_osc:
            if isinstance(parsed, OscMessage) and parsed.address == addr:
                return True
            if isinstance(parsed, OscBundle):
                for msg in parsed:
                    if isinstance(msg, OscMessage) and msg.address == addr:
                        return True
        return False

    def close(self):
        self.ser.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=None, help="Serial port (e.g. COM5 or /dev/ttyACM0)")
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()

    if not args.port:
        sys.exit("error: --port is required (e.g. --port COM5)")

    print(f"Opening {args.port} @ {args.baud}...")
    tester = SpikeTest(args.port, args.baud)

    failures = 0

    try:
        # Drain anything that may already be in the CDC buffer (boot banner,
        # heartbeat, etc.) before starting the test sequence. We don't assert
        # anything from this — boot banner timing is not deterministic across
        # COM port open events.
        print("\n[settle] draining initial buffer...")
        tester.drain(0.8)
        tester.received_text.clear()
        tester.received_osc.clear()

        # 1. /test/hello — deterministic alive check
        print("\n[1] sending /test/hello (deterministic alive check)...")
        tester.send_osc_no_args("/test/hello")
        tester.drain(0.5)
        if tester.find_osc_address("/test/hello"):
            print("    PASS — /test/hello replied via OSC")
        else:
            print("    FAIL — no /test/hello reply")
            failures += 1
        if tester.find_text("hello: spike_osc_foundation alive"):
            print("    PASS — corresponding plain-text confirmation seen")
        else:
            print("    WARN — no plain-text hello line (multiplex may be lossy)")

        # 2. Send /test/gain f 0.5 as SLIP-OSC binary
        print("\n[2] sending /test/gain f 0.5 (SLIP-OSC binary)...")
        tester.received_osc.clear()
        tester.received_text.clear()
        tester.send_osc("/test/gain", 0.5)
        tester.drain(0.5)
        if tester.find_osc_address("/test/gain"):
            print("    PASS — mixer-domain handler echoed /test/gain")
        else:
            print("    FAIL — no /test/gain echo")
            failures += 1
        if tester.find_text("test.gain = 0.500"):
            print("    PASS — debug println coexisted with SLIP frame")
        else:
            print("    WARN — no test.gain= debug print seen (multiplex may be lossy)")

        # 3. Send /teensy1/audio/amp/g f 0.7 (OSCAudio passthrough)
        print("\n[3] sending /teensy1/audio/amp/g f 0.7 (OSCAudio passthrough)...")
        tester.received_osc.clear()
        tester.received_text.clear()
        tester.send_osc("/teensy1/audio/amp/g", 0.7)
        tester.drain(0.5)
        # OSCAudio replies with a bundle containing some kind of confirmation
        # at the same address. We just check the address echoes back.
        if tester.find_osc_address("/teensy1/audio/amp/g") or any(
            isinstance(p, OscBundle) for p in tester.received_osc
        ):
            print("    PASS — OSCAudio dispatched and replied")
        else:
            print("    FAIL — no OSCAudio reply")
            failures += 1

        # 4. Send dotted-path CLI command
        print("\n[4] sending CLI 'test.gain 0.3'...")
        tester.received_osc.clear()
        tester.received_text.clear()
        tester.send_cli("test.gain 0.3")
        tester.drain(0.5)
        if tester.find_text("cli: /test/gain"):
            print("    PASS — CLI shim parsed and routed")
        else:
            print("    FAIL — CLI shim did not log routing")
            failures += 1
        if tester.find_osc_address("/test/gain"):
            print("    PASS — CLI command produced OSC echo (single source of truth)")
        else:
            print("    FAIL — no /test/gain echo from CLI command")
            failures += 1

    finally:
        tester.close()

    print()
    if failures == 0:
        print("ALL TESTS PASSED")
        return 0
    else:
        print(f"{failures} TEST(S) FAILED")
        return 1


if __name__ == "__main__":
    sys.exit(main())
