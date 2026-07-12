// AudioSynthTsf.h — TinySoundFont as a Teensy Audio Library source.
//
//   GM MIDI (via tsf_channel_* in the sink)  ->  tsf voice pool (renders in PSRAM)
//     --tsf_render_float() per audio block-->  stereo int16 AudioStream (0=L, 1=R)
//
// The whole SoundFont is resident (TSF stores samples as float in PSRAM), so the font
// must be compact enough to fit — see the backend / plan. Loading streams off the SD
// card (no whole-file buffer). Rendering happens in the audio ISR (update()); the
// caller must guard tsf_channel_* mutations against it (AudioNoInterrupts) and
// pre-create voices+channels so note-on never reallocates.
#pragma once

#include <Arduino.h>
#include <AudioStream.h>
#include <SD.h>
#include "tsf.h"

// Load a SoundFont from the SD card via a tsf_stream (no stdio, no whole-file buffer).
// Returns a tsf* (PSRAM-resident) or nullptr. Call tsf_close() to free.
static inline int  tsf_sd_read(void *data, void *ptr, unsigned int size) {
    return (int)((File *)data)->read(ptr, size);
}
static inline int  tsf_sd_skip(void *data, unsigned int count) {
    File *f = (File *)data;
    return f->seek(f->position() + count) ? 1 : 0;
}
static inline tsf *tsfLoadFromSD(const char *path) {
    File f = SD.open(path);
    if (!f) return nullptr;
    tsf_stream stream;
    stream.data = &f;
    stream.read = &tsf_sd_read;
    stream.skip = &tsf_sd_skip;
    tsf *t = tsf_load(&stream);   // reads the whole font through the stream, then returns
    f.close();
    return t;
}

class AudioSynthTsf : public AudioStream {
public:
    AudioSynthTsf() : AudioStream(0, nullptr) {}

    // `t` must already be configured with tsf_set_output(TSF_STEREO_UNWEAVED, rate, ...).
    void begin(tsf *t) { m_tsf = t; }
    void setGain(float g) { m_gain = g; }

    void update(void) override {
        tsf *t = m_tsf;
        if (!t) return;
        audio_block_t *bl = allocate(), *br = allocate();
        if (!bl || !br) { if (bl) release(bl); if (br) release(br); return; }

        // UNWEAVED: L samples [0..N-1] then R samples [N..2N-1]. flag_mixing=0 clears first.
        tsf_render_float(t, m_fbuf, AUDIO_BLOCK_SAMPLES, 0);

        const float g = m_gain * 32767.0f;
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
            int l = (int)(m_fbuf[i] * g);
            int r = (int)(m_fbuf[AUDIO_BLOCK_SAMPLES + i] * g);
            bl->data[i] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
            br->data[i] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
        }
        transmit(bl, 0);
        transmit(br, 1);
        release(bl);
        release(br);
    }

private:
    tsf  *m_tsf  = nullptr;
    float m_gain = 1.0f;
    float m_fbuf[2 * AUDIO_BLOCK_SAMPLES];
};
