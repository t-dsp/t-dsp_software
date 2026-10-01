// AudioRecorderF32.h — master-output recorder + playback sum, as one F32 AudioStream node.
//
// Sits INLINE between the post mixer and the DAC, so what it captures is exactly what reaches
// the outputs (every synth, drums, reverb return, metronome, external inputs), pre master fader:
//
//   in 0/1  : the master bus (L/R)         -> recorded, and passed to out 0/1
//   in 2/3  : a WAV player's L/R           -> summed into out 0/1, NOT recorded (so playing back a
//                                             recording can't re-record itself)
//   out 0/1 : to the DAC
//
// Recording writes 16-bit stereo PCM WAV files. update() (audio ISR) converts each block to int16
// and drops it into a lock-free ring (PSRAM when present: 1 MB = ~5.5 s of headroom; else a small
// heap ring); service() (loop) drains the ring to the SD card in 16 KB writes and patches the RIFF
// sizes when the take ends. SD latency therefore never touches the audio thread; if the card falls
// far behind, blocks are counted as dropped (dropped()) rather than blocking anything.
#pragma once
#include <Arduino.h>
#include <AudioStream_F32.h>
#include <FS.h>

extern "C" {
void* extmem_malloc(size_t);
void  extmem_free(void*);
extern uint8_t external_psram_size;   // C linkage, as the Teensy core and MidiSmfFile.h declare it
}

namespace tdsp {

class AudioRecorderF32 : public AudioStream_F32 {
public:
    AudioRecorderF32() : AudioStream_F32(4, inputQueueArray) {}

    // Allocate the ring and make sure `dir` exists. sampleRate goes into the WAV header.
    bool begin(FS& fs, const char* dir, uint32_t sampleRate) {
        fs_ = &fs; rate_ = sampleRate;
        strncpy(dir_, dir, sizeof(dir_) - 1); dir_[sizeof(dir_) - 1] = 0;
        if (!fs.exists(dir_)) fs.mkdir(dir_);
        if (!ring_) {
            uint32_t want = 1u << 19;                       // samples (int16): 1 MB
            if (external_psram_size > 0) ring_ = (int16_t*)extmem_malloc(want * 2);
            if (ring_) psram_ = true;
            else { want = 16384; ring_ = (int16_t*)malloc(want * 2); psram_ = false; }   // 32 KB heap fallback (~170 ms)
            cap_ = ring_ ? want : 0;
        }
        return ring_ != nullptr;
    }

    bool recording() const { return rec_; }
    bool busy() const { return rec_ || stopping_; }        // a take is open (recording or still flushing)
    bool inPsram() const { return psram_; }
    const char* currentFile() const { return file_; }      // "rec_0007.wav" of the open/last take
    uint32_t dropped() const { return drops_; }
    uint32_t recordedSeconds() const { return rate_ ? (uint32_t)(frames_ / rate_) : 0; }
    uint32_t recordedBytes() const { return dataBytes_; }
    // Mute inputs 2/3 (the player) without touching the bus: used while playback is paused, where the
    // resampling reader is frozen (rate 0) and would otherwise hold its last sample as a DC offset.
    void setPlayerMute(bool m) { playerMute_ = m; }
    // Peak |sample| of the player inputs over the last block (0..1), decays ~ per block. The firmware's
    // peakOut meter taps the instrument bus before this node, so playback is invisible to it.
    float playerPeak() const { return playerPeak_; }

    // Start a new take: next free rec_NNNN.wav in dir. Returns false if busy, no ring, or the card
    // refused the file.
    bool startRecording() {
        if (busy() || !ring_ || !fs_) return false;
        int n = nextIndex();
        snprintf(file_, sizeof(file_), "rec_%04d.wav", n);
        char path[72]; snprintf(path, sizeof(path), "%s/%s", dir_, file_);
        if (fs_->exists(path)) fs_->remove(path);
        f_ = fs_->open(path, FILE_WRITE);
        if (!f_) { file_[0] = 0; return false; }
        uint8_t h[44]; buildHeader(h, 0);
        if (f_.write(h, 44) != 44) { f_.close(); file_[0] = 0; return false; }
        dataBytes_ = 0; frames_ = 0; drops_ = 0; err_ = false;
        head_ = tail_ = 0;
        stopping_ = false;
        __asm__ volatile("dmb");
        rec_ = true;
        return true;
    }

    // End the take: stop capturing now; service() drains what is left and closes the file.
    void stopRecording() {
        if (!rec_) return;
        rec_ = false;
        __asm__ volatile("dmb");
        stopping_ = true;
    }

    // Call every loop(). One 16 KB write per pass while recording (keeps loop() responsive);
    // drains everything and finalizes the header once the take has been stopped.
    void service() {
        if (!f_) return;
        for (;;) {
            uint32_t h = head_, t = tail_;
            uint32_t avail = (h + cap_ - t) % cap_;            // samples ready
            if (avail == 0) break;
            if (!stopping_ && avail < kChunkSamples) break;   // wait for a full chunk while live
            uint32_t n = avail < kChunkSamples ? avail : kChunkSamples;
            uint32_t toEnd = cap_ - t;
            if (n > toEnd) n = toEnd;                         // contiguous run only
            size_t w = f_.write((const uint8_t*)(ring_ + t), n * 2);
            if (w != n * 2) err_ = true;
            dataBytes_ += (uint32_t)w;
            tail_ = (t + n) % cap_;
            if (!stopping_) break;                            // one chunk per pass while recording
        }
        if (stopping_) finalize();
    }

