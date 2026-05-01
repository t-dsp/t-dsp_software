// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.

#include "PlaitsSink.h"

#include <math.h>

#include <Audio.h>  // AudioSynthWaveform, AudioMixer4, AudioFilterStateVariable, AudioEffectEnvelope, WAVEFORM_*

PlaitsSink::PlaitsSink(VoicePorts *voices, int voiceCount)
    : _voices(voices)
{
    if (voiceCount < 0)          voiceCount = 0;
    if (voiceCount > kMaxVoices) voiceCount = kMaxVoices;
    _voiceCount = voiceCount;

    for (int i = 0; i < 16; ++i) _channelTimbre[i] = 0.5f;

    // Seed every voice into a defined silent state. AudioSynthWaveform
    // begin() picks the tone type; amplitude(0) prevents any pre-roll
    // before the first note-on. The envelope's sustain is locked at 0
    // (LPG identity — every note decays past release whether held or
    // not); attack short, decay long, release matches decay so a
    // released voice and a still-held voice tail at the same rate.
    const short t1 = toneTypeForOsc1(_model);
    const short t2 = toneTypeForOsc2(_model);
    for (int i = 0; i < _voiceCount; ++i) {
        if (_voices[i].osc1) {
            _voices[i].osc1->begin(t1);
            _voices[i].osc1->amplitude(0.0f);
            _voices[i].osc1->frequency(440.0f);
        }
        if (_voices[i].osc2) {
            _voices[i].osc2->begin(t2);
            _voices[i].osc2->amplitude(0.0f);
            _voices[i].osc2->frequency(440.0f);
        }
        if (_voices[i].filter) {
            _voices[i].filter->frequency(timbreToCutoffHz(_timbre));
            _voices[i].filter->resonance(_resonance);
        }
        if (_voices[i].env) {
            _voices[i].env->attack (3.0f);  // fast pluck-style
            _voices[i].env->hold   (0.0f);
            _voices[i].env->decay  (decayToMs(_decay));
            _voices[i].env->sustain(0.0f);  // LPG: no sustain stage
            _voices[i].env->release(decayToMs(_decay));
        }
    }
    applyMorphAll();
}

void PlaitsSink::setMasterChannel(uint8_t ch) {
    if (ch > 16) ch = 0;
    _masterChannel = ch;
}

void PlaitsSink::setModel(uint8_t model) {
    if (model >= kModelCount) model = ModelVaSaw;
    _model = model;
    applyWaveformAll();
}

void PlaitsSink::setHarmonics(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    _harmonics = v;
    // Re-apply osc2 frequency on every held voice so the new interval
    // takes effect immediately. Released voices are left alone — their
    // tail keeps its original interval.
    for (int i = 0; i < _voiceCount; ++i) {
        if (_state[i].note_held) applyFrequency(i);
    }
}

void PlaitsSink::setTimbre(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    _timbre = v;
    for (int i = 0; i < _voiceCount; ++i) applyFilter(i);
}

void PlaitsSink::setMorph(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    _morph = v;
    applyMorphAll();
}

void PlaitsSink::setDecay(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    _decay = v;
    applyEnvelopeAll();
}

void PlaitsSink::setResonance(float q) {
    if (q < 0.707f) q = 0.707f;
    if (q > 5.0f)   q = 5.0f;
    _resonance = q;
    for (int i = 0; i < _voiceCount; ++i) {
        if (_voices[i].filter) _voices[i].filter->resonance(q);
    }
}

void PlaitsSink::setVoiceVolumeScale(float s) {
    if (s < 0.0f) s = 0.0f;
    if (s > 1.0f) s = 1.0f;
    _volumeScale = s;
}

// ---------------------------------------------------------------------
// MidiSink overrides
// ---------------------------------------------------------------------

void PlaitsSink::onNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    // MPE master-channel notes are ignored (only global CCs honored).
    // _masterChannel == 0 disables this rule, so a plain non-MPE
    // controller plays the engine on any channel — matches the on-
    // screen keyboard which sends ch 1.
    if (_masterChannel != 0 && channel == _masterChannel) return;
    if (channel < 1 || channel > 16) return;
    if (_voiceCount <= 0)            return;

    const int vi = pickVoice();
    if (vi < 0) return;

    Voice &v = _state[vi];
    v.channel    = channel;
    v.note       = note;
    v.note_held  = true;
    v.start_time = ++_counter;
    v.bend_semi  = 0.0f;
    v.base_amp   = (velocity <= 0 ? 0.0f : (float)velocity / 127.0f)
                 * _volumeScale;
    v.timbre     = _channelTimbre[channel - 1];
    v.pressure   = 0.0f;

    applyFrequency(vi);
    applyAmplitude(vi);
    applyFilter   (vi);
    if (_voices[vi].env) _voices[vi].env->noteOn();
}

