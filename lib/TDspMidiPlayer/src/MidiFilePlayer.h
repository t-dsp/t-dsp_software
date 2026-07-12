// MidiFilePlayer.h — non-blocking song sequencer, synth-agnostic.
//
// Plays a MidiFileEvent[] stream (from MidiSmfParser or a baked array) by
// fanning events into a tdsp::MidiSink. Because it only ever talks to the
// MidiSink interface, the SAME player drives Dexed today and ymfm (or any
// future engine) tomorrow — the synth choice lives entirely on the sink side
// (see the TDSP_SYNTH build flag in spike_midi_player).
//
// Non-blocking: tick() is called every loop() and advances the song by the
// real elapsed milliseconds, so other work (USB, control, live MIDI) keeps
// running and the song can be stopped/switched mid-play. Nothing here touches
// the audio ISR.
//
// Channel policy lives here, not in the parser: setChannelMask() decides which
// of the 16 MIDI channels are emitted. The default masks channel 10 (index 9,
// drums) so a single melodic engine (e.g. one Dexed patch) does not try to
// play kick/snare notes. A multi-timbral / drum-capable backend should call
// setChannelMask(0xFFFF) to hear everything.
#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <MidiSink.h>
#include "MidiFileEvent.h"

namespace tdsp {

class MidiFilePlayer {
public:
    // Default channel mask: all channels except index 9 (MIDI channel 10 =
    // drums). Bit i set => channel i is emitted.
    static constexpr uint16_t kMaskNoDrums = (uint16_t)0xFFFF & ~(uint16_t)(1u << 9);
    static constexpr uint16_t kMaskAll     = 0xFFFF;

    void setSink(MidiSink *sink) { sink_ = sink; }

    // Which MIDI channels to emit (bitmask; bit i => channel index i). Applies
    // to note/CC/bend/program events. Changing it mid-song does not retro-
    // actively silence already-sounding notes.
    void setChannelMask(uint16_t mask) { chMask_ = mask; }
    uint16_t channelMask() const { return chMask_; }

    // Forward program-change events to the sink (default true). Turn off for a
    // fixed-timbre backend that should ignore the file's instrument choices.
    void setProgramChangeEnabled(bool en) { pcEnabled_ = en; }

    // Default pitch-bend range in semitones, used to convert the file's 14-bit bend
    // into the MidiSink's float-semitone convention (default 2). A song can override
    // it PER CHANNEL via RPN 0,0 (CC 101=0, 100=0, 6=semitones) — many do (e.g. ±12);
    // ignoring that renders bends at a fraction of their intended depth.
    void setPitchBendRange(float semis) {
        pbDefault_ = semis;
        for (int c = 0; c < 16; c++) pbRange_[c] = semis;
    }

    // Start playing ev[0..count). `ev` must outlive playback (not copied).
    void play(const MidiFileEvent *ev, uint32_t count) {
        stop();                       // release anything currently held
        if (!ev || count == 0) return;
        ev_ = ev; count_ = count; idx_ = 0;
        wait_ = ev_[0].deltaMs; clock_ = 0;
        for (int c = 0; c < 16; c++) {          // reset per-channel bend range + RPN state
            pbRange_[c] = pbDefault_; rpnMsb_[c] = rpnLsb_[c] = 0x7F;
        }
        playing_ = true;
    }

    // Stop and release every held note (sink panic). Safe to call when idle.
    void stop() {
        bool was = playing_;
        playing_ = false;
        ev_ = nullptr; count_ = 0; idx_ = 0;
        if (was && sink_) sink_->onAllNotesOff(0);   // 0 = panic all channels
    }

    bool isPlaying() const { return playing_; }

    // Progress 0..count (event index reached). Handy for a UI/heartbeat.
    uint32_t eventIndex() const { return idx_; }
    uint32_t eventCount() const { return count_; }

