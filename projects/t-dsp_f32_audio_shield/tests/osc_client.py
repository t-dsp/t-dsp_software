"""
osc_client.py -- SLIP-framed OSC client over USB CDC serial for the
T-DSP F32 audio shield firmware.

This module is **pure I/O + protocol**. It contains no test logic. Tests
import `OscClient`, `fader_to_db`, `db_to_fader`, `fader_quantize`, and
`find_teensy_port` from here.

Why hand-roll instead of `python-osc`:

  * `python-osc` does not implement SLIP framing, and its UDP-flavored
    bundle/dispatch glue assumes datagrams. The wire format we need is
    raw OSC packets framed by SLIP (RFC 1055), with optional plain-text
    bytes coexisting on the same port (firmware demuxes by frame-start
    byte 0xC0). We need fine control over both the encoder (so we can
    write `,T`/`,F`/`,N`/`,I` if the firmware ever needs them) and the
    decoder (so we can return parsed addresses and typed args without
    going through osc_server's dispatch tree).
  * The OSC encoding here is the spec-minimum: 4-byte aligned strings,
    `,sif` typetags, big-endian floats and ints. Same encoding the
    firmware's BehringerXmit-style sender produces.

Wire layout for one OSC message frame:

    [0xC0]  SLIP END (frame start, optional but firmware tolerates it)
    <address string, NUL-terminated, 4-byte aligned>
    <typetag string starting with ',', NUL-terminated, 4-byte aligned>
    <args, each 4-byte aligned: 'i'/'f' = 4 bytes BE, 's' = NUL-aligned,
                                'b' = u32 BE length + bytes + pad>
    [0xC0]  SLIP END (frame terminator, mandatory)

Plain-text bytes (CLI commands) outside SLIP frames pass through on the
same port unmodified — the firmware decides per-byte whether it's in a
SLIP frame.
"""
from __future__ import annotations

import math
import struct
import threading
import time
from collections import deque
from dataclasses import dataclass
from typing import Iterable, Optional, Sequence, Union

import serial
import serial.tools.list_ports


# ---------------------------------------------------------------------------
# Teensy USB IDs
# ---------------------------------------------------------------------------
# Sourced from the serial-bridge.mjs reference; kept in sync intentionally.
TEENSY_VID = 0x16C0
TEENSY_PIDS = {
    0x0483,  # USB Serial
    0x0489,  # USB MIDI + Audio + Serial (the build we run on the f32 shield)
    0x048A,  # USB Audio + Serial
    0x048B,  # USB MIDI + Serial
}


def find_teensy_port() -> Optional[str]:
    """Auto-detect the Teensy CDC port by VID/PID.

    Returns the device path (e.g. 'COM7' on Windows, '/dev/ttyACM0' on
    Linux) or None if no Teensy is enumerated. Matches by VID first,
    then by description fallback ("Teensy" in the manufacturer / desc
    fields, for unusual driver bindings)."""
    for p in serial.tools.list_ports.comports():
        if p.vid == TEENSY_VID and (p.pid in TEENSY_PIDS or p.pid is None):
            return p.device
    # Fallback: textual match. Some Linux drivers don't always populate
    # vid/pid via list_ports; "Teensy" in the description is reliable.
    for p in serial.tools.list_ports.comports():
        desc = (p.description or "") + " " + (p.manufacturer or "")
        if "teensy" in desc.lower():
            return p.device
    return None


# ---------------------------------------------------------------------------
# X32 fader law (exactly per the X32 PDF appendix p. 132)
# ---------------------------------------------------------------------------
# 4-segment piecewise linear: float 0..1 maps to dB on -inf..+10 with finer
# resolution near 0 dB and coarser resolution at the very bottom.
#
# Cardinal points the firmware MUST agree on:
#   0.0    -> -inf dB
#   0.0625 -> -60 dB
#   0.25   -> -30 dB
#   0.5    -> -10 dB
#   0.75   ->   0 dB
#   1.0    -> +10 dB
#
# The firmware quantizes to 1024 steps (10-bit), matching X32; tests round
# values via `fader_quantize` before comparing.

def fader_to_db(f: float) -> float:
    if f >= 0.5:
        return f * 40.0 - 30.0     # -10 .. +10 dB
    if f >= 0.25:
        return f * 80.0 - 50.0     # -30 .. -10 dB
    if f >= 0.0625:
        return f * 160.0 - 70.0    # -60 .. -30 dB
    if f > 0.0:
        return f * 480.0 - 90.0    # -inf .. -60 dB (asymptotic)
    return float("-inf")


