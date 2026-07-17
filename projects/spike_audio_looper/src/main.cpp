// spike_audio_looper — bench + compile test for tdsp::AudioLooper.
//
// Wires the anti-feedback topology from planning/audio-looper/DESIGN.md and drives
// the looper against a free-running tdsp::Clock. A 440 Hz sine stands in for the
// device's synth/master bus. Serial keys: r=record o=overdub s=stop c=clear.
#include <Arduino.h>
#include <AudioStream_F32.h>
#include <AudioSettings_F32.h>
#include <synth_sine_f32.h>
#include <AudioMixer_F32.h>
#include <output_i2s_f32.h>
#include <SD.h>
#include <Clock.h>
#include <TDspAudioLoop.h>
#include <AudioLoopWav.h>   // optional SD .wav save (pulls <SD.h>)

static constexpr uint32_t kFrames = 48000;   // 1.0 s stereo @ 48k = 192 KB int16

AudioSettings_F32          g_audio(AUDIO_SAMPLE_RATE_EXACT, AUDIO_BLOCK_SAMPLES);
AudioSynthWaveformSine_F32 src;               // stand-in for the synth/master bus
tdsp::AudioLooper          looper;
AudioMixer4_F32            finalL, finalR;     // record bus (slot 0) + loop return (slot 1)
AudioOutputI2S_F32         i2sOut;

// record bus -> looper inputs (NOT the post-loop mix -> no overdub feedback)
AudioConnection_F32 c_inL (src,    0, looper, 0);
AudioConnection_F32 c_inR (src,    0, looper, 1);
// record bus -> final mix slot 0
AudioConnection_F32 c_busL(src,    0, finalL, 0);
AudioConnection_F32 c_busR(src,    0, finalR, 0);
// loop return -> final mix slot 1
AudioConnection_F32 c_retL(looper, 0, finalL, 1);
AudioConnection_F32 c_retR(looper, 1, finalR, 1);
// final mix -> I2S DAC
AudioConnection_F32 c_outL(finalL, 0, i2sOut, 0);
AudioConnection_F32 c_outR(finalR, 0, i2sOut, 1);

DMAMEM int16_t g_loopBuf[2 * kFrames];        // caller-owned stereo-interleaved buffer
tdsp::Clock    g_clock;

void setup() {
    Serial.begin(115200);
    AudioMemory_F32(30, g_audio);

    src.frequency(440.0f);
    src.amplitude(0.3f);

    finalL.gain(0, 1.0f); finalL.gain(1, 1.0f);   // record bus + loop return, unity
    finalR.gain(0, 1.0f); finalR.gain(1, 1.0f);

    // Free-running master clock at 120 BPM, 4/4 — the looper reads bpm/beatsPerBar.
    g_clock.setSource(tdsp::Clock::Internal);
    g_clock.setBeatsPerBar(4);
    g_clock.setInternalBpm(120.0f);
    g_clock.onMidiStart();                          // running -> positionBeats advances

    looper.setMono(false);                          // stereo (setMono(true) before begin() = mono)
    looper.begin(&g_clock, g_loopBuf, kFrames);
    looper.setBars(2);                              // 2-bar loop
    looper.setReturnLevel(0.9f);
    looper.setClockFollow(true);                    // track master tempo

    SD.begin(BUILTIN_SDCARD);                        // best-effort (for 'w' save)
    Serial.println("[audio-looper spike] r=record o=overdub s=stop c=clear w=save.wav");
}

void loop() {
    g_clock.update(micros());   // advance the internal clock (emits 24-PPQN ticks)
    looper.poll();              // start recording on the next bar downbeat when armed

    while (Serial.available()) {
        switch (Serial.read()) {
            case 'r': looper.armRecord();  Serial.println("[armed record]");  break;
            case 'o': looper.armOverdub(); Serial.println("[overdub]");       break;
            case 's': looper.stop();       Serial.println("[stop]");          break;
            case 'c': looper.clear();      Serial.println("[clear]");         break;
            case 'w': Serial.printf("[save] /loops/take.wav -> %d\n",
                                    tdsp::saveWavFile("/loops/take.wav", looper) ? 1 : 0); break;
            default: break;
        }
    }

    static elapsedMillis hb;
    if (hb >= 1000) {
        hb = 0;
        Serial.printf("state=%d pos=%u/1000 loop=%.2fs cap=%.2fs beat=%.2f cpu=%.1f%%\n",
                      (int)looper.state(), looper.positionPermille(),
                      looper.loopSeconds(), looper.capSeconds(),
                      g_clock.positionBeats(), AudioProcessorUsageMax());
    }
}
