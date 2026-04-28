// Spike: F32 USB stereo loopback through unity mixer to TDM slots 0/1.
//
// M4b state: AudioInputUSB_F32 / AudioOutputUSB_F32 instantiated but
// not wired into the audio graph yet. Their update() methods run every
// block (AudioStream_F32::update_all iterates all instances) which
// exercises the M4a buffer plumbing end-to-end -- but with no audible
// path: usbIn produces blocks that nothing consumes (released directly),
// and usbOut consumes blocks that nothing produces (no-op).
//
// M4c will add AudioConnection_F32 patchcords:
//   usbIn.0 -> mixL.0 -> tdmOut.0    (USB L -> TDM slot 0)
//   usbIn.1 -> mixR.0 -> tdmOut.1    (USB R -> TDM slot 1)
//   usbIn.0 -> usbOut.0              (loopback to host)
//   usbIn.1 -> usbOut.1

#include <Arduino.h>
#include <AudioStream_F32.h>
#include <AudioSettings_F32.h>
#include <AudioMixer_F32.h>
#include "AudioOutputTDM_F32.h"
#include "AudioInputUSB_F32.h"
#include "AudioOutputUSB_F32.h"

AudioInputUSB_F32   usbIn;
AudioOutputUSB_F32  usbOut;
AudioMixer4_F32     mixL;
AudioMixer4_F32     mixR;
AudioOutputTDM_F32  tdmOut;

AudioConnection_F32 patchL(mixL, 0, tdmOut, 0);
AudioConnection_F32 patchR(mixR, 0, tdmOut, 1);

void setup() {
    Serial.begin(115200);
    AudioMemory_F32(32);  // raised from 16 -- USB classes alloc per-block
    mixL.gain(0, 1.0f);
    mixR.gain(0, 1.0f);
}

void loop() {
}
