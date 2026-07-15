// spike_sd_xfer — test harness for tdsp::SdWriteReceiver (TDspSdXfer).
//
// Proves Option C of planning/fast-sd-transfer/PLAN.md: pushing files onto the
// Teensy 4.1's BUILTIN_SDCARD from a host over USB CDC, fast, with no MTP and no
// reflash. This sketch is deliberately minimal — just SD + the '@'-command
// ingest loop wired to the receiver, plus a few verify/poke commands.
//
// Commands (all '@'-prefixed, '\n'-terminated, fields split by 0x1f):
//   @WB=<id>\x1f<path>\x1f<bytes>[\x1f<crc32hex>]   begin a write (raw payload follows)
//   @CRC=<path>                                     -> @CRCR=<path>\x1f<crc32hex>\x1f<bytes>
//   @LS=<dir>                                        list a directory
//   @RM=<path>                                       delete a file
//   @PING                                            -> @PONG (liveness)
//
// Pair with tools/push_file_serial.ps1 for a scripted write + round-trip verify.

#include <Arduino.h>
#include <SD.h>
#include <TDspSdXfer.h>

static tdsp::SdWriteReceiver g_rx(SD);
static bool g_sdOk = false;

static void listDir(Print& reply, const char* path) {
    File dir = SD.open((path && path[0]) ? path : "/");
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        reply.printf("@LSERR=%s\n", path);
        return;
    }
    reply.printf("@LSB=%s\n", (path && path[0]) ? path : "/");
    for (;;) {
        File e = dir.openNextFile();
        if (!e) break;
        reply.printf("@LSE=%s\x1f%lu\x1f%d\n",
                     e.name(), (unsigned long)e.size(), e.isDirectory() ? 1 : 0);
        e.close();
    }
    dir.close();
    reply.println("@LSEND");
}

static bool handleControlLine(char* line, Print& reply) {
    if (!strncmp(line, "@WB=", 4))  { g_rx.begin(line + 4, reply); return true; }
    if (!strncmp(line, "@CRC=", 5)) {
        uint32_t crc = 0, bytes = 0;
        if (tdsp::SdWriteReceiver::fileCrc32(SD, line + 5, crc, bytes))
            reply.printf("@CRCR=%s\x1f%08lx\x1f%lu\n",
                         line + 5, (unsigned long)crc, (unsigned long)bytes);
        else
            reply.printf("@CRCERR=%s\n", line + 5);
        return true;
    }
    if (!strncmp(line, "@RM=", 4)) {
        bool ok = SD.remove(line + 4);
        reply.printf("@RMR=%s\x1f%s\n", line + 4, ok ? "ok" : "fail");
        return true;
    }
    if (!strncmp(line, "@LS=", 4)) { listDir(reply, line + 4); return true; }
    if (!strcmp(line, "@PING"))    { reply.println("@PONG"); return true; }
    return false;
}

void setup() {
    pinMode(LED_BUILTIN, OUTPUT);
    Serial.begin(115200);
    const uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 2000) {
        digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
        delay(100);
    }

    Serial.println("================================");
    Serial.println("  spike_sd_xfer  (TDspSdXfer test)");
    Serial.println("  @WB / @CRC / @LS / @RM / @PING");
    Serial.println("================================");

    g_sdOk = SD.begin(BUILTIN_SDCARD);
    Serial.printf("SD.begin(BUILTIN_SDCARD): %s\n", g_sdOk ? "OK" : "FAILED");
    if (!g_sdOk)
        Serial.println("Check the card is seated in the Teensy 4.1 built-in slot.");
}

void loop() {
    // Line-assembling ingest. While a write is in flight, incoming bytes are the
    // raw payload — route them to the card BEFORE the line assembler sees them.
    static char line[160];
    static size_t n = 0;
    static bool inCmd = false;

    while (Serial.available()) {
        if (g_rx.receiving()) {
            g_rx.pump(Serial, Serial);
            if (g_rx.receiving()) break;   // payload not fully arrived yet
            else continue;                 // transfer done — resume command parsing
        }
        int c = Serial.read();
        if (inCmd) {
            if (c == '\n' || n >= sizeof(line) - 1) {
                line[n] = 0;
                if (!handleControlLine(line, Serial)) Serial.printf("[?] %s\n", line);
                n = 0; inCmd = false;
            } else if (c != '\r') {
                line[n++] = (char)c;
            }
            continue;
        }
        if (c == '@') { inCmd = true; n = 0; line[n++] = '@'; }
        // stray non-'@' bytes outside a command are ignored
    }

    g_rx.tick(Serial, millis());   // abort a stalled transfer

    static uint32_t last = 0;
    const uint32_t now = millis();
    if (now - last >= 1000) { last = now; digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN)); }
}
