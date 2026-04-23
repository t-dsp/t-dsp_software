#include "MidiRouter.h"
#include "MidiSink.h"

namespace tdsp {

// MIDI CC assignments that the router interprets specially. Anything
// else is currently dropped silently — if future sinks need generic CC
// access we can add an onControlChange() to MidiSink and pass through.
static constexpr uint8_t kCcModWheel       = 1;
static constexpr uint8_t kCcDataEntryMsb   = 6;
// static constexpr uint8_t kCcDataEntryLsb  = 38;  // reserved — we accept
//                                                  //  MSB-only PB range
static constexpr uint8_t kCcSustainPedal   = 64;
static constexpr uint8_t kCcTimbre         = 74;   // MPE Y axis
static constexpr uint8_t kCcRpnLsb         = 100;
static constexpr uint8_t kCcRpnMsb         = 101;
static constexpr uint8_t kCcAllSoundOff    = 120;
static constexpr uint8_t kCcAllNotesOff    = 123;

// RPN 0, 0 = Pitch Bend Sensitivity (in semitones, MSB) + cents (LSB).
static constexpr uint8_t kRpnPitchBendMsb  = 0;
static constexpr uint8_t kRpnPitchBendLsb  = 0;

MidiRouter::MidiRouter() {
    // ChannelState defaults are set by in-class initializers; nothing
    // else to do here. The array is stack-allocated inside the router
    // and zero/default-initialized on construction.
}

bool MidiRouter::addSink(MidiSink *sink) {
    if (!sink) return false;
    if (_sinkCount >= kMaxSinks) return false;
    // Reject duplicates so a caller that registers the same sink twice
    // (e.g. setup() running twice under some test harness) doesn't
    // cause each event to be dispatched twice.
    for (int i = 0; i < _sinkCount; ++i) {
        if (_sinks[i] == sink) return false;
    }
    _sinks[_sinkCount++] = sink;
    return true;
}

void MidiRouter::removeSink(MidiSink *sink) {
    if (!sink) return;
    for (int i = 0; i < _sinkCount; ++i) {
        if (_sinks[i] == sink) {
            // Shift remaining sinks left to keep the array contiguous.
            for (int j = i + 1; j < _sinkCount; ++j) {
                _sinks[j - 1] = _sinks[j];
            }
            --_sinkCount;
            _sinks[_sinkCount] = nullptr;
            return;
        }
    }
}

void MidiRouter::setPitchBendRange(uint8_t channel, float semitones) {
    const int idx = channelIndex(channel);
    if (idx < 0) return;
    // Clamp to something sensible — negative or absurdly large ranges
    // are either bugs upstream or device misconfiguration. LinnStrument
    // goes up to 96, so cap a bit above that.
    if (semitones < 0.0f)    semitones = 0.0f;
    if (semitones > 127.0f)  semitones = 127.0f;
    _channels[idx].pitchBendRange = semitones;
}

float MidiRouter::pitchBendRange(uint8_t channel) const {
    const int idx = channelIndex(channel);
    if (idx < 0) return kDefaultPitchBendRange;
    return _channels[idx].pitchBendRange;
}

float MidiRouter::currentPitchBend(uint8_t channel) const {
    const int idx = channelIndex(channel);
    return (idx < 0) ? 0.0f : _channels[idx].pitchBend;
}

float MidiRouter::currentTimbre(uint8_t channel) const {
    const int idx = channelIndex(channel);
    return (idx < 0) ? 0.5f : _channels[idx].timbre;
}

float MidiRouter::currentPressure(uint8_t channel) const {
    const int idx = channelIndex(channel);
    return (idx < 0) ? 0.0f : _channels[idx].pressure;
}

// --- Note events ------------------------------------------------------

void MidiRouter::handleNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    const int idx = channelIndex(channel);
    if (idx < 0) return;
    if (note > 127) return;  // sanity — MIDI is 7-bit
    // MIDI running-status idiom: note-on velocity 0 means note-off. Fold
    // here so every sink sees the same event shape and we don't have to
    // remember the rule in each sink.
    if (velocity == 0) {
        for (int i = 0; i < _sinkCount; ++i) {
            _sinks[i]->onNoteOff(channel, note, 0);
        }
        return;
    }
    if (velocity > 127) velocity = 127;
    for (int i = 0; i < _sinkCount; ++i) {
        _sinks[i]->onNoteOn(channel, note, velocity);
    }
}

void MidiRouter::handleNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) {
    const int idx = channelIndex(channel);
    if (idx < 0) return;
    if (note > 127) return;
    if (velocity > 127) velocity = 127;
    for (int i = 0; i < _sinkCount; ++i) {
        _sinks[i]->onNoteOff(channel, note, velocity);
    }
}

// --- Continuous controllers -------------------------------------------

void MidiRouter::handlePitchBend(uint8_t channel, int16_t raw) {
    const int idx = channelIndex(channel);
    if (idx < 0) return;
    // Clamp the raw 14-bit value. USBHost_t36 delivers it in signed
    // -8192..+8191 form. +8191 maps to +range semitones, -8192 to -range.
    if (raw < -8192) raw = -8192;
    if (raw >  8191) raw =  8191;
    const float normalized = (raw >= 0) ? (float)raw /  8191.0f
                                        : (float)raw /  8192.0f;
    const float semitones  = normalized * _channels[idx].pitchBendRange;
    _channels[idx].pitchBend = semitones;
    for (int i = 0; i < _sinkCount; ++i) {
        _sinks[i]->onPitchBend(channel, semitones);
    }
}

