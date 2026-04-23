// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.

#include "AcidSink.h"

#include <math.h>

#include <Audio.h>

AcidSink::AcidSink(const VoicePorts &ports)
    : _ports(ports)
{
    if (_ports.osc) {
        _ports.osc->begin(toneTypeFor(_waveform));
        _ports.osc->amplitude(0.0f);
        _ports.osc->frequency(220.0f);
        // Square wave defaults to 50% duty; the waveform pulseWidth
        // API is only for the PULSE type. Saw/Square are fixed-shape
        // on AudioSynthWaveform.
    }
    if (_ports.filter) {
        _ports.filter->frequency(_cutoffHz);
        _ports.filter->resonance(_resonance);
    }
    if (_ports.ampEnv) {
        // Plucked-string shape: very short attack, decay to 0 over
        // _ampDecaySec, zero sustain, very short release. Release is
        // only used on note-off; when a note has already died to zero
        // there's nothing to release.
        _ports.ampEnv->attack (2.0f);
        _ports.ampEnv->decay  (_ampDecaySec * 1000.0f);
        _ports.ampEnv->sustain(0.0f);
        _ports.ampEnv->release(10.0f);
    }
    _currentHz = noteToHz(_targetNote);
    _targetHz  = _currentHz;
}

void AcidSink::setMidiChannel(uint8_t ch)   { if (ch > 16) ch = 0; _midiCh = ch; }

void AcidSink::setWaveform(uint8_t w) {
    if (w > 1) w = 0;
    _waveform = w;
    if (_ports.osc) _ports.osc->begin(toneTypeFor(w));
}

void AcidSink::setTuning(int semitones) {
    if (semitones < -24) semitones = -24;
    if (semitones >  24) semitones =  24;
    _tuningSemi = semitones;
    if (_held) applyFrequencies();
}

void AcidSink::setCutoff(float hz) {
    if (hz < 20.0f)    hz = 20.0f;
    if (hz > 20000.0f) hz = 20000.0f;
    _cutoffHz = hz;
    applyFilterCutoff();
}

void AcidSink::setResonance(float q) {
    if (q < 0.707f) q = 0.707f;
    if (q > 5.0f)   q = 5.0f;
    _resonance = q;
    if (_ports.filter) _ports.filter->resonance(q);
}

void AcidSink::setEnvModAmount(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    _envModAmount = v;
    applyFilterCutoff();
}

void AcidSink::setEnvDecay(float seconds) {
    if (seconds < 0.01f) seconds = 0.01f;
    _envDecaySec = seconds;
}

void AcidSink::setAmpDecay(float seconds) {
    if (seconds < 0.01f) seconds = 0.01f;
    _ampDecaySec = seconds;
    if (_ports.ampEnv) _ports.ampEnv->decay(seconds * 1000.0f);
}

void AcidSink::setAccent(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    _accentAmount = v;
}

void AcidSink::setAccentThreshold(uint8_t v) {
    if (v < 1)   v = 1;
    if (v > 127) v = 127;
    _accentThreshold = v;
}

void AcidSink::setSlideMs(float ms) {
    if (ms < 0.0f)    ms = 0.0f;
    if (ms > 1000.0f) ms = 1000.0f;
    _slideMs = ms;
}

void AcidSink::setVoiceVolumeScale(float s) {
    if (s < 0.0f) s = 0.0f;
    if (s > 1.0f) s = 1.0f;
    _voiceVolumeScale = s;
}

void AcidSink::tick(uint32_t nowMs) {
    const uint32_t dtMs = (_lastTickMs == 0) ? 0 : (nowMs - _lastTickMs);
    _lastTickMs = nowMs;

    if (!_held && _filterEnvVal <= 0.0001f && _currentHz == _targetHz) return;

    // Filter envelope decay — exponential toward 0. At every tick the
    // remaining envelope value is multiplied by exp(-dt / tau), where
    // tau is set so the envelope drops to ~3% in _envDecaySec (~e^-3.5).
    if (_filterEnvVal > 0.0001f && dtMs > 0) {
        const float tau    = _envDecaySec / 3.5f;
        const float factor = expf(-((float)dtMs / 1000.0f) / tau);
        _filterEnvVal *= factor;
        applyFilterCutoff();
    }

    // Portamento glide toward _targetHz in log-space.
    if (_currentHz != _targetHz && _slideMs > 0.0f && dtMs > 0) {
        const float logNow = log2f(_currentHz);
        const float logTgt = log2f(_targetHz);
        const float diff   = logTgt - logNow;
        const float maxStep = (float)dtMs / _slideMs;
        float step;
        if (diff >= 0.0f) step = (diff <= maxStep) ? diff : maxStep;
        else              step = (-diff <= maxStep) ? diff : -maxStep;
        _currentHz = powf(2.0f, logNow + step);
        applyFrequencies();
    } else if (_currentHz != _targetHz) {
        _currentHz = _targetHz;
        applyFrequencies();
    }
}

// ---------------------------------------------------------------------
// MidiSink overrides
// ---------------------------------------------------------------------

