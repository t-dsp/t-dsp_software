// Spike 1 — OSC + F32 + library vendoring + cores overlay + multiplex foundation
//
// Validates the foundation for the T-DSP mixer framework. Pure software loopback
// over USB Audio (no TAC5212 hardware). Three things coexist on the same USB CDC
// stream:
//
//   1. SLIP-OSC binary frames     -> dispatched to mixer-domain or OSCAudio handlers
//   2. Plain Serial.println text  -> human-readable boot/heartbeat output
//   3. Dotted-path CLI input      -> ASCII lines that are converted to OSC and
//                                    routed through the same dispatcher as #1
//
// The 0xC0 first-byte discriminator routes incoming bytes between #1 and #3.
// SLIP framing on output makes #1 and #2 transparently coexist on the host side.
//
// See planning/osc-mixer-foundation/07-spike-plan.md#spike-1-foundation

#include <Arduino.h>
#include <Audio.h>            // stock Teensy Audio Library (framework-bundled for Spike 1)

// OpenAudio_ArduinoLibrary.h is the aggregator header but it transitively
// pulls in output_i2s_quad_f32.h, which uses C++17 inline variables that
// gcc 5.4.1 (Teensy's ARM toolchain) cannot parse. Include only the specific
// F32 headers we need for Spike 1. Toolchain upgrade or upstream patch is
// the longer-term fix — see the Spike 1 report.
#include "AudioStream_F32.h"  // AudioConnection_F32, AudioStream_F32 base
#include "AudioMixer_F32.h"   // AudioMixer4_F32
#include "AudioConvert_F32.h" // AudioConvert_I16toF32, AudioConvert_F32toI16
#include "play_queue_f32.h"   // AudioPlayQueue_F32 (needed by USB_Audio_F32.h)
#include "record_queue_f32.h" // AudioRecordQueue_F32 (needed by USB_Audio_F32.h)
#include "USB_Audio_F32.h"    // AudioInputUSB_F32, AudioOutputUSB_F32

#include <OSCBundle.h>
#include <OSCMessage.h>
#include <SLIPEncodedSerial.h>   // upstream CNMAT/OSC; SLIPEncodedUSBSerial is a
                                 // template alias defined in this header

#include "OSCAudioBase.h"     // OSCAudio dispatch + OSC<Class> wrappers

// =========================================================================
// Audio graph: USB In (F32, stereo) -> AudioMixer4_F32 (L,R) -> USB Out (F32)
// Pure software loopback. /test/gain f scales both L and R mixer slot 0.
// =========================================================================

AudioInputUSB_F32   usbIn;
AudioMixer4_F32     mixL;
AudioMixer4_F32     mixR;
AudioOutputUSB_F32  usbOut;

AudioConnection_F32 c1(usbIn, 0, mixL, 0);
AudioConnection_F32 c2(usbIn, 1, mixR, 0);
AudioConnection_F32 c3(mixL,  0, usbOut, 0);
AudioConnection_F32 c4(mixR,  0, usbOut, 1);

// One stock-I16 OSCAudio wrapper instance so the /teensy*/audio/ debug
// surface has *something* to dispatch to. Not connected to any audio path.
// OSCAudio's autogen wrappers cover I16 Audio Library classes only; F32
// wrappers are a planned giveback PR (see 04-dependencies.md).
OSCAudioAmplifier amp("amp");

// OSCAudio declares `extern void listObjects(void)` and calls it from
// OSCAudioBase's constructor (debug helper). We provide a minimal definition
// here so the linker is happy. Walk the linked list and print object names
// so the boot output shows what OSC objects exist.
void listObjects(void) {
    OSCAudioBase* obj = OSCAudioBase::getFirst();
    while (obj != nullptr) {
        Serial.printf("OSCAudio object: %s\n", obj->name);
        obj = obj->getNext();
    }
}

// =========================================================================
// SLIP-OSC transport over USB CDC
// =========================================================================

SLIPEncodedUSBSerial SLIPSerial(thisBoardsSerialUSB);

// One reused OSCMessage / OSCBundle pair to avoid per-packet allocation.
// (CNMAT/OSC tip discovered in OSCAudio: call .empty() to recycle.)
OSCMessage incomingMsg;
OSCBundle  replyBundle;

// =========================================================================
// Mixer-domain handler: /test/gain f x
// Calls the F32 mixer methods directly. Bypasses OSCAudio dispatch.
// This is the X32 mixer-surface pattern in miniature.
// =========================================================================

