//
// Created by Nicholas Newdigate on 10/02/2019.
//

#ifndef TEENSYAUDIOLIBRARY_RESAMPLINGSDREADER_H
#define TEENSYAUDIOLIBRARY_RESAMPLINGSDREADER_H

#include "SD.h"
#include <cstdint>
#include "spi_interrupt.h"
#include "loop_type.h"
#include "interpolation.h"
#include "IndexableSDFile.h"
#include "ResamplingReader.h"

// Settings for SD card buffering. Each streaming voice's ring = SAMPLE_SIZE*COUNT int16 from the
// heap while sounding. On a RAM-tight no-PSRAM board (e.g. mix-kit TDSP_DRUM_SD, ~144 KB free OCRAM)
// the default 7-deep ring (~28 KB/voice) can't fit enough simultaneous voices, so polyphonic hits
// fail to allocate and drop. Override COUNT with -D TDSP_SD_RESAMPLE_BUFFER_COUNT=<n> (e.g. 4 ->
// ~16 KB/voice, 8 voices ~128 KB) to fit them all. Default matches upstream.
#undef RESAMPLE_BUFFER_SAMPLE_SIZE
#undef RESAMPLE_BUFFER_COUNT
#define RESAMPLE_BUFFER_SAMPLE_SIZE 2048
#ifdef TDSP_SD_RESAMPLE_BUFFER_COUNT
#define RESAMPLE_BUFFER_COUNT 		  TDSP_SD_RESAMPLE_BUFFER_COUNT
#else
#define RESAMPLE_BUFFER_COUNT 		  7
#endif

namespace newdigate {

class ResamplingSdReader : public ResamplingReader< IndexableSDFile<RESAMPLE_BUFFER_SAMPLE_SIZE, RESAMPLE_BUFFER_COUNT>, File > {
public:
    ResamplingSdReader(SDClass &sd = SD) : 
        ResamplingReader(),
        _sd(sd)
    {
    }
    
    virtual ~ResamplingSdReader() {
    }

    int16_t getSourceBufferValue(long index) override {
        return (_sourceBuffer == nullptr)
					?0
					:(*_sourceBuffer)[index];
    }

    int available(void)
    {
        return _playing;
    }

    File open(char *filename) override {
        return _sd.open(filename);
    }

    void close(void) override
    {
        if (_playing)
            stop();
		
        if (_sourceBuffer != nullptr) {
            _sourceBuffer->close();
            delete _sourceBuffer;
            _sourceBuffer = nullptr;
        }
		
        if (_filename != nullptr) {
            delete [] _filename;
            _filename = nullptr;
        }
    }

    IndexableSDFile<RESAMPLE_BUFFER_SAMPLE_SIZE, RESAMPLE_BUFFER_COUNT>* createSourceBuffer() override {
		File f = open(_filename);
        return new IndexableSDFile<RESAMPLE_BUFFER_SAMPLE_SIZE, RESAMPLE_BUFFER_COUNT>(_filename, _sd, f);
    }

    IndexableSDFile<RESAMPLE_BUFFER_SAMPLE_SIZE, RESAMPLE_BUFFER_COUNT>* createSourceBuffer(File& file) override {
        return new IndexableSDFile<RESAMPLE_BUFFER_SAMPLE_SIZE, RESAMPLE_BUFFER_COUNT>(_filename, _sd, file);
    }

    // Reuse the already-allocated ring buffers for a new file (no per-play new/delete -> no heap
    // fragmentation on rapid drum retriggers). Only the FIRST play on a voice allocates.
    void reopenSourceBuffer(File& file) override {
        if (_sourceBuffer) _sourceBuffer->reopen(file);
        else _sourceBuffer = createSourceBuffer(file);
    }

    // Borrowed persistent handle: reuse the rings and mark the file not-owned (never closed here).
    void reopenBorrowedSourceBuffer(File& file) override {
        if (_sourceBuffer) _sourceBuffer->reopen(file, /*ownFile=*/false);
        else { _sourceBuffer = createSourceBuffer(file); _sourceBuffer->setOwnsFile(false); }
    }

    uint32_t positionMillis(void) {
        if (_file_size == 0) return 0;
        if (!_useDualPlaybackHead) {
            return (uint32_t) (( (double)_bufferPosition1 * lengthMillis() ) / (double)(_file_size/2));
        } else 
        {
            if (_crossfade < 0.5)
                return (uint32_t) (( (double)_bufferPosition1 * lengthMillis() ) / (double)(_file_size/2));
            else
                return (uint32_t) (( (double)_bufferPosition2 * lengthMillis() ) / (double)(_file_size/2));
        }
    }

    uint32_t lengthMillis(void) {
        return ((uint64_t)_file_size * B2M) >> 32;
    }
    
protected:    
     SDClass &_sd;
};

}

#endif //TEENSYAUDIOLIBRARY_RESAMPLINGSDREADER_H