    virtual void update(void) override {
        audio_block_f32_t* inL = AudioStream_F32::receiveReadOnly_f32(0);
        audio_block_f32_t* inR = AudioStream_F32::receiveReadOnly_f32(1);
        audio_block_f32_t* pL  = AudioStream_F32::receiveReadOnly_f32(2);
        audio_block_f32_t* pR  = AudioStream_F32::receiveReadOnly_f32(3);
        const int n = inL ? inL->length : inR ? inR->length : pL ? pL->length : pR ? pR->length : AUDIO_BLOCK_SAMPLES;

        if (rec_ && ring_) {
            uint32_t h = head_, t = tail_;
            uint32_t freeS = (t + cap_ - h - 1) % cap_;
            if (freeS >= (uint32_t)(2 * n)) {
                for (int i = 0; i < n; i++) {
                    ring_[h] = toI16(inL ? inL->data[i] : 0.0f); h = (h + 1 == cap_) ? 0 : h + 1;
                    ring_[h] = toI16(inR ? inR->data[i] : 0.0f); h = (h + 1 == cap_) ? 0 : h + 1;
                }
                __asm__ volatile("dmb");
                head_ = h;
                frames_ += n;
            } else {
                drops_++;
            }
        }

        audio_block_f32_t* oL = AudioStream_F32::allocate_f32();
        audio_block_f32_t* oR = AudioStream_F32::allocate_f32();
        const bool mix = !playerMute_;
        float pk = playerPeak_ * 0.8f;
        if (mix && (pL || pR)) {
            for (int i = 0; i < n; i++) {
                float a = pL ? fabsf(pL->data[i]) : 0.0f, b = pR ? fabsf(pR->data[i]) : 0.0f;
                if (a > pk) pk = a; if (b > pk) pk = b;
            }
        }
        playerPeak_ = pk;
        if (oL) {
            for (int i = 0; i < n; i++) oL->data[i] = (inL ? inL->data[i] : 0.0f) + ((mix && pL) ? pL->data[i] : 0.0f);
            oL->length = n; AudioStream_F32::transmit(oL, 0); AudioStream_F32::release(oL);
        }
        if (oR) {
            for (int i = 0; i < n; i++) oR->data[i] = (inR ? inR->data[i] : 0.0f) + ((mix && pR) ? pR->data[i] : 0.0f);
            oR->length = n; AudioStream_F32::transmit(oR, 1); AudioStream_F32::release(oR);
        }
        if (inL) AudioStream_F32::release(inL);
        if (inR) AudioStream_F32::release(inR);
        if (pL)  AudioStream_F32::release(pL);
        if (pR)  AudioStream_F32::release(pR);
    }

private:
    static constexpr uint32_t kChunkSamples = 8192;   // 16 KB per SD write

    static inline int16_t toI16(float v) {
        if (v >= 1.0f) return 32767;
        if (v <= -1.0f) return -32768;
        return (int16_t)(v * 32767.0f);
    }

    void buildHeader(uint8_t* h, uint32_t dataBytes) const {   // (not FLASHMEM: const member + lambdas trips a section-type conflict)
        auto put32 = [&](int o, uint32_t v) { h[o] = v; h[o + 1] = v >> 8; h[o + 2] = v >> 16; h[o + 3] = v >> 24; };
        auto put16 = [&](int o, uint16_t v) { h[o] = v; h[o + 1] = v >> 8; };
        memcpy(h + 0, "RIFF", 4);  put32(4, 36u + dataBytes);
        memcpy(h + 8, "WAVE", 4);
        memcpy(h + 12, "fmt ", 4); put32(16, 16u);
        put16(20, 1);              put16(22, 2);                 // PCM, stereo
        put32(24, rate_);          put32(28, rate_ * 4u);        // byte rate
        put16(32, 4);              put16(34, 16);                // block align, bits
        memcpy(h + 36, "data", 4); put32(40, dataBytes);
    }

    void finalize() {
        uint8_t h[44]; buildHeader(h, dataBytes_);
        if (f_.seek(0)) f_.write(h, 44);
        f_.close();
        stopping_ = false;
    }

    // Highest existing rec_NNNN.wav + 1 (one directory pass; fine for hundreds of takes).
    int nextIndex() {
        int best = 0;
        File d = fs_->open(dir_);
        if (d && d.isDirectory()) {
            for (;;) {
                File e = d.openNextFile();
                if (!e) break;
                const char* nm = e.name(); const char* b = strrchr(nm, '/'); b = b ? b + 1 : nm;
                int v = 0;
                if (sscanf(b, "rec_%d", &v) == 1 && v > best) best = v;
                e.close();
            }
            d.close();
        }
        return best + 1;
    }

    audio_block_f32_t* inputQueueArray[4];
    FS*      fs_ = nullptr;
    File     f_;
    char     dir_[24] = "/recordings";
    char     file_[32] = "";
    uint32_t rate_ = 48000;
    int16_t* ring_ = nullptr;
    uint32_t cap_ = 0;
    volatile uint32_t head_ = 0, tail_ = 0;
    volatile bool rec_ = false, stopping_ = false;
    bool     psram_ = false, err_ = false;
    volatile bool playerMute_ = false;
    volatile float playerPeak_ = 0.0f;
    uint32_t dataBytes_ = 0, drops_ = 0;
    volatile uint32_t frames_ = 0;
};

} // namespace tdsp
