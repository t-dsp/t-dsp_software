// spike_spdif_min — minimal optical S/PDIF test / repeater.
//
// Stock Teensy Audio library only (no F32/OpenAudio/vendored libs, no codec).
// The on-board LED blinks at 1 Hz and USB Serial prints a once-per-second
// heartbeat, so a reboot loop is obvious (a healthy board counts up=1s,2s,3s…).
//
// Three build modes (see platformio.ini envs):
//   default            : 1 kHz sine -> SPDIF OUT (pin 14). Output/stability test.
//   -D SPDIF_LOOPBACK  : adds SPDIF IN (pin 15) metered. Self-loopback test:
//                        cable OUT->IN, RX peak should read ~0.5 (the tone).
//   -D SPDIF_PASSTHRU  : SPDIF IN (pin 15) -> SPDIF OUT (pin 14) repeater, with
//                        the received level metered. Feed an external optical
//                        source into IN; OUT mirrors it.

#include <Arduino.h>
#include <Audio.h>

#if defined(SPDIF_LOOPBACK) || defined(SPDIF_PASSTHRU)
  #define HAVE_SPDIF_IN 1
#endif

AudioOutputSPDIF3      spdifOut;                  // optical TOSLINK out, pin 14

#ifdef HAVE_SPDIF_IN
AudioInputSPDIF3       spdifIn;                   // optical TOSLINK in,  pin 15
AudioAnalyzePeak       peakL;
AudioAnalyzePeak       peakR;
AudioConnection        patchMeterL(spdifIn, 0, peakL, 0);
AudioConnection        patchMeterR(spdifIn, 1, peakR, 0);
#endif

#ifdef SPDIF_PASSTHRU
// Repeater: received audio is re-transmitted on the optical output.
AudioConnection        patchThruL(spdifIn, 0, spdifOut, 0);
AudioConnection        patchThruR(spdifIn, 1, spdifOut, 1);
#else
// Otherwise generate a 1 kHz test tone on the output.
AudioSynthWaveformSine sine1;
AudioConnection        patchOutL(sine1, 0, spdifOut, 0);
AudioConnection        patchOutR(sine1, 0, spdifOut, 1);
#endif

elapsedMillis statusTimer;

void setup() {
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);              // solid until setup finishes

    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 2000) { /* brief wait for host, no hang */ }

    Serial.println();
    Serial.println("=== spike_spdif_min ===");
#ifdef SPDIF_PASSTHRU
    Serial.println("MODE: passthrough  SPDIF IN (pin 15) -> SPDIF OUT (pin 14)");
#elif defined(SPDIF_LOOPBACK)
    Serial.println("MODE: loopback test  sine -> OUT(14), IN(15) metered");
#else
    Serial.println("MODE: output test  1 kHz sine -> SPDIF OUT (pin 14)");
#endif

    AudioMemory(24);
#ifndef SPDIF_PASSTHRU
    sine1.frequency(1000.0f);
    sine1.amplitude(0.5f);
#endif

    Serial.println("setup() complete -- audio running, heartbeat below:");
}

void loop() {
    if (statusTimer >= 1000) {
        statusTimer = 0;
        digitalToggle(LED_BUILTIN);
        Serial.printf("alive  up=%lus  cpu=%.2f%%  audioMemMax=%u",
                      (unsigned long)(millis() / 1000),
                      AudioProcessorUsageMax(),
                      AudioMemoryUsageMax());
#ifdef HAVE_SPDIF_IN
        float l = peakL.available() ? peakL.read() : -1.0f;
        float r = peakR.available() ? peakR.read() : -1.0f;
        Serial.printf("  RX peakL=%.3f peakR=%.3f", l, r);
#endif
        Serial.println();
    }
}
