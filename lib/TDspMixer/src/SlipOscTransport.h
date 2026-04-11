// SlipOscTransport — byte-level I/O for SLIP-framed OSC over USB CDC.
//
// Wraps the Teensy's USB Serial stream (`Serial` when USB_MIDI_AUDIO_SERIAL
// is the USB mode) and presents a callback-based API for incoming OSC
// messages and outgoing reply bundles. Plain-text debug output via
// `Serial.print*` coexists on the same stream — the host-side demuxer
// routes bytes based on the SLIP frame-start byte 0xC0.
//
// Uses CNMAT/OSC's SLIPEncodedUSBSerial wrapper for frame-aware I/O. The
// receive path uses the endofPacket() pattern that Spike 1 validated — the
// outer loop peeks for 0xC0 in Serial, enters a frame-drain loop that
// reads bytes through SLIPSerial until endofPacket() returns true, then
// dispatches the accumulated OSCMessage.
//
// Plain-text lines (CLI input) are still routed through Serial.read() in
// the outer discriminator loop when the first byte is not 0xC0. That path
// goes to DottedCliShim.
//
// This class does NOT own the OscDispatcher — it just calls a callback
// with the parsed OSCMessage. Main.cpp wires the callback to the
// dispatcher's route() method.

#pragma once

#include <stdint.h>

class OSCMessage;
class OSCBundle;

namespace tdsp {

class SlipOscTransport {
public:
    // Callback type for incoming OSC messages. The transport passes the
    // fully-parsed message; the callback is responsible for dispatch.
    using OscMessageHandler = void (*)(OSCMessage &msg, void *userData);

    // Callback type for plain ASCII lines (CLI input). The transport
    // accumulates bytes until it sees \r or \n, then calls this with the
    // null-terminated line. The callback is responsible for routing to
    // DottedCliShim or similar.
    using CliLineHandler = void (*)(char *line, int length, void *userData);

    SlipOscTransport();

    // Must be called from setup() before any other method.
    void begin(uint32_t baudRate = 115200);

    // Register the dispatch callbacks and their user-data pointers.
    void setOscMessageHandler(OscMessageHandler handler, void *userData);
    void setCliLineHandler(CliLineHandler handler, void *userData);

    // Called from loop() every iteration. Reads bytes from Serial, routes
    // SLIP frames to the OSC handler and ASCII lines to the CLI handler.
    // Also drains any queued outgoing bundle if present.
    void poll();

    // Send an OSC bundle back to the host, SLIP-framed. Call this from a
    // handler or directly after mutating the model to echo changes.
    void sendBundle(OSCBundle &bundle);

    // Send a raw OSCMessage (convenience for single-message replies).
    void sendMessage(OSCMessage &msg);

    // Partial-frame timeout guard (ms). If a SLIP frame doesn't complete
    // within this window, the receive loop bails out so a malformed or
    // interrupted frame can't wedge the transport.
    static constexpr uint32_t kFrameTimeoutMs = 100;

private:
    OscMessageHandler _oscHandler = nullptr;
    void             *_oscHandlerData = nullptr;
    CliLineHandler    _cliHandler = nullptr;
    void             *_cliHandlerData = nullptr;

    // CLI line accumulator. Grown to 128 bytes to allow reasonably long
    // dotted-path commands with string arguments.
    static constexpr int kCliBufSize = 128;
    char _cliBuf[kCliBufSize];
    int  _cliLen = 0;
};

}  // namespace tdsp