void PlaitsSink::onNoteOff(uint8_t channel, uint8_t note, uint8_t /*velocity*/) {
    const int vi = findActive(channel, note);
    if (vi < 0) return;

    _state[vi].note_held = false;
    if (_voices[vi].env) _voices[vi].env->noteOff();
    // Don't refresh start_time — released voices should be the first
    // steal candidates, but only after held voices.
}

void PlaitsSink::onPitchBend(uint8_t channel, float semitones) {
    for (int i = 0; i < _voiceCount; ++i) {
        Voice &v = _state[i];
        if (!v.note_held || v.channel != channel) continue;
        v.bend_semi = semitones;
        applyFrequency(i);
    }
}

void PlaitsSink::onPressure(uint8_t channel, float value) {
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    for (int i = 0; i < _voiceCount; ++i) {
        Voice &v = _state[i];
        if (!v.note_held || v.channel != channel) continue;
        v.pressure = value;
        applyAmplitude(i);
    }
}

void PlaitsSink::onTimbre(uint8_t channel, float value) {
    if (channel < 1 || channel > 16) return;
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    _channelTimbre[channel - 1] = value;
    for (int i = 0; i < _voiceCount; ++i) {
        Voice &v = _state[i];
        if (!v.note_held || v.channel != channel) continue;
        v.timbre = value;
        applyFilter(i);
    }
}

void PlaitsSink::onAllNotesOff(uint8_t channel) {
    for (int i = 0; i < _voiceCount; ++i) {
        Voice &v = _state[i];
        if (channel != 0 && v.channel != channel) continue;
        if (_voices[i].env) _voices[i].env->noteOff();
        v.note_held = false;
    }
}

// ---------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------

int PlaitsSink::pickVoice() {
    if (_voiceCount <= 0) return -1;

    int      bestIdle     = -1;
    uint32_t bestIdleTime = 0;
    for (int i = 0; i < _voiceCount; ++i) {
        if (_state[i].note_held) continue;
        if (bestIdle < 0 || _state[i].start_time < bestIdleTime) {
            bestIdle     = i;
            bestIdleTime = _state[i].start_time;
        }
    }
    if (bestIdle >= 0) return bestIdle;

    int      bestHeld     = 0;
    uint32_t bestHeldTime = _state[0].start_time;
    for (int i = 1; i < _voiceCount; ++i) {
        if (_state[i].start_time < bestHeldTime) {
            bestHeld     = i;
            bestHeldTime = _state[i].start_time;
        }
    }
    return bestHeld;
}

int PlaitsSink::findActive(uint8_t ch, uint8_t note) {
    for (int i = 0; i < _voiceCount; ++i) {
        const Voice &v = _state[i];
        if (v.note_held && v.channel == ch && v.note == note) return i;
    }
    return -1;
}

void PlaitsSink::applyFrequency(int vi) {
    const Voice &v = _state[vi];
    const float baseHz   = noteToHz(v.note) * powf(2.0f, v.bend_semi / 12.0f);
    const float intervalSemi = harmonicsToSemitones(_harmonics);
    const float osc2Hz   = baseHz * powf(2.0f, intervalSemi / 12.0f);
    if (_voices[vi].osc1) _voices[vi].osc1->frequency(baseHz);
    if (_voices[vi].osc2) _voices[vi].osc2->frequency(osc2Hz);
}

void PlaitsSink::applyAmplitude(int vi) {
    // Pressure: 0..1 maps to 0.5..1.0 amp factor (so a un-pressed key
    // is half-amplitude rather than silent — matches MPE behaviour and
    // gives a usable note before pressure is applied).
    const Voice &v = _state[vi];
    const float amp = v.base_amp * (0.5f + 0.5f * v.pressure);
    if (_voices[vi].osc1) _voices[vi].osc1->amplitude(amp);
    if (_voices[vi].osc2) _voices[vi].osc2->amplitude(amp);
}

void PlaitsSink::applyFilter(int vi) {
    if (!_voices[vi].filter) return;
    // Cutoff = baseCutoff(timbre) × 2^(perVoiceTimbre × 2 - 1). Per-voice
    // CC#74 ranges ±1 octave around the base — 0.5 is neutral.
    const float baseHz = timbreToCutoffHz(_timbre);
    const float t      = _state[vi].timbre;
    float hz = baseHz * powf(2.0f, t * 2.0f - 1.0f);
    if (hz < 20.0f)    hz = 20.0f;
    if (hz > 20000.0f) hz = 20000.0f;
    _voices[vi].filter->frequency(hz);
}