void MidiRouter::handleChannelPressure(uint8_t channel, uint8_t pressure) {
    const int idx = channelIndex(channel);
    if (idx < 0) return;
    if (pressure > 127) pressure = 127;
    const float v = (float)pressure * (1.0f / 127.0f);
    _channels[idx].pressure = v;
    for (int i = 0; i < _sinkCount; ++i) {
        _sinks[i]->onPressure(channel, v);
    }
}

void MidiRouter::handleProgramChange(uint8_t channel, uint8_t program) {
    const int idx = channelIndex(channel);
    if (idx < 0) return;
    if (program > 127) program = 127;
    for (int i = 0; i < _sinkCount; ++i) {
        _sinks[i]->onProgramChange(channel, program);
    }
}

// --- Control Change ---------------------------------------------------
//
// CC handling splits along a few axes: MPE-important CCs (timbre,
// sustain, mod wheel), RPN state machine (CC#100/101 select, CC#6
// writes), and the panic CCs (#120, #123). Everything else is dropped.

void MidiRouter::handleControlChange(uint8_t channel, uint8_t cc, uint8_t value) {
    const int idx = channelIndex(channel);
    if (idx < 0) return;
    if (value > 127) value = 127;

    switch (cc) {
    case kCcModWheel: {
        const float v = (float)value * (1.0f / 127.0f);
        for (int i = 0; i < _sinkCount; ++i) {
            _sinks[i]->onModWheel(channel, v);
        }
        return;
    }
    case kCcSustainPedal: {
        // Standard MIDI: 0..63 = off, 64..127 = on. A few controllers
        // send continuous values for half-pedaling; we flatten to a
        // boolean here and leave half-pedal as a follow-on if needed.
        const bool on = (value >= 64);
        for (int i = 0; i < _sinkCount; ++i) {
            _sinks[i]->onSustain(channel, on);
        }
        return;
    }
    case kCcTimbre: {
        const float v = (float)value * (1.0f / 127.0f);
        _channels[idx].timbre = v;
        for (int i = 0; i < _sinkCount; ++i) {
            _sinks[i]->onTimbre(channel, v);
        }
        return;
    }
    case kCcRpnMsb:
        _channels[idx].rpnMsb = value;
        return;
    case kCcRpnLsb:
        _channels[idx].rpnLsb = value;
        return;
    case kCcDataEntryMsb:
        applyDataEntry(idx, value);
        return;
    case kCcAllSoundOff:
    case kCcAllNotesOff:
        // Both trigger the same onAllNotesOff path — a synth receiving
        // All-Sound-Off vs All-Notes-Off differs in envelope release
        // semantics, but for Phase 2b we don't distinguish. Sinks can
        // implement their own release handling.
        for (int i = 0; i < _sinkCount; ++i) {
            _sinks[i]->onAllNotesOff(channel);
        }
        return;
    default:
        // Unhandled CC. Drop silently.
        return;
    }
}

void MidiRouter::applyDataEntry(int chIdx, uint8_t value) {
    // Only honor Data Entry when an RPN is actively selected. A null
    // RPN (0x7F,0x7F) or an RPN we don't handle is dropped.
    auto &cs = _channels[chIdx];
    if (cs.rpnMsb == kRpnPitchBendMsb && cs.rpnLsb == kRpnPitchBendLsb) {
        // Pitch bend sensitivity: MSB = semitones, LSB (CC#38) = cents.
        // We honor MSB only; cents are rarely set by real controllers
        // and the quarter-semi resolution this would buy is inaudible
        // in the use cases we care about. If a controller needs
        // fractional-semi bend ranges (LinnStrument supports it) we'll
        // revisit.
        if (value > 127) value = 127;
        cs.pitchBendRange = (float)value;
    }
    // Unknown RPN: ignore. Do NOT clear the RPN selection here — a
    // standards-compliant controller sends a null-RPN (0x7F,0x7F)
    // explicitly after the Data Entry to "close" the selection, and
    // we respect that via handleControlChange(CC#101/100, 0x7F).
}

void MidiRouter::handleSysEx(const uint8_t *data, size_t length, bool last) {
    if (!data || length == 0) return;
    for (int i = 0; i < _sinkCount; ++i) {
        _sinks[i]->onSysEx(data, length, last);
    }
}

// --- System Real-Time -------------------------------------------------
//
// No channel, no payload. Fan out as-is; sinks that don't care return
// via the MidiSink default no-op overrides.

void MidiRouter::handleClock() {
    for (int i = 0; i < _sinkCount; ++i) _sinks[i]->onClock();
}

void MidiRouter::handleStart() {
    for (int i = 0; i < _sinkCount; ++i) _sinks[i]->onStart();
}

void MidiRouter::handleContinue() {
    for (int i = 0; i < _sinkCount; ++i) _sinks[i]->onContinue();
}

void MidiRouter::handleStop() {
    for (int i = 0; i < _sinkCount; ++i) _sinks[i]->onStop();
}

}  // namespace tdsp
