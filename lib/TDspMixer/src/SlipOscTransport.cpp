#include "SlipOscTransport.h"

#include <Arduino.h>
#include <OSCBundle.h>
#include <OSCMessage.h>
#include <SLIPEncodedSerial.h>  // CNMAT/OSC's SLIPEncodedUSBSerial template

namespace tdsp {

// Shared SLIP-wrapped Serial instance. The transport holds one of these
// across all instances (there's only ever one instance in practice, but
// this keeps the global state contained).
static SLIPEncodedUSBSerial g_slipSerial(thisBoardsSerialUSB);

SlipOscTransport::SlipOscTransport() {
    _cliLen = 0;
    for (int i = 0; i < kCliBufSize; ++i) _cliBuf[i] = 0;
}

void SlipOscTransport::begin(uint32_t baudRate) {
    // NOTE: Serial.begin() is typically called by main.cpp before this
    // class runs. SLIPSerial.begin() is a no-op on USB CDC — the underlying
    // Serial's baud rate comes from the host. We call it anyway for
    // consistency with CNMAT/OSC examples.
    g_slipSerial.begin(baudRate);
}

void SlipOscTransport::setOscMessageHandler(OscMessageHandler handler, void *userData) {
    _oscHandler = handler;
    _oscHandlerData = userData;
}

void SlipOscTransport::setCliLineHandler(CliLineHandler handler, void *userData) {
    _cliHandler = handler;
    _cliHandlerData = userData;
}

// Control-write timestamp: after any control write (snapshot reply,
// fader echo), streaming broadcasts AND incoming frame processing are
// suppressed for a cooldown period. This prevents CDC burst contention
// with USB Audio isochronous transfers on the shared USB controller.
static uint32_t s_lastControlWriteMs = 0;

void SlipOscTransport::poll() {
    while (Serial.available()) {
        int firstByte = Serial.peek();
        if (firstByte < 0) break;

        if (firstByte == 0xC0) {
            // Throttle: one OSC frame per poll() call, with minimum gap.
            // Remaining data sits safely in the serial RX buffer
            // (4 KB at Hi-Speed) until the next loop() iteration.
            {
                uint32_t now = millis();
                if (now - _lastFrameMs < kFrameGapMs) return;
            }

            // SLIP frame start. Drain the frame using the endofPacket
            // pattern validated by Spike 1.
            OSCMessage msg;
            msg.empty();
            unsigned long frameStart = millis();
            while (!g_slipSerial.endofPacket()) {
                if (g_slipSerial.available()) {
                    int c = g_slipSerial.read();
                    if (c >= 0) msg.fill((uint8_t)c);
                }
                if (millis() - frameStart > kFrameTimeoutMs) break;
            }
            if (!msg.hasError() && _oscHandler) {
                _oscHandler(msg, _oscHandlerData);
            }
            _lastFrameMs = millis();
            return;  // one frame per poll() — let remaining data wait
        }

        if (firstByte == '\r' || firstByte == '\n') {
            // Line terminator: flush the CLI buffer if non-empty.
            Serial.read();  // consume
            if (_cliLen > 0) {
                _cliBuf[_cliLen] = '\0';
                if (_cliHandler) {
                    _cliHandler(_cliBuf, _cliLen, _cliHandlerData);
                }
                _cliLen = 0;
            }
            continue;
        }

        // Plain ASCII byte: accumulate into CLI buffer.
        int b = Serial.read();
        if (_cliLen < kCliBufSize - 1) {
            _cliBuf[_cliLen++] = (char)b;
        }
        // If buffer is full, silently truncate — the handler will see the
        // full prefix and caller is expected to not send lines > 128 chars.
    }
}

// --- Non-blocking buffered SLIP send ---
//
// The CNMAT SLIPEncodedUSBSerial encoder writes byte-by-byte through
// Serial.write(). If the CDC TX buffer is full (nobody reading),
// Serial.write() blocks waiting for space, which stalls loop() and
// freezes the device. Fix: serialize the OSC payload into a scratch
// buffer, SLIP-encode it, check availableForWrite(), and either send
// the whole frame in one call or silently drop it.

static constexpr int kSlipBufSize = 2048;
static uint8_t s_slipBuf[kSlipBufSize];

// Capture OSC serialization into a byte buffer.
struct BufPrint : public Print {
    uint8_t *buf;
    int cap, len;
    BufPrint(uint8_t *b, int c) : buf(b), cap(c), len(0) {}
    size_t write(uint8_t b) override {
        if (len < cap) buf[len++] = b;
        return 1;
    }
    size_t write(const uint8_t *b, size_t s) override {
        for (size_t i = 0; i < s; ++i) write(b[i]);
        return s;
    }
};

static int slipEncode(const uint8_t *src, int srcLen,
                      uint8_t *dst, int dstMax) {
    int pos = 0;
    if (pos < dstMax) dst[pos++] = 0xC0;
    for (int i = 0; i < srcLen && pos < dstMax - 1; ++i) {
        uint8_t b = src[i];
        if (b == 0xC0)      { dst[pos++] = 0xDB; if (pos < dstMax) dst[pos++] = 0xDC; }
        else if (b == 0xDB) { dst[pos++] = 0xDB; if (pos < dstMax) dst[pos++] = 0xDD; }
        else                { dst[pos++] = b; }
    }
    if (pos < dstMax) dst[pos++] = 0xC0;
    return pos;
}

// --- Priority: control > streaming ---
//
// After a control write, streaming is suppressed for a cooldown period
// so the USB controller has uncontested time for the control transfer
// (and Audio isochronous transfers aren't disrupted by back-to-back
// CDC bulk submissions).
// After a control write (snapshot, fader echo), suppress all streaming
// broadcasts for this many ms. Prevents CDC burst contention: a control
// transfer gets exclusive bus time so Audio isochronous scheduling isn't
// disrupted. Meters/spectrum briefly pause (imperceptible at 100ms) then
// resume on the next tick.
static constexpr uint32_t kStreamingCooldownMs = 100;

// Blocking control path — for infrequent, must-deliver control messages
// (fader echoes, snapshot replies). May block up to TX_TIMEOUT_MSEC
// (120 ms) if the host isn't reading, then fast-fail via the core's
// transmit_previous_timeout flag. Records timestamp for cooldown.
static void sendSlipFrameControl(const uint8_t *oscData, int oscLen) {
    int slipLen = slipEncode(oscData, oscLen, s_slipBuf, kSlipBufSize);
    Serial.write(s_slipBuf, slipLen);
    s_lastControlWriteMs = millis();
}

// Non-blocking broadcast path — for high-frequency ephemeral data
// (meters, spectrum). Uses writeNonBlocking() which returns immediately
// if the TX buffer is full. Skipped entirely during the post-control
// cooldown window to avoid CDC burst contention with Audio.
static void sendSlipFrameBroadcast(const uint8_t *oscData, int oscLen) {
    if (millis() - s_lastControlWriteMs < kStreamingCooldownMs) return;
    int slipLen = slipEncode(oscData, oscLen, s_slipBuf, kSlipBufSize);
    Serial.writeNonBlocking((const uint8_t *)s_slipBuf, slipLen);
}

void SlipOscTransport::sendBundle(OSCBundle &bundle) {
    uint8_t oscBuf[kSlipBufSize];
    BufPrint bp(oscBuf, kSlipBufSize);
    bundle.send(bp);
    sendSlipFrameControl(oscBuf, bp.len);
}

void SlipOscTransport::sendMessage(OSCMessage &msg) {
    uint8_t oscBuf[kSlipBufSize];
    BufPrint bp(oscBuf, kSlipBufSize);
    msg.send(bp);
    sendSlipFrameControl(oscBuf, bp.len);
}

void SlipOscTransport::broadcastBundle(OSCBundle &bundle) {
    uint8_t oscBuf[kSlipBufSize];
    BufPrint bp(oscBuf, kSlipBufSize);
    bundle.send(bp);
    sendSlipFrameBroadcast(oscBuf, bp.len);
}

void SlipOscTransport::broadcastMessage(OSCMessage &msg) {
    uint8_t oscBuf[kSlipBufSize];
    BufPrint bp(oscBuf, kSlipBufSize);
    msg.send(bp);
    sendSlipFrameBroadcast(oscBuf, bp.len);
}

}  // namespace tdsp