void PlaitsSink::applyMorphAll() {
    // Equal-power crossfade: gain1 = cos(morph * π/2), gain2 = sin(morph
    // * π/2). At morph=0.5 each input is √0.5 (~0.707), so the summed
    // power stays constant. Linear crossfade has a -3 dB dip at the
    // middle which the user perceives as "the synth got quiet" mid-
    // sweep.
    const float pi_2  = 1.5707963267948966f;
    const float gain1 = cosf(_morph * pi_2);
    const float gain2 = sinf(_morph * pi_2);
    for (int i = 0; i < _voiceCount; ++i) {
        if (!_voices[i].voiceMix) continue;
        _voices[i].voiceMix->gain(0, gain1);
        _voices[i].voiceMix->gain(1, gain2);
        // Inputs 2/3 are unused — leave at default 1.0 (silent because
        // nothing's connected, but be defensive against future wiring).
        _voices[i].voiceMix->gain(2, 0.0f);
        _voices[i].voiceMix->gain(3, 0.0f);
    }
}

void PlaitsSink::applyWaveformAll() {
    const short t1 = toneTypeForOsc1(_model);
    const short t2 = toneTypeForOsc2(_model);
    for (int i = 0; i < _voiceCount; ++i) {
        if (_voices[i].osc1) _voices[i].osc1->begin(t1);
        if (_voices[i].osc2) _voices[i].osc2->begin(t2);
    }
}

void PlaitsSink::applyEnvelopeAll() {
    const float ms = decayToMs(_decay);
    for (int i = 0; i < _voiceCount; ++i) {
        if (!_voices[i].env) continue;
        _voices[i].env->decay(ms);
        // Release matches decay so a still-held LPG voice and a released
        // one tail at the same rate — that's the LPG identity (the gate
        // closes the same way regardless of finger state).
        _voices[i].env->release(ms);
    }
}

float PlaitsSink::harmonicsToSemitones(float h) {
    // Snap to musical intervals. Five anchors at 0, 0.25, 0.5, 0.75, 1.0;
    // pick the nearest. Linear detune was rejected — every snap point
    // is musically usable, mid-snap values were dissonant.
    static const float kAnchors[5] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
    static const float kSemis[5]   = { 0.0f, 7.0f,  12.0f, 19.0f, 24.0f };
    int best = 0;
    float bestDist = 1e9f;
    for (int i = 0; i < 5; ++i) {
        const float d = fabsf(h - kAnchors[i]);
        if (d < bestDist) { bestDist = d; best = i; }
    }
    return kSemis[best];
}

float PlaitsSink::decayToMs(float d) {
    // Exponential 50 ms .. 3000 ms. log2(3000/50) = ~5.9 octaves; expand
    // 0..1 across that range so small movements at the low end give
    // fine pluck control.
    const float lo = 50.0f;
    const float hi = 3000.0f;
    return lo * powf(hi / lo, d);
}

float PlaitsSink::timbreToCutoffHz(float t) {
    const float lo = 200.0f;
    const float hi = 16000.0f;
    return lo * powf(hi / lo, t);
}

float PlaitsSink::noteToHz(uint8_t note) {
    return 440.0f * powf(2.0f, ((float)note - 69.0f) / 12.0f);
}

short PlaitsSink::toneTypeForOsc1(uint8_t model) {
    switch (model) {
        case ModelVaSaw:      return WAVEFORM_SAWTOOTH;
        case ModelVaSquare:   return WAVEFORM_SQUARE;
        case ModelFmSine:     return WAVEFORM_SINE;
        case ModelHollowTri:  return WAVEFORM_TRIANGLE;
        case ModelMixedPulse: return WAVEFORM_SAWTOOTH;
        default:              return WAVEFORM_SAWTOOTH;
    }
}

short PlaitsSink::toneTypeForOsc2(uint8_t model) {
    switch (model) {
        case ModelVaSaw:      return WAVEFORM_SAWTOOTH;
        case ModelVaSquare:   return WAVEFORM_SQUARE;
        case ModelFmSine:     return WAVEFORM_SINE;
        case ModelHollowTri:  return WAVEFORM_TRIANGLE;
        case ModelMixedPulse: return WAVEFORM_SQUARE;
        default:              return WAVEFORM_SAWTOOTH;
    }
}
