// spike_spdif_alex — alex6679's CURRENT async S/PDIF input + an I2S/SAI output.
//
// Mirrors his canonical example_spdif.cpp (AsyncAudioInputSPDIF3 -> AudioOutputSPDIF3)
// and ADDS an AudioOutputI2S so the SAI peripheral is brought up alongside the
// S/PDIF receiver — the exact combination that hangs with our old code.
//
// LED heartbeat (1/sec toggle): blinks => alive (his code coexists with the SAI);
// dark/frozen => still hangs (board/silicon, not software). No external source
// needed for the hang test — we only need to see whether it boots and runs.

#include <Audio.h>

AsyncAudioInputSPDIF3  spdifIn(false, false, 100, 20, 80);  // his current ctor
AudioOutputSPDIF3      spdifOut;        // canonical update driver + S/PDIF clock
AudioOutputI2S         i2sOut;          // brings up SAI1 — the coexistence test

AudioConnection patchOutL(spdifIn, 0, spdifOut, 0);
AudioConnection patchOutR(spdifIn, 1, spdifOut, 1);
AudioConnection patchI2sL(spdifIn, 0, i2sOut, 0);
AudioConnection patchI2sR(spdifIn, 1, i2sOut, 1);

elapsedMillis hb;

void setup() {
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);    // solid during init; loop() makes it blink
    AudioMemory(40);
    Serial.begin(115200);
}

void loop() {
    if (hb >= 500) {
        hb = 0;
        digitalToggle(LED_BUILTIN);
        Serial.printf("alive up=%lus  spdif=%s  inFreq=%.0f  cpuMax=%.1f%%  memMax=%u\n",
                      (unsigned long)(millis() / 1000),
                      AsyncAudioInputSPDIF3::isLocked() ? "LOCKED" : "no-signal",
                      spdifIn.getInputFrequency(),
                      AudioProcessorUsageMax(),
                      AudioMemoryUsageMax());
    }
}