void AcidSink::onNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (!channelMatches(channel)) return;
    if (velocity == 0) { onNoteOff(channel, note, 0); return; }

    const bool slideFromPrevious = _held;  // classic 303 rule — slide only on legato
    const bool accent = (velocity >= _accentThreshold);
    _lastAccent = accent;

    pushNote(note);
    _targetNote = note;
    _targetHz   = noteToHz(note) * powf(2.0f, ((float)_tuningSemi + _bend_semi) / 12.0f);
    if (!slideFromPrevious || _slideMs <= 0.0f) {
        _currentHz = _targetHz;
    }
    _held = true;

    // Re-seed filter envelope on every note-on — this is the defining
    // 303 pop that every note is pokey regardless of legato state.
    _filterEnvVal = 1.0f;
    applyFrequencies();
    applyFilterCutoff();

    const float velN = (float)velocity / 127.0f;
    applyOscAmplitude(accent, velN);

    if (_ports.ampEnv) {
        if (!slideFromPrevious) {
            // Full retrigger for a non-legato note-on.
            _ports.ampEnv->noteOn();
        } else {
            // Legato — we could retrigger or hold. Classic 303 tied
            // notes don't retrigger the amp envelope (that's the
            // "slide" sound — pitch glides but amp keeps going). We
            // only retrigger the filter envelope, which is a subtle
            // but essential 303 trait.
        }
    }
}

void AcidSink::onNoteOff(uint8_t channel, uint8_t note, uint8_t /*velocity*/) {
    if (!channelMatches(channel)) return;
    popNote(note);

    uint8_t top;
    if (topNote(&top)) {
        // Legato fallback: slide to the next-highest held note without
        // retriggering.
        _targetNote = top;
        _targetHz   = noteToHz(top) * powf(2.0f, ((float)_tuningSemi + _bend_semi) / 12.0f);
        // Slide time applies here too — releasing a top note while a
        // lower is held glides us back down.
        if (_slideMs <= 0.0f) {
            _currentHz = _targetHz;
            applyFrequencies();
        }
    } else {
        // Stack empty — release the voice.
        _held = false;
        if (_ports.ampEnv) _ports.ampEnv->noteOff();
    }
}

void AcidSink::onPitchBend(uint8_t channel, float semitones) {
    if (!channelMatches(channel)) return;
    _bend_semi = semitones;
    if (_held) {
        _targetHz = noteToHz(_targetNote) * powf(2.0f, ((float)_tuningSemi + _bend_semi) / 12.0f);
        applyFrequencies();
    }
}

void AcidSink::onAllNotesOff(uint8_t channel) {
    if (channel != 0 && !channelMatches(channel)) return;
    _noteStackCount = 0;
    if (_held) {
        _held = false;
        if (_ports.ampEnv) _ports.ampEnv->noteOff();
    }
}

// ---------------------------------------------------------------------
// Note stack
// ---------------------------------------------------------------------

void AcidSink::pushNote(uint8_t note) {
    popNote(note);
    if (_noteStackCount >= kStackSize) {
        for (int i = 1; i < kStackSize; ++i) _noteStack[i - 1] = _noteStack[i];
        _noteStackCount = kStackSize - 1;
    }
    _noteStack[_noteStackCount++] = note;
}

void AcidSink::popNote(uint8_t note) {
    for (int i = 0; i < _noteStackCount; ++i) {
        if (_noteStack[i] == note) {
            for (int j = i + 1; j < _noteStackCount; ++j) _noteStack[j - 1] = _noteStack[j];
            --_noteStackCount;
            return;
        }
    }
}

bool AcidSink::topNote(uint8_t *out) const {
    if (_noteStackCount <= 0) return false;
    if (out) *out = _noteStack[_noteStackCount - 1];
    return true;
}

// ---------------------------------------------------------------------
// Apply helpers
// ---------------------------------------------------------------------

void AcidSink::applyFrequencies() {
    if (_ports.osc) _ports.osc->frequency(_currentHz);
}

void AcidSink::applyFilterCutoff() {
    if (!_ports.filter) return;
    // Final cutoff = baseCutoff × 2^(envMod × envVal × 6 + accent boost).
    // The ×6 turns env 0..1 into 0..6 octaves of swing — classic 303
    // range. Accent adds up to +1.5 octaves on top of that when
    // envAmount is dialled up.
    const float accentBoost = (_lastAccent && _filterEnvVal > 0.01f)
                             ? (_accentAmount * 1.5f * _filterEnvVal)
                             : 0.0f;
    const float octaves = _envModAmount * _filterEnvVal * 6.0f + accentBoost;
    float hz = _cutoffHz * powf(2.0f, octaves);
    if (hz < 20.0f)    hz = 20.0f;
    if (hz > 16000.0f) hz = 16000.0f;   // clamp below Nyquist headroom
    _ports.filter->frequency(hz);
}

void AcidSink::applyOscAmplitude(bool accent, float velNormalized) {
    // Base amplitude at 0.75 (headroom for downstream distortion
    // stages, limiter). Accent pushes up to 1.0 × velocity. Velocity
    // taper is quadratic so soft notes are noticeably quieter —
    // 303-style dynamics.
    const float velScaled = velNormalized * velNormalized;  // quadratic taper
    const float accentGain = accent ? (1.0f + _accentAmount * 0.4f) : 1.0f;
    const float amp = 0.75f * velScaled * accentGain * _voiceVolumeScale;
    if (_ports.osc) _ports.osc->amplitude(amp);
}

float AcidSink::noteToHz(uint8_t note) {
    return 440.0f * powf(2.0f, ((float)note - 69.0f) / 12.0f);
}

short AcidSink::toneTypeFor(uint8_t wave) {
    return (wave == 1) ? WAVEFORM_SQUARE : WAVEFORM_SAWTOOTH;
}
