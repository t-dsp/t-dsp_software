// Spike: F32 USB stereo loopback through unity mixer to TDM slots 0/1.
//
// M4c state: full F32 audio graph wired:
//
//   USB host  ---24-bit/48k--->  AudioInputUSB_F32  (Q31 ring -> F32)
//                                     |
//                                     +-> AudioMixer4_F32 (unity)  -> AudioOutputTDM_F32 slot 0/1
//                                     |
//                                     +-> AudioOutputUSB_F32  ---24-bit/48k---> USB host  (loopback)
//
// The mixer is a placeholder for future DSP -- DSP blocks drop into
// the slot between usbIn and tdmOut without touching any other wiring.
//
// Loopback to host means the user can confirm round-trip by playing a
// signal from a DAW into the device and recording it back from the
// device input track. Round-trip latency ~ 2 audio blocks plus USB
// jitter (a few ms).

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

// USB L -> mixer L -> TDM slot 0 / usbOut L
AudioConnection_F32 patchUsbL_mix   (usbIn,  0, mixL,   0);
AudioConnection_F32 patchUsbL_usbOut(usbIn,  0, usbOut, 0);
AudioConnection_F32 patchMixL_tdm   (mixL,   0, tdmOut, 0);

// USB R -> mixer R -> TDM slot 1 / usbOut R
AudioConnection_F32 patchUsbR_mix   (usbIn,  1, mixR,   0);
AudioConnection_F32 patchUsbR_usbOut(usbIn,  1, usbOut, 1);
AudioConnection_F32 patchMixR_tdm   (mixR,   0, tdmOut, 1);

void setup() {
    Serial.begin(115200);
    AudioMemory_F32(32);
    mixL.gain(0, 1.0f);
    mixR.gain(0, 1.0f);
}

void loop() {
    // M4c: nothing to do in loop. M5 may add a slow status print of the
    // rx_overruns / tx_underruns counters once we're verifying audio
    // continuity over time.
}