// CNMAT/OSC's OSCMessage::dispatch() takes a 1-arg callback: void(OSCMessage&)
void handleTestGain(OSCMessage& msg) {
    if (!msg.isFloat(0)) {
        Serial.println("test.gain: expected float arg");
        return;
    }
    float g = msg.getFloat(0);
    if (g < 0.0f) g = 0.0f;
    if (g > 4.0f) g = 4.0f;  // some headroom for testing
    mixL.gain(0, g);
    mixR.gain(0, g);
    Serial.printf("test.gain = %.3f\n", g);

    // Echo the new value back as an OSC reply
    OSCMessage echo("/test/gain");
    echo.add(g);
    replyBundle.add(echo);
}

// /test/hello — respond with banner-like info so the harness can confirm the
// device is alive after opening the COM port. The original boot banner is
// printed once in setup() and may have already scrolled past (or never reached
// the host) by the time pyserial opens the port. /test/hello is the
// deterministic alternative.
void handleTestHello(OSCMessage& msg) {
    Serial.println("hello: spike_osc_foundation alive");
    OSCMessage reply("/test/hello");
    reply.add("spike_osc_foundation");
    reply.add((int32_t)1);  // version
    replyBundle.add(reply);
}

// =========================================================================
// Top-level routing: dispatch incoming OSCMessage to the right subtree
// =========================================================================

#if defined(OSCAUDIO_DEBUG_SURFACE)
// Plain function (not lambda) for OSCAudio passthrough. Lambdas with no
// captures should decay to function pointers, but using a regular function
// avoids any subtlety in CNMAT/OSC's callback type handling.
void routeOscAudioPassthrough(OSCMessage& msg, int addrOff) {
    // OSCAudio bug workaround: staticPrepareReplyResult() in OSCUtils.cpp
    // calls reply.getOSCMessage(reply.size() - 1) without bounds-checking.
    // When the bundle is empty, the index is -1 and the subsequent
    // pLastMsg->size() dereferences NULL/garbage, wedging the loop on
    // Teensy (no MMU). OSCAudio's design assumes reply bundles arrive
    // pre-loaded with at least one placeholder message — its own examples
    // satisfy this via the OSCSubscribe update flow. Direct callers like
    // this spike must seed the placeholder themselves. Tracked in
    // planning/osc-mixer-foundation/upstream-prs.md as PR 1 to OSCAudio.
    if (replyBundle.size() == 0) {
        replyBundle.add("/oscaudio/reply");
    }
    OSCAudioBase::routeAll(msg, addrOff, replyBundle);
}
#endif

void routeOscMessage(OSCMessage& msg) {
    if (msg.hasError()) {
        Serial.println("routeOscMessage: msg has error");
        return;
    }

    // Mixer-surface routes go to our own handlers (no OSCAudio involvement).
    msg.dispatch("/test/gain",  handleTestGain);
    msg.dispatch("/test/hello", handleTestHello);

    // OSCAudio /audio passthrough — debug surface for poking individual
    // Audio Library objects via /teensy*/audio/<obj>/<method>.
    // Build-flag-gated; production builds compile this out.
#if defined(OSCAUDIO_DEBUG_SURFACE)
    msg.route("/teensy*/audio", routeOscAudioPassthrough);
#endif
}

// =========================================================================
// Dotted-path CLI shim: ~50 lines that convert "test.gain 0.5" -> OSCMessage
// and route it through the same dispatcher. No independent handlers.
// Single source of truth = the address tree on the dispatcher side.
// =========================================================================

char cliBuf[128];
size_t cliLen = 0;

bool isFloatLiteral(const char* s) {
    if (!*s) return false;
    bool sawDot = false, sawDigit = false;
    if (*s == '+' || *s == '-') ++s;
    while (*s) {
        if (*s == '.') { if (sawDot) return false; sawDot = true; }
        else if (*s >= '0' && *s <= '9') sawDigit = true;
        else return false;
        ++s;
    }
    return sawDigit && sawDot;  // require a dot to disambiguate from int
}

bool isIntLiteral(const char* s) {
    if (!*s) return false;
    if (*s == '+' || *s == '-') ++s;
    if (!*s) return false;
    while (*s) {
        if (*s < '0' || *s > '9') return false;
        ++s;
    }
    return true;
}