def db_to_fader(d: float) -> float:
    if d < -60.0:
        return (d + 90.0) / 480.0
    if d < -30.0:
        return (d + 70.0) / 160.0
    if d < -10.0:
        return (d + 50.0) / 80.0
    if d <= 10.0:
        return (d + 30.0) / 40.0
    raise ValueError(f"dB out of range: {d}")


def fader_quantize(f: float) -> float:
    """Snap a 0..1 fader value to the firmware's 1024-step grid.

    The 0.5 offset is the X32 convention -- it biases rounding so the
    cardinal float points (0.0, 0.0625, 0.25, 0.5, 0.75, 1.0) all land
    exactly on a quantization step instead of straddling two."""
    if f < 0.0:
        return 0.0
    if f > 1.0:
        return 1.0
    return int(f * 1023.5) / 1023.0


# ---------------------------------------------------------------------------
# OSC encoding / decoding
# ---------------------------------------------------------------------------

def _pad4(b: bytes) -> bytes:
    """OSC requires every chunk be 4-byte aligned, NUL-padded."""
    pad = (-len(b)) % 4
    return b + b"\x00" * pad


def _osc_string(s: str) -> bytes:
    """OSC string = UTF-8 bytes + NUL + zero-or-more NULs to 4-byte boundary."""
    return _pad4(s.encode("utf-8") + b"\x00")


def _osc_blob(blob: bytes) -> bytes:
    return _pad4(struct.pack(">I", len(blob)) + blob)


# Allowed OSC arg type. We accept Python bool (encoded as int 0/1, since
# the firmware uses int 0/1 instead of OSC ,T/,F booleans on the wire),
# int (i32), float (f32), str (s), and bytes (b blob).
OscArg = Union[int, float, str, bytes, bool]


def encode_osc(address: str, *args: OscArg) -> bytes:
    """Encode a single OSC message (no bundle wrapper)."""
    if not address.startswith("/"):
        raise ValueError(f"OSC address must start with '/': {address!r}")

    typetag_chars = [","]
    payload = bytearray()
    for a in args:
        if isinstance(a, bool):
            # X32-style: bool on the wire is int 0 or 1 with type 'i'.
            typetag_chars.append("i")
            payload += struct.pack(">i", 1 if a else 0)
        elif isinstance(a, int):
            typetag_chars.append("i")
            payload += struct.pack(">i", a)
        elif isinstance(a, float):
            typetag_chars.append("f")
            payload += struct.pack(">f", a)
        elif isinstance(a, str):
            typetag_chars.append("s")
            payload += _osc_string(a)
        elif isinstance(a, (bytes, bytearray, memoryview)):
            typetag_chars.append("b")
            payload += _osc_blob(bytes(a))
        else:
            raise TypeError(f"Unsupported OSC arg type: {type(a).__name__}")

    typetag = "".join(typetag_chars)
    return _osc_string(address) + _osc_string(typetag) + bytes(payload)


@dataclass
class OscMessage:
    address: str
    typetag: str   # always starts with ','
    args: tuple    # parsed args (int / float / str / bytes)

    def __repr__(self) -> str:
        return f"OscMessage({self.address!r}, {self.typetag!r}, {self.args!r})"


