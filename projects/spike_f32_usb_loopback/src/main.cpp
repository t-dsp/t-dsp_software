// Spike: F32 USB stereo loopback through unity mixer to TDM slots 0/1.
//
// Milestone 1: TDM-only smoke test. Instantiates AudioOutputTDM_F32 and an
// F32 mixer producing silence so the audio graph compiles and links.
// USB audio classes (AudioInputUSB_F32 / AudioOutputUSB_F32) and the rest
// of the wiring will land at milestone 4 once the vendored USB core is
// updated for AUDIO_SUBSLOT_SIZE=3.

#include <Arduino.h>
#include <AudioStream_F32.h>
#include <AudioSettings_F32.h>
#include <AudioMixer_F32.h>
#include "AudioOutputTDM_F32.h"

AudioMixer4_F32     mixL;
AudioMixer4_F32     mixR;
AudioOutputTDM_F32  tdmOut;

AudioConnection_F32 patchL(mixL, 0, tdmOut, 0);
AudioConnection_F32 patchR(mixR, 0, tdmOut, 1);

void setup() {
    Serial.begin(115200);
    AudioMemory_F32(16);
    mixL.gain(0, 1.0f);
    mixR.gain(0, 1.0f);
}

void loop() {
}