void runCliLine(char* line) {
    // Tokenize: first token is the dotted address, remaining tokens are args.
    char* addrTok = strtok(line, " \t");
    if (!addrTok) return;

    // Convert dots to slashes, prepend leading slash.
    char addrBuf[96];
    addrBuf[0] = '/';
    size_t i = 0;
    for (; addrTok[i] && i < sizeof(addrBuf) - 2; ++i) {
        addrBuf[i + 1] = (addrTok[i] == '.') ? '/' : addrTok[i];
    }
    addrBuf[i + 1] = '\0';

    OSCMessage msg(addrBuf);
    char* argTok;
    while ((argTok = strtok(nullptr, " \t")) != nullptr) {
        if (isFloatLiteral(argTok))      msg.add((float)atof(argTok));
        else if (isIntLiteral(argTok))   msg.add((int32_t)atoi(argTok));
        else                             msg.add(argTok);  // string
    }

    Serial.printf("cli: %s\n", addrBuf);
    routeOscMessage(msg);
}

// =========================================================================
// Setup
// =========================================================================

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 2000) { /* wait for USB CDC */ }

    SLIPSerial.begin(115200);

    AudioMemory_F32(40);

    mixL.gain(0, 1.0f);
    mixR.gain(0, 1.0f);

    Serial.println("================================");
    Serial.println("  Spike 1 — OSC Foundation");
    Serial.println("================================");
    Serial.println("transport: SLIP-OSC + plain text + dotted CLI on USB CDC");
    Serial.println("audio:     USB In F32 -> mixer -> USB Out F32");
    Serial.println("try:       test.gain 0.5    (CLI)");
    Serial.println("           /test/gain f 0.5 (OSC binary)");
    Serial.println();
}

// =========================================================================
// Main loop
// =========================================================================

unsigned long lastHeartbeat = 0;

void loop() {
    // Heartbeat every 30s — proves multiplex stays clean over time
    if (millis() - lastHeartbeat >= 30000) {
        lastHeartbeat = millis();
        Serial.printf("heartbeat: %lu ms uptime\n", millis());
    }

    // Read incoming bytes; first-byte discriminator routes between SLIP-OSC
    // and dotted-path CLI.
    //
    // SUBTLE: SLIPEncodedSerial wraps the same Serial object as the outer loop.
    // Its available() method is NOT a stateless byte counter — it advances the
    // SLIP state machine and consumes bytes from Serial as a side effect. Once
    // we've decided "this is a SLIP frame" (peek == 0xC0), we must hand the
    // stream to SLIPSerial via the endofPacket() loop pattern that CNMAT/OSC's
    // own examples use. Trying to interleave SLIPSerial.available()/read()
    // with Serial.peek()/read() causes a race where SLIPSerial consumes the
    // start byte, returns 0 (frame body not yet arrived), and the outer loop
    // then misroutes the body bytes through the CLI branch.
    while (Serial.available()) {
        int firstByte = Serial.peek();
        if (firstByte < 0) break;

        if (firstByte == 0xC0) {
            // SLIP frame start — let SLIPSerial own the bytes until end of frame.
            incomingMsg.empty();
            unsigned long frameStart = millis();
            bool timedOut = false;
            while (!SLIPSerial.endofPacket()) {
                if (SLIPSerial.available()) {
                    int c = SLIPSerial.read();
                    if (c >= 0) incomingMsg.fill((uint8_t)c);
                }
                // Partial-frame guard: if the rest of the frame doesn't arrive
                // within 100ms, give up. Better to drop one bad frame than to
                // wedge the loop forever.
                if (millis() - frameStart > 100) {
                    timedOut = true;
                    break;
                }
            }
            if (!incomingMsg.hasError() && !timedOut) {
                routeOscMessage(incomingMsg);
            }
        } else if (firstByte == '\r' || firstByte == '\n') {
            // Bare line terminator — drop it; if we have a buffered line, run it.
            Serial.read();  // consume
            if (cliLen > 0) {
                cliBuf[cliLen] = '\0';
                runCliLine(cliBuf);
                cliLen = 0;
            }
        } else {
            // Plain ASCII byte — accumulate into CLI line buffer.
            int b = Serial.read();
            if (cliLen < sizeof(cliBuf) - 1) {
                cliBuf[cliLen++] = (char)b;
            }
        }
    }

    // If we accumulated reply messages, send them as one bundle and reset.
    if (replyBundle.size() > 0) {
        SLIPSerial.beginPacket();
        replyBundle.send(SLIPSerial);
        SLIPSerial.endPacket();
        replyBundle.empty();
    }
}