    // Advance the song by elapsed real time. Call every loop().
    void tick() {
        if (!playing_ || !sink_) return;
        while (playing_ && clock_ >= wait_) {
            clock_ -= wait_;
            dispatch(ev_[idx_]);
            if (++idx_ >= count_) {                   // reached the end
                playing_ = false;
                sink_->onAllNotesOff(0);
                ev_ = nullptr;
                return;
            }
            wait_ = ev_[idx_].deltaMs;
        }
    }

private:
    void dispatch(const MidiFileEvent &e) {
        if (e.kind == kRest) return;
        if (!(chMask_ & (uint16_t)(1u << e.channel))) return;   // channel filtered
        const uint8_t ch = (uint8_t)(e.channel + 1);            // MidiSink is 1-based
        switch (e.kind) {
            case kNoteOn:        sink_->onNoteOn(ch, e.data1, e.data2); break;
            case kNoteOff:       sink_->onNoteOff(ch, e.data1, e.data2); break;
            case kProgramChange: if (pcEnabled_) sink_->onProgramChange(ch, e.data1); break;
            case kControlChange: dispatchCC(ch, e.data1, e.data2); break;
            case kPitchBend: {
                const int value = ((int)e.data2 << 7) | (int)e.data1;   // 0..16383, center 8192
                sink_->onPitchBend(ch, ((float)(value - 8192) / 8192.0f) * pbRange_[e.channel]);
                break;
            }
            default: break;
        }
    }

    // Translate the handful of controllers the MidiSink models. Anything else
    // (expression, pan, ...) is dropped for now — the sink has no generic CC
    // path, and these cover the musically important cases.
    void dispatchCC(uint8_t ch, uint8_t cc, uint8_t val) {
        const uint8_t ci = (uint8_t)(ch - 1);   // 0-based channel for RPN/range state
        switch (cc) {
            case 1:   sink_->onModWheel(ch, val / 127.0f); break;   // mod wheel
            case 74:  sink_->onTimbre  (ch, val / 127.0f); break;   // MPE timbre
            case 64:  sink_->onSustain (ch, val >= 64);    break;   // sustain pedal
            // RPN: select register (101=MSB, 100=LSB); data entry 6 sets its value.
            // RPN 0,0 = pitch-bend range in semitones (a song can raise it above ±2).
            case 101: rpnMsb_[ci] = val; break;
            case 100: rpnLsb_[ci] = val; break;
            case 6:   if (rpnMsb_[ci] == 0 && rpnLsb_[ci] == 0) pbRange_[ci] = (float)val; break;
            case 120: // all sound off
            case 123: sink_->onAllNotesOff(ch); break;              // all notes off
            default:  break;
        }
    }

    MidiSink            *sink_      = nullptr;
    const MidiFileEvent *ev_        = nullptr;
    uint32_t             count_     = 0;
    uint32_t             idx_       = 0;
    uint32_t             wait_      = 0;
    elapsedMillis        clock_;
    bool                 playing_   = false;
    bool                 pcEnabled_ = true;
    float                pbDefault_ = 2.0f;              // fallback bend range (no RPN in file)
    float                pbRange_[16] = {2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2};  // per-channel, RPN-settable
    uint8_t              rpnMsb_[16] = {0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F};
    uint8_t              rpnLsb_[16] = {0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F};
    uint16_t             chMask_    = kMaskNoDrums;
};

// Expand a legacy {deltaMs,note,velocity} note stream (the old SongEv format)
// into MidiFileEvent[] on a single channel, so baked songs generated for the
// old player still play through the new one. Returns the event count written.
// `Legacy` must have .dms / .note / .vel fields (SongEv is layout-compatible).
template <typename Legacy>
static uint32_t expandLegacyNotes(const Legacy *in, uint32_t n,
                                  MidiFileEvent *out, uint32_t maxOut,
                                  uint8_t channel = 0) {
    uint32_t no = 0;
    for (uint32_t i = 0; i < n && no < maxOut; ++i) {
        uint8_t kind = in[i].vel ? kNoteOn : (in[i].note ? kNoteOff : kRest);
        out[no++] = { in[i].dms, kind, channel, in[i].note, in[i].vel };
    }
    return no;
}

} // namespace tdsp
