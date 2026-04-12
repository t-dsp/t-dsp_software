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

void SlipOscTransport::poll() {
    while (Serial.available()) {
        int firstByte = Serial.peek();
        if (firstByte < 0) break;

        if (firstByte == 0xC0) {
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
            continue;
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

void SlipOscTransport::sendBundle(OSCBundle &bundle) {
    g_slipSerial.beginPacket();
    bundle.send(g_slipSerial);
    g_slipSerial.endPacket();
}

void SlipOscTransport::sendMessage(OSCMessage &msg) {
    g_slipSerial.beginPacket();
    msg.send(g_slipSerial);
    g_slipSerial.endPacket();
}

}  // namespace tdsp
