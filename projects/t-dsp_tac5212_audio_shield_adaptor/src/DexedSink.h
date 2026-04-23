// DexedSink — MidiSink adapter for synth_dexed's AudioSynthDexed engine.
//
// Filters incoming MIDI to a single configured channel (or omni when
// _listenChannel == 0) and forwards the event to the Dexed engine via
// its keydown / keyup / pitch bend / mod wheel / sustain API.
//
// Why a wrapper rather than letting Dexed receive MIDI directly
// -------------------------------------------------------------
// Dexed has its own MIDI handling path inside the library, but threading
// it into our USB host + OSC bridge setup would duplicate routing logic
// we already have in MidiRouter. Wrapping Dexed as a sink means:
//   * One MIDI source (USB host, USB device, OSC) drives many engines
//     (Dexed, future MPE VA, etc.) uniformly.
//   * Per-engine channel filtering lives next to the engine that cares,
//     not buried in the router.
//   * Testing a second engine later doesn't change Dexed's code at all.
//
// MPE note on the Dexed side
// --------------------------
// Dexed is NOT MPE-aware. It shares global pitch bend / mod wheel across
// every playing voice. If you set _listenChannel to 0 (omni) and play
// from a LinnStrument in MPE mode, you'll hear notes but the expressive
// controls will fight each other (channel-N pitch bend affects every
// sounding voice, not just the one note on channel N). That's an
// architectural limitation of Dexed, not of our wrapper. For true MPE
// expression, use the TDspMPE VA engine (Phase 2d).

#pragma once

#include <stdint.h>
#include <synth_dexed.h>
#include <MidiSink.h>

class DexedSink : public tdsp::MidiSink {
public:
    // Construct with a pointer to the engine. The engine itself lives at
    // file scope in main.cpp (Teensy Audio objects must be statically
    // allocated so their constructors register them in the audio graph).
    explicit DexedSink(AudioSynthDexed *dexed) : _dexed(dexed) {}

    // 0 = omni (accept all channels). 1..16 = single-channel MIDI.
    // Out-of-range values are clamped to 0 (omni).
    void setListenChannel(uint8_t channel) {
        _listenChannel = (channel <= 16) ? channel : 0;
    }
    uint8_t listenChannel() const { return _listenChannel; }

    // --- MidiSink overrides ---------------------------------------------

    void onNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) override {
        if (!listens(channel)) return;
        _dexed->keydown(note, velocity);
    }

    void onNoteOff(uint8_t channel, uint8_t note, uint8_t /*velocity*/) override {
        if (!listens(channel)) return;
        _dexed->keyup(note);
    }

    void onPitchBend(uint8_t channel, float semitones) override {
        if (!listens(channel)) return;
        // synth_dexed's pitch bend is expressed as a 14-bit MIDI value
        // (int16_t -8192..+8191) times an integer sensitivity in
        // semitones. The router has already pre-scaled `semitones` by
        // the per-channel pitch bend range, so feeding it in with
        // Dexed's default sensitivity would double-scale. We override
        // Dexed's sensitivity to 1 semi and send the raw semitone value
        // remapped into ±8191 counts — the cleanest way to make
        // external range control work without patching Dexed.
        const int16_t counts = clampCounts((int)(semitones * 8192.0f));
        _dexed->setPitchbendRange(1);
        _dexed->setPitchbend(counts);
    }

    void onModWheel(uint8_t channel, float value) override {
        if (!listens(channel)) return;
        const uint8_t v = toMidi7(value);
        _dexed->setModWheel(v);
    }

    void onPressure(uint8_t channel, float value) override {
        if (!listens(channel)) return;
        // Dexed calls channel pressure "aftertouch" — the synth_dexed
        // voice architecture feeds it through the same modulation
        // matrix the MOD wheel uses (per-voice target controlled by
        // setAftertouchTarget). For Phase 2c we just forward the raw
        // 0..127 value; the engine's DX7-matching default targeting
        // drives amp/pitch of whatever operators the current voice
        // assigns to aftertouch.
        const uint8_t v = toMidi7(value);
        _dexed->setAftertouch(v);
    }

    void onSustain(uint8_t channel, bool on) override {
        if (!listens(channel)) return;
        _dexed->setSustain(on);
    }

    void onAllNotesOff(uint8_t channel) override {
        // channel == 0 is the "panic" convention (release everything
        // regardless of channel filter); otherwise honor the filter.
        if (channel != 0 && !listens(channel)) return;
        _dexed->panic();
    }

private:
    AudioSynthDexed *_dexed;
    uint8_t          _listenChannel = 0;  // default: omni (all channels)

    bool listens(uint8_t channel) const {
        return _listenChannel == 0 || _listenChannel == channel;
    }

    static uint8_t toMidi7(float v) {
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        return (uint8_t)(v * 127.0f + 0.5f);
    }

    static int16_t clampCounts(int counts) {
        if (counts < -8192) return -8192;
        if (counts >  8191) return  8191;
        return (int16_t)counts;
    }
};