def decode_osc(data: bytes) -> OscMessage:
    """Decode one OSC message (no bundle handling). Raises on malformed input.

    Bundles ('#bundle\\x00...' prefix) aren't expected from this firmware
    -- it sends discrete messages, one per SLIP frame -- so this decoder
    rejects them rather than silently picking the first child."""
    if data.startswith(b"#bundle\x00"):
        raise ValueError("OSC bundle decoding not implemented (firmware sends single messages)")

    # Address
    addr_end = data.find(b"\x00")
    if addr_end < 0:
        raise ValueError("OSC: no NUL after address")
    address = data[:addr_end].decode("utf-8")
    pos = ((addr_end // 4) + 1) * 4   # advance past NUL + alignment padding

    # Typetag
    if pos >= len(data) or data[pos:pos + 1] != b",":
        # Some encoders omit the typetag entirely (rare; not spec-conformant).
        # Treat it as ',' (no args).
        return OscMessage(address=address, typetag=",", args=())
    tt_end = data.find(b"\x00", pos)
    if tt_end < 0:
        raise ValueError("OSC: no NUL after typetag")
    typetag = data[pos:tt_end].decode("ascii")
    pos = ((tt_end // 4) + 1) * 4

    # Args — one per char after the leading comma
    args = []
    for c in typetag[1:]:
        if c == "i":
            if pos + 4 > len(data):
                raise ValueError("OSC: truncated 'i' arg")
            args.append(struct.unpack(">i", data[pos:pos + 4])[0])
            pos += 4
        elif c == "f":
            if pos + 4 > len(data):
                raise ValueError("OSC: truncated 'f' arg")
            args.append(struct.unpack(">f", data[pos:pos + 4])[0])
            pos += 4
        elif c == "s":
            s_end = data.find(b"\x00", pos)
            if s_end < 0:
                raise ValueError("OSC: no NUL in 's' arg")
            args.append(data[pos:s_end].decode("utf-8"))
            pos = ((s_end // 4) + 1) * 4
        elif c == "b":
            if pos + 4 > len(data):
                raise ValueError("OSC: truncated 'b' length")
            (n,) = struct.unpack(">I", data[pos:pos + 4])
            pos += 4
            if pos + n > len(data):
                raise ValueError("OSC: truncated 'b' payload")
            args.append(bytes(data[pos:pos + n]))
            pos += n
            pos = ((pos + 3) // 4) * 4  # align
        elif c in ("T", "F"):
            args.append(c == "T")
        elif c == "N":
            args.append(None)
        elif c == "I":
            args.append(float("inf"))
        else:
            raise ValueError(f"OSC: unsupported typetag char {c!r}")

    return OscMessage(address=address, typetag=typetag, args=tuple(args))


# ---------------------------------------------------------------------------
# SLIP framing (RFC 1055)
# ---------------------------------------------------------------------------
SLIP_END = 0xC0
SLIP_ESC = 0xDB
SLIP_ESC_END = 0xDC
SLIP_ESC_ESC = 0xDD


def slip_encode(packet: bytes) -> bytes:
    """Wrap a raw OSC packet in a SLIP frame. We use both leading and
    trailing END bytes (RFC 1055 recommended): the leading END flushes
    any garbage from a previous incomplete frame, the trailing END
    terminates this frame."""
    out = bytearray([SLIP_END])
    for b in packet:
        if b == SLIP_END:
            out.extend([SLIP_ESC, SLIP_ESC_END])
        elif b == SLIP_ESC:
            out.extend([SLIP_ESC, SLIP_ESC_ESC])
        else:
            out.append(b)
    out.append(SLIP_END)
    return bytes(out)


class SlipDecoder:
    """Stateful SLIP decoder. Feed it bytes; pull complete frames out.

    Plain-text bytes (anything not bounded by 0xC0) are silently
    discarded — the test harness only cares about OSC frames. (The
    firmware's CLI text lines come back on the same port; tests that
    want to see them should bypass this decoder and read the serial
    port directly.)"""

    def __init__(self) -> None:
        self._buf = bytearray()
        self._in_frame = False
        self._escaped = False
        self._frames: deque[bytes] = deque()

    def feed(self, data: bytes) -> None:
        for b in data:
            if not self._in_frame:
                # Skip plain-text bytes until we see a frame start.
                if b == SLIP_END:
                    self._in_frame = True
                    self._buf.clear()
                    self._escaped = False
                continue
            # In frame
            if self._escaped:
                if b == SLIP_ESC_END:
                    self._buf.append(SLIP_END)
                elif b == SLIP_ESC_ESC:
                    self._buf.append(SLIP_ESC)
                else:
                    # Protocol violation — discard the partial frame.
                    self._in_frame = False
                    self._buf.clear()
                self._escaped = False
            elif b == SLIP_ESC:
                self._escaped = True
            elif b == SLIP_END:
                # Frame terminator. Empty frames (back-to-back ENDs) are
                # legal and treated as separators only.
                if self._buf:
                    self._frames.append(bytes(self._buf))
                self._buf.clear()
                # Stay "in frame" — the trailing END can also be the
                # leading END of the next frame, so don't drop state.
            else:
                self._buf.append(b)

    def pop(self) -> Optional[bytes]:
        return self._frames.popleft() if self._frames else None


# ---------------------------------------------------------------------------
# OSC client (serial transport)
# ---------------------------------------------------------------------------

class OscClient:
    """Thread-safe SLIP-OSC client over a pyserial connection.

    Usage:
        with OscClient.open('COM7') as cli:
            cli.send('/info')
            msg = cli.recv(timeout=1.0)
            assert msg.address == '/info'

    Reads are pumped on a background thread into a queue; sends are
    direct (the firmware-side bridge throttle isn't in our way -- we're
    talking to the firmware directly, not via the bridge). If a future
    iteration needs to bypass our queue (e.g. very tight latency
    measurements), use `recv_match(predicate, ...)` which filters at
    pop time."""

    @classmethod
    def open(cls, port: str, baud: int = 115200, timeout: float = 0.05) -> "OscClient":
        ser = serial.Serial(
            port=port,
            baudrate=baud,
            timeout=timeout,
            # Same as the bridge: do NOT touch DTR/RTS; the Teensy
            # composite USB-Audio path is sensitive to control-line
            # transitions on the CDC endpoint.
            dsrdtr=False,
            rtscts=False,
        )
        return cls(ser)

    def __init__(self, ser: serial.Serial) -> None:
        self._ser = ser
        self._slip = SlipDecoder()
        self._inbox: deque[OscMessage] = deque()
        self._inbox_lock = threading.Lock()
        self._inbox_event = threading.Event()
        self._stop = threading.Event()
        self._reader = threading.Thread(target=self._reader_loop, daemon=True)
        self._reader.start()

    # --- context manager ---------------------------------------------------
    def __enter__(self) -> "OscClient":
        return self

    def __exit__(self, *_exc) -> None:
        self.close()

    def close(self) -> None:
        self._stop.set()
        try:
            if self._ser.is_open:
                self._ser.close()
        except Exception:
            pass

    # --- background reader -------------------------------------------------
    def _reader_loop(self) -> None:
        while not self._stop.is_set():
            try:
                data = self._ser.read(4096)
            except Exception:
                break
            if not data:
                continue
            self._slip.feed(data)
            while True:
                frame = self._slip.pop()
                if frame is None:
                    break
                try:
                    msg = decode_osc(frame)
                except Exception:
                    # Malformed OSC -- drop it. Tests that care can
                    # inspect the raw stream by reading the serial port
                    # directly before constructing the client.
                    continue
                with self._inbox_lock:
                    self._inbox.append(msg)
                    self._inbox_event.set()

    # --- send --------------------------------------------------------------
    def send(self, address: str, *args: OscArg) -> None:
        """Encode and SLIP-frame an OSC message, write it to the port."""
        packet = encode_osc(address, *args)
        self._ser.write(slip_encode(packet))
        # Don't flush() per call — the OS buffers small writes anyway,
        # and pyserial's flush() forces an I/O sync that hurts latency
        # measurements in test_volume_responsiveness.

    def send_text(self, line: str) -> None:
        """Send a plain-text CLI command (no SLIP framing). Useful for
        invoking firmware debug commands during tests."""
        if not line.endswith("\n"):
            line += "\n"
        self._ser.write(line.encode("utf-8"))

    # --- receive -----------------------------------------------------------
    def recv(self, timeout: float = 1.0) -> Optional[OscMessage]:
        """Pop the next OSC message off the queue; None on timeout."""
        deadline = time.monotonic() + timeout
        while True:
            with self._inbox_lock:
                if self._inbox:
                    msg = self._inbox.popleft()
                    if not self._inbox:
                        self._inbox_event.clear()
                    return msg
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            self._inbox_event.wait(timeout=remaining)

    def recv_match(self, predicate, timeout: float = 1.0) -> Optional[OscMessage]:
        """Pop the next message matching `predicate(msg) -> bool`. Discards
        non-matching messages while waiting. Returns None on timeout.

        Useful when /xremote echoes are interleaved with the reply you
        actually care about."""
        deadline = time.monotonic() + timeout
        while True:
            msg = self.recv(timeout=max(0.0, deadline - time.monotonic()))
            if msg is None:
                return None
            if predicate(msg):
                return msg

    def drain(self) -> int:
        """Discard any queued messages. Returns the number dropped.
        Tests call this between cases so subscription echoes from a
        previous test don't leak into the next one."""
        with self._inbox_lock:
            n = len(self._inbox)
            self._inbox.clear()
            self._inbox_event.clear()
        return n

    # --- request/reply convenience ----------------------------------------
    def query(self, address: str, *args: OscArg, timeout: float = 1.0) -> Optional[OscMessage]:
        """Send `address args`, then wait for the next message on the
        same address. Most read-only OSC operations on this firmware
        echo the requested address with the current value."""
        self.drain()
        self.send(address, *args)
        return self.recv_match(lambda m: m.address == address, timeout=timeout)
