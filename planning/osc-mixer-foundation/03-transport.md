# 03 — Transport

## Decision

**v1 transport: SLIP-encoded OSC over the USB CDC serial endpoint**, multiplexed with plain ASCII debug text and dotted-path CLI input on the same stream.

This is the only transport in scope for this epic. Future transports (UDP-over-Ethernet, OSCQuery) are noted at the end of this doc and parked for later.

## Why this transport

The Teensy 4.1 firmware is built with `USB_MIDI_AUDIO_SERIAL` so the USB descriptor exposes three things simultaneously:

- USB Audio Class device (so the host sees a sound card)
- USB MIDI device (so the host sees a MIDI port)
- USB CDC serial device (so the host sees a COM port)

The CDC serial endpoint is sitting there, free, on the same physical USB cable. We can use it for OSC framing without adding any hardware, without changing the USB type, and without conflicting with the USB audio that's already running.

Considered and rejected for v1:

- **MIDI CC** — only 7-bit, awkward for floating-point gains, hierarchical addressing requires NRPN gymnastics, no good way to send strings or blobs. Wrong tool for a mixer protocol.
- **USB HID** — bidirectional and not COM-port-exclusive, but Teensy's standard USB types don't include HID alongside audio+MIDI+serial. Adding it requires a custom `usb_desc.h` which is more invasive than what we get for free with CDC.
- **Ethernet UDP** — the right answer for v2 if/when the adaptor PCB has an Ethernet magjack. v1 doesn't because we don't know if the magjack is there. Architected for swap-in later.

Considered for v1 and committed to:

- **SLIP-encoded OSC over USB CDC** — zero new hardware, coexists with USB audio/MIDI, well-supported by CNMAT/OSC's `SLIPEncodedUSBSerial` wrapper, host-side decoding is trivial in any language.

## What SLIP framing buys us

OSC packets are binary blobs of arbitrary length. To put them on a serial stream, we need a way to know where one packet ends and the next begins. SLIP (RFC 1055) is a tiny framing protocol designed for exactly this:

- `0xC0` (`END`) marks the boundary between frames.
- `0xDB` (`ESC`) escapes any literal `0xC0` or `0xDB` inside the payload.
- Decoding is a state machine of about 20 lines.

CNMAT/OSC ships `SLIPEncodedSerial` and `SLIPEncodedUSBSerial` wrappers that do the encoding for us. We instantiate one against the Teensy's USB CDC `Serial` object and write OSC bundles through it; on the host side we feed received bytes through a SLIP decoder and parse the resulting payloads as OSC.

## Multiplexing rules

**The same USB CDC stream carries three different things at the same time.** This is the most interesting transport decision in the design, because it sounds like it shouldn't work and in fact works very cleanly because of how `0xC0` is positioned in the byte space.

### Output direction (Teensy → host)

Two streams interleaved:

- **OSC frames** written via `SLIPEncodedUSBSerial`. Each frame is `0xC0 [escaped payload bytes] 0xC0`.
- **Plain ASCII debug text** written via `Serial.print` / `Serial.println`. Boot banners, status prints, error messages, anything human-readable.

A SLIP decoder on the host runs a state machine:

- **State IDLE:** waiting for a frame to begin. If a byte is `0xC0`, transition to RECEIVING and clear the buffer. **Otherwise, the byte is "garbage" between frames — discarded by the SLIP decoder.**
- **State RECEIVING:** accumulating a frame. If a byte is `0xC0`, deliver the buffer as a complete payload and return to IDLE.

