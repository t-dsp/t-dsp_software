// MultisampleSlot.cpp — see header for design notes.

#include <Arduino.h>
#include <Audio.h>
#include <SD.h>
#include <math.h>

#include <OpenAudio_ArduinoLibrary.h>
#include <TeensyVariablePlayback.h>

#include "MultisampleSlot.h"
#include "NoteParser.h"
#include "../osc/X32FaderLaw.h"

namespace tdsp_synth {

// Envelope shape — fast attack (immediate), no built-in decay/sustain
// shaping (the WAV's own piano decay does that), longer release for a
// natural piano-tail feel. The release sample (if loaded) plays
// alongside this fade and supplies the actual damper-thunk timbre.
static constexpr float kAttackMs  = 0.0f;
static constexpr float kHoldMs    = 0.0f;
static constexpr float kDecayMs   = 0.0f;
static constexpr float kSustain   = 1.0f;
static constexpr float kReleaseMs = 600.0f;

// Release-sample gain, on top of per-voice mixer gain × velocity curve.
// Two flavors:
//   * per-key: every note-off fires a tiny damper-thunk. Barely audible
//     — should color the release of each note without calling attention
//     to itself.
//   * pedal-release: collective thunk when the sustain pedal lifts.
//     Clear, intentional event modeling all dampers falling together.
static constexpr float kReleaseGainPerKey = 0.07f;   // ~-23 dB attenuation
static constexpr float kReleaseGainPedal  = 0.50f;   // ~-6 dB attenuation

// Per-voice maximum gain (envelope post-multiply). 1/kVoices keeps
// 8 simultaneous voices from clipping at the int16 sum stage; we
// recover headroom in the F32 per-slot gain (default fader = 0.75 X32 ≈ 0 dB).
static constexpr float kVoiceMixGain = 1.0f / 8.0f;

// MIDI velocity → linear amplitude. PlayPiano's table is a perceptually-
// shaped curve (roughly square-law); we apply it on top of kVoiceMixGain
// so dynamics feel like an X32 fader rather than a flat multiply.
static float velocityCurve(uint8_t v) {
    if (v == 0) return 0.0f;
    const float x = (float)v / 127.0f;
    return x * x;  // square-law is close enough; refine later if needed
}

MultisampleSlot::MultisampleSlot(const AudioContext &ctx)
    : _ctx(ctx), _sink(this) {
    for (int i = 0; i < kVoices; ++i) {
        _voiceNote[i]       = -1;
        _voiceVel[i]        = 64;
        _voiceStartMs[i]    = 0;
        _voiceReleasing[i]  = false;
        _voiceIsRelease[i]  = false;
        _voiceSustained[i]  = false;
    }
    _bankName[0] = '\0';
    for (int i = 0; i < kMaxReleaseSamples; ++i) {
        _releasePath[i][0] = '\0';
        _releaseRoot[i]    = kReleaseNoNote;
    }
}

const char* MultisampleSlot::displayName() const {
    if (!_bankName[0]) return "Sampler";
    // Capitalize the bank name for display: "piano" -> "Piano".
    // Cached so we don't re-write on every call.
    static char displayBuf[kBankNameLen + 1];
    static const char *cachedSrc = nullptr;
    if (cachedSrc != _bankName) {
        size_t n = strlen(_bankName);
        if (n >= sizeof(displayBuf)) n = sizeof(displayBuf) - 1;
        memcpy(displayBuf, _bankName, n);
        displayBuf[n] = '\0';
        if (n > 0) {
            displayBuf[0] = (char)toupper((unsigned char)displayBuf[0]);
        }
        cachedSrc = _bankName;
    }
    return displayBuf;
}

void MultisampleSlot::begin() {
    initEnvelopes();
    // Mixer gains start at 1.0 — we don't use mixer gain to mute voices
    // (the envelope does that). Stage-1 mixers sum 4 envelopes; stage-2
    // sums the two stage-1 outputs.
    for (int s = 0; s < 3; ++s) {
        _ctx.mixL[s].gain(0, 1.0f);
        _ctx.mixL[s].gain(1, 1.0f);
        _ctx.mixL[s].gain(2, 1.0f);
        _ctx.mixL[s].gain(3, 1.0f);
        _ctx.mixR[s].gain(0, 1.0f);
        _ctx.mixR[s].gain(1, 1.0f);
        _ctx.mixR[s].gain(2, 1.0f);
        _ctx.mixR[s].gain(3, 1.0f);
    }
    applyGain();
}

void MultisampleSlot::initEnvelopes() {
    for (int i = 0; i < kVoices; ++i) {
        for (auto *e : { &_ctx.envL[i], &_ctx.envR[i] }) {
            e->delay(0.0f);
            e->attack(kAttackMs);
            e->hold(kHoldMs);
            e->decay(kDecayMs);
            e->sustain(kSustain);
            e->release(kReleaseMs);
        }
    }
}

void MultisampleSlot::setActive(bool active) {
    if (_active == active) return;
    if (_active && !active) {
        hardStopAll();
    }
    _active = active;
    applyGain();
}

void MultisampleSlot::panic() {
    hardStopAll();
}

void MultisampleSlot::setVolume(float v) {
    _volume = tdsp::x32::quantizeFader(v);
    applyGain();
}

void MultisampleSlot::setOn(bool on) {
    _on = on;
    applyGain();
}

void MultisampleSlot::setListenChannel(uint8_t ch) {
    _listenChannel = (ch <= 16) ? ch : 0;
}

void MultisampleSlot::applyGain() {
    const float linear =
        (_active && _on)
            ? tdsp::x32::faderToLinear(_volume)
            : 0.0f;
    _ctx.gainL->setGain(linear);
    _ctx.gainR->setGain(linear);
}

// --- Bank scan ---------------------------------------------------------

// Parse a basename like "C4", "C4_v8", or "C4v8" into (midi, midiVelocity).
// Returns true on success. midiVelocity defaults to 64 (mf) when no v-tag.
// v-tag mapping: vN -> (N*127 + 8) / 16 (linear, v1=8, v8=64, v16=127).
static bool parseSampleBasename(const char *base, int &midi, uint8_t &midiVel) {
    if (!base || !*base) return false;

    // Look for a 'v' followed by digits — separator may be '_' or absent.
    // We split the basename at the v-tag, parse the note from the prefix,
    // and the velocity from the digits after 'v'.
    const char *vTag = nullptr;
    for (const char *p = base + 1; *p; ++p) {
        if ((*p == 'v' || *p == 'V') && isdigit((unsigned char)p[1])) {
            // Accept "<note>v<N>" and "<note>_v<N>"; reject mid-name 'v's
            // by requiring the prefix to end in a digit ("C4", "F#3-1")
            // or an octave-sign character.
            char prev = p[-1];
            if (isdigit((unsigned char)prev) || prev == '_') {
                vTag = p;
                break;
            }
        }
    }

    char noteBuf[16] = {0};
    if (vTag) {
        size_t prefixLen = (size_t)(vTag - base);
        // Strip a trailing '_' from the prefix, if any (so "C4_v8" -> "C4").
        if (prefixLen > 0 && base[prefixLen - 1] == '_') prefixLen -= 1;
        if (prefixLen == 0 || prefixLen >= sizeof(noteBuf)) return false;
        memcpy(noteBuf, base, prefixLen);
        noteBuf[prefixLen] = '\0';
    } else {
        size_t n = strlen(base);
        if (n == 0 || n >= sizeof(noteBuf)) return false;
        memcpy(noteBuf, base, n + 1);
    }

    midi = parseNoteName(noteBuf);
    if (midi < 0) return false;

    if (vTag) {
        int v = 0;
        for (const char *p = vTag + 1; *p && isdigit((unsigned char)*p); ++p) {
            v = v * 10 + (*p - '0');
        }
        if (v < 1) v = 1;
        if (v > 16) v = 16;
        midiVel = (uint8_t)((v * 127 + 8) / 16);
    } else {
        midiVel = 64;
    }
    return true;
}

// Detect a release-sample filename and (optionally) extract its
// recorded MIDI note. Accepts:
//   _release1.wav            -> noteOut = -1 (round-robin pool)
//   _release_C4.wav          -> noteOut = 60
//   release_F#3.wav          -> noteOut = 54
//   rls5.wav                 -> noteOut = -1
//   rel-A0.wav               -> noteOut = 21
//
// Returns true if the filename matches a release-sample pattern,
// false otherwise. noteOut is set to -1 when the filename has no
// parseable note suffix (caller should treat as kReleaseNoNote).
static bool parseReleaseBasename(const char *base, int &noteOut) {
    noteOut = -1;
    if (!base) return false;

    const char *p = base;
    if (*p == '_') ++p;

    int prefixLen = 0;
    if (strncasecmp(p, "release", 7) == 0)      prefixLen = 7;
    else if (strncasecmp(p, "rls", 3) == 0)     prefixLen = 3;
    else if (strncasecmp(p, "rel", 3) == 0)     prefixLen = 3;
    else return false;

    p += prefixLen;
    // Skip optional separator before the note name.
    if (*p == '_' || *p == '-') ++p;

    // Empty suffix or pure-digit suffix (e.g., "rel1") -> no note info.
    if (!*p) return true;
    bool allDigits = true;
    for (const char *q = p; *q; ++q) {
        if (!isdigit((unsigned char)*q)) { allDigits = false; break; }
    }
    if (allDigits) return true;

    int midi = parseNoteName(p);
    if (midi >= 0) noteOut = midi;
    return true;
}

int MultisampleSlot::scanBank(const char *bankPath) {
    hardStopAll();
    _numSamples = 0;
    _numReleaseSamples = 0;
    _bankName[0] = '\0';

    if (!bankPath || !*bankPath) return 0;

    // Extract last path component as the display bank name.
    const char *p = strrchr(bankPath, '/');
    const char *baseName = p ? p + 1 : bankPath;
    strncpy(_bankName, baseName, sizeof(_bankName) - 1);
    _bankName[sizeof(_bankName) - 1] = '\0';

    File dir = SD.open(bankPath);
    if (!dir) {
        Serial.print("[sampler] bank not found on SD: ");
        Serial.println(bankPath);
        return 0;
    }
    if (!dir.isDirectory()) {
        Serial.print("[sampler] not a directory: ");
        Serial.println(bankPath);
        dir.close();
        return 0;
    }

    while (true) {
        File entry = dir.openNextFile();
        if (!entry) break;
        if (entry.isDirectory()) {
            entry.close();
            continue;
        }
        if (_numSamples >= kMaxSamples) {
            entry.close();
            break;
        }

        // Get filename without path. Teensy SD's name() is the basename.
        const char *fname = entry.name();
        // Skip dotfiles (macOS metadata etc). Files starting with '_' are
        // accepted because release samples may be named "_release1.wav".
        if (fname[0] == '.') {
            entry.close();
            continue;
        }

        // Require .wav extension (case-insensitive).
        const size_t len = strlen(fname);
        if (len < 5) { entry.close(); continue; }
        const char *ext = fname + len - 4;
        if (!(ext[0] == '.' &&
              (ext[1] == 'w' || ext[1] == 'W') &&
              (ext[2] == 'a' || ext[2] == 'A') &&
              (ext[3] == 'v' || ext[3] == 'V'))) {
            entry.close();
            continue;
        }

        // Strip extension to get the basename.
        char base[32] = {0};
        size_t baseLen = len - 4;
        if (baseLen >= sizeof(base)) baseLen = sizeof(base) - 1;
        memcpy(base, fname, baseLen);
        base[baseLen] = '\0';

        // Release sample? Try the release parser first; if it matches,
        // store with optional per-note root and skip note-name parsing.
        {
            int releaseNote = -1;
            if (parseReleaseBasename(base, releaseNote)) {
                if (_numReleaseSamples < kMaxReleaseSamples) {
                    snprintf(_releasePath[_numReleaseSamples],
                             sizeof(_releasePath[_numReleaseSamples]),
                             "%s/%s", bankPath, fname);
                    _releaseRoot[_numReleaseSamples] =
                        (releaseNote >= 0) ? (uint8_t)releaseNote : kReleaseNoNote;
                    _numReleaseSamples += 1;
                }
                entry.close();
                continue;
            }
        }

        int midi = -1;
        uint8_t midiVel = 64;
        if (!parseSampleBasename(base, midi, midiVel)) {
            entry.close();
            continue;
        }

        snprintf(_samplePath[_numSamples], sizeof(_samplePath[_numSamples]),
                 "%s/%s", bankPath, fname);
        _sampleRoot[_numSamples]     = (uint8_t)midi;
        _sampleVelocity[_numSamples] = midiVel;
        _numSamples += 1;

        entry.close();
    }
    dir.close();

    Serial.print("[sampler] bank \"");
    Serial.print(_bankName);
    Serial.print("\" loaded: ");
    Serial.print(_numSamples);
    Serial.print(" samples");
    if (_numReleaseSamples > 0) {
        Serial.print(" + ");
        Serial.print(_numReleaseSamples);
        Serial.print(" release sample");
        Serial.print(_numReleaseSamples == 1 ? "" : "s");
    }
    Serial.println();
    return _numSamples;
}

// --- Voice management --------------------------------------------------

int MultisampleSlot::closestSampleIdx(uint8_t note, uint8_t velocity) const {
    if (_numSamples <= 0) return -1;
    // Hierarchical: closest root note first, then closest velocity among
    // ties. Single-pass: track best (noteDist, velDist, idx) in
    // lex-order. velDist only matters when noteDist matches.
    int bestIdx = 0;
    int bestNoteDist = abs((int)note - (int)_sampleRoot[0]);
    int bestVelDist  = abs((int)velocity - (int)_sampleVelocity[0]);
    for (int i = 1; i < _numSamples; ++i) {
        int n = abs((int)note - (int)_sampleRoot[i]);
        int v = abs((int)velocity - (int)_sampleVelocity[i]);
        if (n < bestNoteDist || (n == bestNoteDist && v < bestVelDist)) {
            bestNoteDist = n;
            bestVelDist  = v;
            bestIdx = i;
        }
    }
    return bestIdx;
}

int MultisampleSlot::pickVoice(uint8_t /*note*/) {
    // Prefer idle slots; then voices already releasing; then steal the
    // oldest playing one.
    int idleIdx = -1;
    int releasingIdx = -1;
    int oldestIdx = 0;
    uint32_t oldestMs = _voiceStartMs[0];
    for (int i = 0; i < kVoices; ++i) {
        if (_voiceNote[i] < 0 && !_voiceReleasing[i]) {
            idleIdx = i;
            break;
        }
        if (_voiceReleasing[i] && releasingIdx < 0) releasingIdx = i;
        if (_voiceStartMs[i] < oldestMs) {
            oldestMs = _voiceStartMs[i];
            oldestIdx = i;
        }
    }
    if (idleIdx >= 0)      return idleIdx;
    if (releasingIdx >= 0) return releasingIdx;
    return oldestIdx;
}

void MultisampleSlot::doNoteOn(uint8_t note, uint8_t velocity) {
    if (_numSamples == 0) return;

    const int v = pickVoice(note);
    if (v < 0) return;

    // If stealing an active voice, hard-stop it first to avoid layering
    // the new note onto the tail of the old SD stream.
    if (_voiceNote[v] >= 0 || _voiceReleasing[v] || _voiceIsRelease[v]) {
        hardStopVoice(v);
    }

    const int sampleIdx = closestSampleIdx(note, velocity);
    if (sampleIdx < 0) return;

    const int rootNote = _sampleRoot[sampleIdx];
    const float rate = powf(2.0f, (float)((int)note - rootNote) / 12.0f);

    AudioPlaySdResmp     &player = _ctx.players[v];
    AudioEffectEnvelope  &envL   = _ctx.envL[v];
    AudioEffectEnvelope  &envR   = _ctx.envR[v];

    // Quadratic interpolation = better quality, more CPU. Fine for 8 voices
    // on T4.1; we can drop to none later if CPU budget gets tight.
    player.enableInterpolation(true);
    if (!player.playWav(_samplePath[sampleIdx])) {
        Serial.print("[sampler] playWav failed: ");
        Serial.println(_samplePath[sampleIdx]);
        return;
    }
    player.setPlaybackRate(rate);

    // Per-voice mixer gain combines kVoiceMixGain headroom + velocity curve.
    const float vGain = kVoiceMixGain * velocityCurve(velocity);
    const int mixSlot = v % 4;
    const int mixStage = (v < 4) ? 0 : 1;
    _ctx.mixL[mixStage].gain(mixSlot, vGain);
    _ctx.mixR[mixStage].gain(mixSlot, vGain);

    envL.noteOn();
    envR.noteOn();

    _voiceNote[v]      = (int8_t)note;
    _voiceVel[v]       = velocity;
    _voiceStartMs[v]   = millis();
    _voiceReleasing[v] = false;
}

void MultisampleSlot::doNoteOff(uint8_t note) {
    // Find the most recent voice playing this note (highest startMs).
    int best = -1;
    uint32_t bestMs = 0;
    for (int i = 0; i < kVoices; ++i) {
        if (_voiceNote[i] == (int8_t)note && !_voiceReleasing[i] &&
            !_voiceIsRelease[i]) {
            if (best < 0 || _voiceStartMs[i] > bestMs) {
                best = i;
                bestMs = _voiceStartMs[i];
            }
        }
    }
    if (best < 0) return;

    if (_sustain) {
        // Sustain pedal is down — defer the release. The damper-thunk
        // is suppressed too; it'll fire (once, collective) when the
        // pedal lifts. Real piano: lifting a key while pedaled doesn't
        // damp the string, so no thunk.
        _voiceSustained[best] = true;
        return;
    }

    const uint8_t releasedVel = _voiceVel[best];
    releaseVoice(best);
    // Fire a per-key release sample (subtle damper sound) for this
    // specific note, scaled to its attack velocity.
    triggerReleaseSample(note, releasedVel, /*pedal=*/false);
}

void MultisampleSlot::setSustain(bool on) {
    if (_sustain == on) return;
    _sustain = on;
    if (!on) {
        // Pedal released — release every voice that was held by it.
        // Fire a single damper-thunk pitched to the average of the
        // sustained notes (so a low chord gets a low thunk and a high
        // chord gets a high one). Single thunk, not per-note, to avoid
        // polyphony spikes when releasing a big chord.
        int repSumNote = 0, repSumVel = 0, repCount = 0;
        for (int i = 0; i < kVoices; ++i) {
            if (_voiceSustained[i]) {
                if (_voiceNote[i] >= 0) {
                    repSumNote += _voiceNote[i];
                    repSumVel  += _voiceVel[i];
                    repCount   += 1;
                }
                releaseVoice(i);
                _voiceSustained[i] = false;
            }
        }
        if (repCount > 0) {
            const uint8_t repNote = (uint8_t)(repSumNote / repCount);
            const uint8_t repVel  = (uint8_t)(repSumVel  / repCount);
            Serial.print("[sampler] pedal-up: releasing ");
            Serial.print(repCount);
            Serial.print(" sustained voices, thunk note=");
            Serial.print((int)repNote);
            Serial.print(" vel=");
            Serial.println((int)repVel);
            triggerReleaseSample(repNote, repVel, /*pedal=*/true);
        }
    }
}

void MultisampleSlot::triggerReleaseSample(uint8_t note, uint8_t velocity, bool pedal) {
    if (_numReleaseSamples == 0) {
        Serial.println("[sampler] triggerRelease: no release samples loaded");
        return;
    }

    // Find a free voice; don't steal a holding/playing voice, but DO
    // steal a voice that's already in release fade — its tail is about
    // to vanish anyway, and a missing damper-thunk is more audible than
    // a slightly-clipped tail.
    int v = -1;
    for (int i = 0; i < kVoices; ++i) {
        if (_voiceNote[i] < 0 && !_voiceReleasing[i] && !_voiceIsRelease[i]) {
            v = i;
            break;
        }
    }
    if (v < 0) {
        // Fall back to stealing a releasing voice (oldest first).
        uint32_t oldest = 0;
        for (int i = 0; i < kVoices; ++i) {
            if (_voiceReleasing[i] || _voiceIsRelease[i]) {
                if (v < 0 || _voiceStartMs[i] < oldest) {
                    v = i;
                    oldest = _voiceStartMs[i];
                }
            }
        }
    }
    if (v < 0) {
        Serial.println("[sampler] triggerRelease: no voice available");
        return;
    }
    if (_voiceNote[v] >= 0 || _voiceReleasing[v] || _voiceIsRelease[v]) {
        // Stolen voice — clear it before reusing.
        hardStopVoice(v);
    }

    // Pick the release sample. If any have a recorded root note, prefer
    // the closest one to `note`. Otherwise round-robin over the unset
    // pool — older bank format (e.g., "rel1.wav", "rel2.wav") with no
    // per-note mapping.
    int rIdx       = -1;
    int bestDist   = INT_MAX;
    int unsetCount = 0;
    int firstUnset = -1;
    for (int i = 0; i < _numReleaseSamples; ++i) {
        if (_releaseRoot[i] == kReleaseNoNote) {
            if (firstUnset < 0) firstUnset = i;
            unsetCount++;
        } else {
            int d = abs((int)note - (int)_releaseRoot[i]);
            if (d < bestDist) {
                bestDist = d;
                rIdx     = i;
            }
        }
    }
    if (rIdx < 0) {
        // No samples carry a note; round-robin over the unset pool.
        if (firstUnset < 0) return;  // shouldn't happen — numReleaseSamples > 0
        rIdx = firstUnset + (_releaseRrCounter % unsetCount);
        _releaseRrCounter += 1;
    }

    AudioPlaySdResmp    &player = _ctx.players[v];
    AudioEffectEnvelope &envL   = _ctx.envL[v];
    AudioEffectEnvelope &envR   = _ctx.envR[v];

    player.enableInterpolation(false);  // release samples are unpitched
    if (!player.playWav(_releasePath[rIdx])) {
        Serial.print("[sampler] release playWav failed: ");
        Serial.println(_releasePath[rIdx]);
        return;
    }
    player.setPlaybackRate(1.0f);

    // Compose gain like a regular note: per-voice mix headroom × velocity
    // curve × release-sample attenuation. Per-key releases are far
    // quieter than the collective pedal-release thunk.
    const float kReleaseGain = pedal ? kReleaseGainPedal : kReleaseGainPerKey;
    const float vGain = kVoiceMixGain * velocityCurve(velocity) * kReleaseGain;
    const int mixSlot  = v % 4;
    const int mixStage = (v < 4) ? 0 : 1;
    _ctx.mixL[mixStage].gain(mixSlot, vGain);
    _ctx.mixR[mixStage].gain(mixSlot, vGain);

    envL.noteOn();
    envR.noteOn();

    _voiceNote[v]      = -1;        // not associated with a played note
    _voiceVel[v]       = velocity;  // for accounting; doesn't drive gain post-trigger
    _voiceStartMs[v]   = millis();
    _voiceReleasing[v] = false;     // not in a fade-out — let the WAV ring out
    _voiceIsRelease[v] = true;
}

void MultisampleSlot::releaseVoice(int v) {
    if (v < 0 || v >= kVoices) return;
    _ctx.envL[v].noteOff();
    _ctx.envR[v].noteOff();
    _voiceReleasing[v] = true;
    // Note number stays set so a same-note retrigger from the keyboard
    // doesn't double-trigger this voice mid-release; pollVoices() clears
    // it once the envelope decays.
}

void MultisampleSlot::hardStopVoice(int v) {
    if (v < 0 || v >= kVoices) return;
    // Tear down the envelope without a release tail (clicks possible —
    // use only when stealing or panicking).
    _ctx.envL[v].noteOff();
    _ctx.envR[v].noteOff();
    _ctx.players[v].stop();
    _voiceNote[v]       = -1;
    _voiceReleasing[v]  = false;
    _voiceIsRelease[v]  = false;
    _voiceSustained[v]  = false;
}

void MultisampleSlot::hardStopAll() {
    for (int i = 0; i < kVoices; ++i) hardStopVoice(i);
}

void MultisampleSlot::pollVoices() {
    for (int i = 0; i < kVoices; ++i) {
        if (_voiceNote[i] < 0 && !_voiceReleasing[i] && !_voiceIsRelease[i]) continue;

        // Release-sample voice: when the SD stream finishes, kick the
        // envelope into release so it fades and the slot frees up.
        // Without this the envelope stays in sustain forever (noteOff
        // was never called) and pollVoices below wouldn't reclaim it.
        if (_voiceIsRelease[i] && !_voiceReleasing[i] &&
            !_ctx.players[i].isPlaying()) {
            _ctx.envL[i].noteOff();
            _ctx.envR[i].noteOff();
            _voiceReleasing[i] = true;
        }

        if (!_ctx.envL[i].isActive() && !_ctx.envR[i].isActive()) {
            _ctx.players[i].stop();
            _voiceNote[i]      = -1;
            _voiceReleasing[i] = false;
            _voiceIsRelease[i] = false;
        }
    }
}

// --- MidiSink shim -----------------------------------------------------

void MultisampleSlot::Sink::onNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (!_slot->listens(channel)) return;
    if (velocity == 0) {
        _slot->doNoteOff(note);
    } else {
        _slot->doNoteOn(note, velocity);
    }
}

void MultisampleSlot::Sink::onNoteOff(uint8_t channel, uint8_t note, uint8_t /*velocity*/) {
    if (!_slot->listens(channel)) return;
    _slot->doNoteOff(note);
}

void MultisampleSlot::Sink::onSustain(uint8_t channel, bool on) {
    if (!_slot->listens(channel)) return;
    _slot->setSustain(on);
}

void MultisampleSlot::Sink::onAllNotesOff(uint8_t channel) {
    if (channel != 0 && !_slot->listens(channel)) return;
    _slot->hardStopAll();
}

}  // namespace tdsp_synth
