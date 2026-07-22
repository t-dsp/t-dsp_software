//
// Created by Nicholas Newdigate on 18/07/2020.
//

#ifndef TEENSY_RESAMPLING_SDREADER_PLAYSDRAWRESMP_H
#define TEENSY_RESAMPLING_SDREADER_PLAYSDRAWRESMP_H


#include "Arduino.h"
#include "AudioStream.h"
#include "SD.h"
#include "stdint.h"
#include "ResamplingSdReader.h"
#include "playresmp.h"

class AudioPlaySdResmp : public AudioPlayResmp<newdigate::ResamplingSdReader>
{
public:
    AudioPlaySdResmp(SDClass &sd = SD) :
        AudioPlayResmp<newdigate::ResamplingSdReader>()
    {
        reader = new newdigate::ResamplingSdReader(sd);
        begin();
    }
    

    // Play a WAV from a caller-owned, already-open File (persistent per-sample handle) — no per-hit
    // SD.open(). The caller keeps the handle open across triggers; we only seek/read it.
    bool playWavHandle(File& f) { return reader->playWavHandle(f); }

    virtual ~AudioPlaySdResmp() {
        delete reader;
    }
};


#endif //TEENSY_RESAMPLING_SDREADER_PLAYSDRAWRESMP_H