Because plain ASCII text contains no `0xC0` bytes (it's outside the printable range and not a common control character), text bytes are emitted by the Teensy *between* SLIP frames and are discarded by the SLIP decoder while still being readable as text by anything else listening to the same stream. The host implementation does both:

```
for byte in serial_stream:
    slip_decoder.feed(byte)            # may emit a complete OSC frame
    if not slip_decoder.in_frame:
        text_console.feed(byte)        # accumulates ASCII lines
```

The result: the OSC tooling sees only OSC frames; a serial monitor sees only the debug text; both work simultaneously on the same connection.

**Discipline required on the firmware side:** OSC frame writes (`SLIPSerial.beginPacket() / endPacket()`) and `Serial.print*` calls must be on the main loop only, never from ISRs, and must not be interleaved mid-frame. CNMAT's `SLIPEncodedSerial::beginPacket` writes a `0xC0`; if a `Serial.println` from another code path runs between `beginPacket` and `endPacket`, the text bytes appear inside the SLIP frame and corrupt the OSC payload. Single-threaded `loop()` makes this trivially safe as long as we don't call OSC write paths from interrupt handlers.

### Input direction (host → Teensy)

Two streams interleaved here too — OSC frames (binary) and dotted-path CLI commands (text). The discrimination rule is **first-byte inspection**:

- **First byte is `0xC0`** → start of a SLIP frame. Feed subsequent bytes to the SLIP decoder; on closing `0xC0`, hand the payload to the OSC dispatcher.
- **First byte is printable ASCII** (or `\r` / `\n`) → start of a CLI line. Accumulate bytes until `\n`; hand the line to the CLI shim, which converts it to an `OSCMessage` and routes through the same dispatcher.

This works for the same reason output multiplexing works: `0xC0` is unambiguously not ASCII text. A user typing `codec.tac5212.adc.1.gain 6.0\n` cannot accidentally trigger the SLIP decoder because no character on their keyboard maps to `0xC0`.

The CLI shim is a thin (~50-line) translator. It does not have its own handler implementations — it converts the dotted-path text into an OSC message and feeds the dispatcher. See [02-osc-protocol.md](02-osc-protocol.md#the-cli-escape-hatch) for the design and rationale.

## Library choice

**Firmware side: CNMAT/OSC** with its `SLIPEncodedUSBSerial` wrapper.

CNMAT/OSC is the canonical Arduino OSC library, ships with Teensyduino (so probably available without explicit vendoring), well-supported, and is what OSCAudio is built on. We use it for both raw OSC handling (when needed) and indirectly via OSCAudio's higher-level dispatch.

**Host side: language-dependent.**

- **Python (test harness):** `python-osc` for OSC encoding/decoding, `pyserial` for the serial port, hand-rolled SLIP codec (~20 lines) or the `slip` package.
- **Rust (eventual Tauri client):** `rosc` for OSC, `serialport` for the CDC port, `slip-codec` crate for framing.
- **JavaScript (browser-based clients):** `osc.js` for OSC, Web Serial API for the port, hand-rolled SLIP.

The wire format is identical regardless of client language.

## Latency and bandwidth budget

USB CDC at full speed (12 Mbit/s) gives ~1 MB/s of practical throughput. For comparison:

- A typical OSC fader message is ~32 bytes including SLIP framing.
- A 6-channel meter blob (peak + RMS per channel) is ~64 bytes.
- At 30 Hz meter streaming: ~2 KB/s. At 50 Hz: ~3.2 KB/s.
- Fader updates from a client: << 1 KB/s even during heavy automation.

We have ~500× headroom on bandwidth. The bottleneck will be the firmware's `loop()` rate, not the wire.

Latency is bounded by the USB CDC polling interval (1 ms for full speed) plus `loop()` execution time. Round-trip from a client fader move to the audio graph applying the change should be well under 5 ms in the absence of other load. This is good enough for live mixing — engineers don't perceive control latency below ~10 ms.

## Future transports

**v2 — OSC over UDP via Ethernet (Teensy 4.1).** If the adaptor PCB has the Ethernet magjack populated (currently unknown), this is the obvious next transport. Benefits:

- Multiple clients simultaneously (FOH + monitor + phones).
- LAN-wide reach — TouchOSC on a phone, QLab on another machine, all without USB.
- USB CDC stays free for debug/log output.
- Allows OSCQuery for self-describing endpoints.

The firmware's transport layer should be abstracted so that adding a UDP transport is "instantiate `EthernetOscTransport` alongside `SlipOscTransport` and feed both into the dispatcher" — handlers don't change.

**v3 — OSCQuery + mDNS for discoverability.** OSCQuery (Vidvox proposal) is a JSON-over-HTTP schema announcement that lets clients enumerate the OSC tree, including types, ranges, descriptions, units. Clients like Open Stage Control auto-generate UIs from OSCQuery. Requires HTTP, so this is downstream of the Ethernet transport.

**Not on the roadmap:** WebSockets, MQTT, Bluetooth audio. None of these add value over UDP for a wired pro audio context.

## Failure modes and recovery

What can go wrong with this transport, and how the system reacts:

- **Host serial monitor hogs the COM port.** The Teensy bootloader handles this gracefully — uploading via PlatformIO will reset and retake the port. The user has to close their monitor before upload (already documented in the project README).
- **Client crashes mid-frame.** The next byte the host writes is presumably plain text or another `0xC0`. If it's `0xC0`, the half-frame in the decoder's buffer is silently discarded (the decoder treats `0xC0` as both end-of-frame and start-of-frame — a half-received frame is a frame with bad OSC data, which the parser rejects with an error). The system self-resyncs.
- **Teensy reboots.** The host's serial port stays open across the reboot (the CDC re-enumerates without a full disconnect), so the next frame the firmware writes after boot is received normally. The boot banner appears as plain text and is read by the text console.
- **Garbage byte injection** (e.g. EMI on the cable). The SLIP decoder's state machine treats unknown bytes in IDLE state as garbage and discards them. A garbage byte mid-frame results in a corrupted payload that the OSC parser rejects. The next valid frame works normally.

The transport is robust to all the failure modes we expect in practice.

## What's NOT in scope for transport in this epic

- Encryption / authentication — not relevant for a wired USB connection on a single machine.
- Compression — bandwidth headroom is too large to bother.
- Reliable delivery — OSC over SLIP-USB-CDC is already reliable; USB CDC is lossless and in-order.
- Fragmentation / large message handling — meter blobs are tiny, scenes are sent as bundled file references, no need for application-layer fragmentation.
