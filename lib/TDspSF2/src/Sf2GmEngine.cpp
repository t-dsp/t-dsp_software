// Sf2GmEngine.cpp — see Sf2GmEngine.h for the architecture.
#include "Sf2GmEngine.h"
#include <SD.h>
#include <math.h>

// The Teensy core auto-detects installed PSRAM (8 MB now, 16 MB after the second
// APS6404L is soldered). sf22aswt already spills samples to it; we only read the
// size to pick how many instruments stay resident. 0 = no PSRAM.
extern "C" uint8_t external_psram_size;
extern "C" void *extmem_malloc(size_t size);
extern "C" void  extmem_free(void *ptr);

// note + fractional pitch-bend -> Hz (A440 equal temperament). Used for live bend;
// AudioSynthWavetable::noteToFreq only takes an int, so we compute the float here.
static inline float noteBendToFreq(float note) {
    return 440.0f * powf(2.0f, (note - 69.0f) * (1.0f / 12.0f));
}

void Sf2GmEngine::attachVoices(AudioSynthWavetable* voices, int count) {
    m_voice = voices;
    m_numVoices = (count > kMaxVoices) ? kMaxVoices : (count < 0 ? 0 : count);
}

bool Sf2GmEngine::begin(const char* sf2Path) {
    // reset allocator + channel state
    for (int i = 0; i < kMaxVoices; i++) {
        m_vNote[i] = -1; m_vChan[i] = 0; m_vAge[i] = 0;
        m_vBaseNote[i] = 0; m_vInst[i] = nullptr;
    }
    for (int c = 0; c < 17; c++) { m_program[c] = 0; m_bend[c] = 0.0f; }
    if (m_noteInst)  { extmem_free(m_noteInst);  m_noteInst = nullptr; }
    if (m_instScale) { extmem_free(m_instScale); m_instScale = nullptr; }
    if (m_instRoot)  { extmem_free(m_instRoot);  m_instRoot = nullptr; }
    m_ready = false;

    if (!m_master.ReadFile(sf2Path)) {
        Serial.print("[sf2] ReadFile FAILED: ");
        m_master.printSF2ErrorInfo(Serial);
        return false;
    }
    if (!buildPresetMap(sf2Path))
        Serial.println("[sf2] warning: preset map incomplete (GM routing degraded)");

    // Size the resident-instrument cache from installed PSRAM. A GM moment needs
    // <=17 distinct patches (16 melodic + drums), so 20 rarely evicts; 16 MB gets more.
    // A busy GM moment can touch ~13 distinct drum instruments (one per drum) + up to
    // 15 melodic channels, so size generously; the library still budget-checks each
    // load against actual PSRAM and ensureInstrument() degrades gracefully on overflow.
    int mb = (int)external_psram_size;
    m_numSlots = (mb >= 16) ? 32 : (mb >= 1 ? 24 : 4);
    if (m_numSlots > kMaxSlots) m_numSlots = kMaxSlots;

    // Voices start silent (tone_amp == 0) until amplitude() is called.
    for (int i = 0; i < m_numVoices; i++) m_voice[i].amplitude(m_gain);

    // Preload the default melodic patch (GM 0 @ mid-note) + kick drum so the first
    // note never waits on SD.
    if (m_noteInst) {
        int piano = m_noteInst[0 * 128 + 60];
        int kick  = m_noteInst[kDrumProg * 128 + 36];
        if (piano >= 0) ensureInstrument(piano);
        if (kick  >= 0) ensureInstrument(kick);
        m_ready = true;
        Serial.printf("[sf2] ready: %d MB PSRAM, %d cache slots, %d voices "
                      "(GM0@60->inst%d, kick36->inst%d)\n",
                      mb, m_numSlots, m_numVoices, piano, kick);
    } else {
        Serial.println("[sf2] ERROR: note map not built -> engine idle");
    }
    return m_ready;
}

// Build the per-(program,note) instrument map with PROPER SF2 zone matching.
//
// A GM preset (melodic or drum) is a set of zones; each zone points at an SF2
// instrument and applies over a key range. The range is the zone's own `keyRange`
// generator if present, else the range the instrument itself covers (the union of
// its internal igen keyRanges). A note sounds the instrument of the matching zone.
// This is essential because GeneralUser GS layers many instruments per preset:
// drum kits map one instrument per drum, and ~half the melodic patches are split by
// key/velocity across 2-3 instruments. The naive "last instrument in the preset"
// approach played the wrong sample (drums = noise, split patches = out of key/silent).
//
// We slurp the six pdta sub-chunks into PSRAM (positions from the master's public
// lazy `sfbk`), resolve all 129*128 cells in RAM (no per-note SD seeks), then free
// the scratch. The 33 KB note map stays PSRAM-resident.
bool Sf2GmEngine::buildPresetMap(const char* sf2Path) {
    auto& pdta = m_master.sfbk.pdta;
    const uint32_t PHN = pdta.phdr_count, PBN = pdta.pbag_count, PGN = pdta.pgen_count;
    const uint32_t INN = pdta.inst_count, IBN = pdta.ibag_count, IGN = pdta.igen_count;
    const uint32_t SHN = pdta.shdr_count;
    if (PHN < 2 || PBN < 2 || PGN < 1 || INN < 2 || IBN < 2 || IGN < 1 || SHN < 1) return false;

    File f = SD.open(sf2Path);
    if (!f) return false;
    auto slurp = [&](uint32_t pos, uint32_t bytes) -> uint8_t* {
        uint8_t* p = (uint8_t*)extmem_malloc(bytes);
        if (p) { f.seek(pos); f.read(p, bytes); }
        return p;
    };
    uint8_t* PBAG = slurp(pdta.pbag_position, PBN * 4);
    uint8_t* PGEN = slurp(pdta.pgen_position, PGN * 4);
    uint8_t* PHDR = slurp(pdta.phdr_position, PHN * 38);
    uint8_t* INST = slurp(pdta.inst_position, INN * 22);
    uint8_t* IBAG = slurp(pdta.ibag_position, IBN * 4);
    uint8_t* IGEN = slurp(pdta.igen_position, IGN * 4);
    uint8_t* SHDR = slurp(pdta.shdr_position, SHN * 46);
    f.close();

    m_noteInst  = (int16_t*)extmem_malloc(129 * 128 * sizeof(int16_t));
    m_instScale = (int16_t*)extmem_malloc(INN * sizeof(int16_t));
    m_instRoot  = (int16_t*)extmem_malloc(INN * sizeof(int16_t));
    m_instCount = (int)INN;
    bool ok = PBAG && PGEN && PHDR && INST && IBAG && IGEN && SHDR &&
              m_noteInst && m_instScale && m_instRoot;

    if (ok) {
        auto rd16 = [](const uint8_t* b, uint32_t off) -> uint16_t {
            return (uint16_t)(b[off] | (b[off + 1] << 8));
        };
        const uint16_t GEN_INST  = (uint16_t)SF22ASWT::SFGenerator::instrument;       // 41
        const uint16_t GEN_KRNG  = (uint16_t)SF22ASWT::SFGenerator::keyRange;          // 43
        const uint16_t GEN_SCALE = (uint16_t)SF22ASWT::SFGenerator::scaleTuning;       // 56
        const uint16_t GEN_ROOT  = (uint16_t)SF22ASWT::SFGenerator::overridingRootKey; // 58
        const uint16_t GEN_SMPL  = (uint16_t)SF22ASWT::SFGenerator::sampleID;          // 53

        // Per-instrument scaleTuning + root: scan each instrument's zones, take the
        // scaleTuning in effect (zone override, else the instrument's global zone) and
        // the root of its first sample zone (overridingRootKey, else the sample header's
        // original key). Enough to correct the pitched-percussion / SFX patches whose
        // roots are uniform; scale defaults to 100 (normal) so ordinary patches are untouched.
        for (uint32_t i = 0; i < INN; i++) { m_instScale[i] = 100; m_instRoot[i] = 60; }
        for (uint32_t i = 0; i + 1 < INN; i++) {
            uint32_t b0 = rd16(INST, i * 22 + 20), b1 = rd16(INST, (i + 1) * 22 + 20);
            int gScale = 100; int scale = 100, root = -1; bool haveSample = false;
            for (uint32_t z = b0; z < b1 && z + 1 < IBN; z++) {
                uint32_t g0 = rd16(IBAG, z * 4), g1 = rd16(IBAG, (z + 1) * 4);
                int zScale = -1, zRoot = -1, sampleId = -1;
                for (uint32_t g = g0; g < g1 && g < IGN; g++) {
                    uint16_t op = rd16(IGEN, g * 4), amt = rd16(IGEN, g * 4 + 2);
                    if      (op == GEN_SCALE) zScale = (int16_t)amt;
                    else if (op == GEN_ROOT)  zRoot  = amt;
                    else if (op == GEN_SMPL)  sampleId = amt;
                }
                if (sampleId < 0) { if (zScale >= 0) gScale = zScale; continue; } // global zone
                if (!haveSample) {                                                // first real sample zone
                    scale = (zScale >= 0) ? zScale : gScale;
                    root  = (zRoot  >= 0) ? zRoot
                          : (sampleId < (int)SHN ? SHDR[sampleId * 46 + 40] : 60);
                    haveSample = true;
                }
            }
            m_instScale[i] = (int16_t)scale;
            if (root >= 0) m_instRoot[i] = (int16_t)root;
        }

        // Does instrument `inst` cover note N via any of its own igen keyRanges?
        // (No keyRange anywhere -> covers the whole keyboard.)
        auto instCovers = [&](uint32_t inst, int N) -> bool {
            if (inst + 1 >= INN) return false;
            uint32_t b0 = rd16(INST, inst * 22 + 20), b1 = rd16(INST, (inst + 1) * 22 + 20);
            bool sawKR = false;
            for (uint32_t z = b0; z < b1 && z + 1 < IBN; z++) {
                uint32_t g0 = rd16(IBAG, z * 4), g1 = rd16(IBAG, (z + 1) * 4);
                for (uint32_t g = g0; g < g1 && g < IGN; g++)
                    if (rd16(IGEN, g * 4) == GEN_KRNG) {
                        sawKR = true;
                        uint16_t amt = rd16(IGEN, g * 4 + 2);
                        if ((amt & 0xFF) <= N && N <= (amt >> 8)) return true;
                    }
            }
            return !sawKR;
        };
        // Resolve one preset (phdr index pi) at note N -> instrument, or -1.
        // Preset zones with an explicit keyRange win over range-less ones; within each
        // group the FIRST match wins. First (not last) matters because GeneralUser lists
        // the MAIN instrument first and layers auxiliary ones after (release/sine/percussion
        // tails) that AudioSynthWavetable can't sum — last-wins played the tail, not the note.
        auto resolve = [&](uint32_t pi, int N) -> int {
            uint32_t bagS = rd16(PHDR, pi * 38 + 24), bagE = rd16(PHDR, (pi + 1) * 38 + 24);
            int explicitHit = -1, coverHit = -1;
            for (uint32_t z = bagS; z < bagE && z + 1 < PBN; z++) {
                uint32_t g0 = rd16(PBAG, z * 4), g1 = rd16(PBAG, (z + 1) * 4);
                int kLo = -1, kHi = -1, inst = -1;
                for (uint32_t g = g0; g < g1 && g < PGN; g++) {
                    uint16_t op = rd16(PGEN, g * 4), amt = rd16(PGEN, g * 4 + 2);
                    if (op == GEN_KRNG) { kLo = amt & 0xFF; kHi = amt >> 8; }
                    else if (op == GEN_INST) inst = amt;
                }
                if (inst < 0) continue;                                     // global zone
                if (kLo >= 0) { if (explicitHit < 0 && kLo <= N && N <= kHi) explicitHit = inst; }
                else if (coverHit < 0 && instCovers(inst, N)) coverHit = inst;
            }
            return (explicitHit >= 0) ? explicitHit : coverHit;
        };

        for (int i = 0; i < 129 * 128; i++) m_noteInst[i] = -1;

        // Locate the phdr row for each GM program (bank 0) + the drum kit (bank 128).
        int progPi[128]; for (int p = 0; p < 128; p++) progPi[p] = -1;
        int drumPi = -1;
        for (uint32_t i = 0; i + 1 < PHN; i++) {              // skip trailing EOP
            uint16_t preset = rd16(PHDR, i * 38 + 20), bank = rd16(PHDR, i * 38 + 22);
            if (bank == 0 && preset < 128) { if (progPi[preset] < 0) progPi[preset] = i; }
            else if (bank == kDrumBank)    { if (preset == 0) drumPi = i; else if (drumPi < 0) drumPi = i; }
        }
        for (int p = 0; p < 128; p++)
            if (progPi[p] >= 0)
                for (int N = 0; N < 128; N++) m_noteInst[p * 128 + N] = (int16_t)resolve(progPi[p], N);
        if (drumPi >= 0)
            for (int N = 0; N < 128; N++) m_noteInst[kDrumProg * 128 + N] = (int16_t)resolve(drumPi, N);
    }

    if (PBAG) extmem_free(PBAG);
    if (PGEN) extmem_free(PGEN);
    if (PHDR) extmem_free(PHDR);
    if (INST) extmem_free(INST);
    if (IBAG) extmem_free(IBAG);
    if (IGEN) extmem_free(IGEN);
    if (SHDR) extmem_free(SHDR);
    return ok;
}

int Sf2GmEngine::resolveInstrument(uint8_t channel, uint8_t note) const {
    if (channel < 1 || channel > 16 || !m_noteInst || note > 127) return -1;
    int prog = (channel == kDrumChannel) ? kDrumProg : m_program[channel];
    int inst = m_noteInst[prog * 128 + note];
    if (inst < 0 && channel != kDrumChannel)          // melodic fallback: GM 0 at this note
        inst = m_noteInst[0 * 128 + note];
    return inst;
}

bool Sf2GmEngine::isDrumInstrument(int idx) const {
    if (idx < 0 || !m_noteInst) return false;
    const int16_t* row = m_noteInst + kDrumProg * 128;
    for (int n = 0; n < 128; n++) if (row[n] == idx) return true;
    return false;
}

bool Sf2GmEngine::instrumentInUse(int instIndex) const {
    // find the resident data pointer for this instrument, then see if a voice pins it
    const AudioSynthWavetable::instrument_data* p = nullptr;
    for (int s = 0; s < m_numSlots; s++)
        if (m_slot[s].instIndex == instIndex) { p = m_slot[s].inst; break; }
    if (!p) return false;
    for (int i = 0; i < m_numVoices; i++)
        if (m_voice[i].isPlaying() && m_vInst[i] == p) return true;
    return false;
}

AudioSynthWavetable::instrument_data* Sf2GmEngine::ensureInstrument(int instIndex) {
    if (instIndex < 0) return nullptr;

    // already resident?
    for (int s = 0; s < m_numSlots; s++)
        if (m_slot[s].instIndex == instIndex && m_slot[s].inst) {
            m_slot[s].lastUsed = ++m_useCounter;
            return m_slot[s].inst;
        }

    // pick a target slot: prefer empty, else LRU that no live voice pins
    int target = -1;
    for (int s = 0; s < m_numSlots; s++)
        if (m_slot[s].instIndex < 0) { target = s; break; }
    if (target < 0) {
        uint32_t best = UINT32_MAX;
        for (int s = 0; s < m_numSlots; s++)
            if (!instrumentInUse(m_slot[s].instIndex) && m_slot[s].lastUsed < best) {
                best = m_slot[s].lastUsed; target = s;
            }
        if (target < 0) return nullptr;   // everything pinned (should not happen)
    }

    Slot& sl = m_slot[target];
    if (!sl.cloned) {
        if (!m_master.CloneInto(sl.reader)) return nullptr;
        sl.cloned = true;
    }
    AudioSynthWavetable::instrument_data* old = sl.inst;
    AudioSynthWavetable::instrument_data* loaded = nullptr;
    // Load_instrument frees THIS reader's previous sample data (PSRAM) before loading.
    if (!sl.reader.Load_instrument(instIndex, loaded)) {
        return old ? old : nullptr;       // keep prior patch; degrade, don't crash
    }
    // Drums are one-shots. Several GeneralUser drum samples loop (open hi-hat, toms that
    // inherit loop=1 from the instrument's global zone) with 15-48 s release envelopes, so
    // a hit rings for tens of seconds ("stuck"). Force those samples non-looping; they then
    // play their body once and stop. (Drum-set instruments are all percussion, so disabling
    // loop is harmless even for the few also used as melodic patches, e.g. agogo/woodblock.)
    if (isDrumInstrument(instIndex)) {
        auto* sd = const_cast<AudioSynthWavetable::sample_data*>(loaded->samples);
        for (int i = 0; i < loaded->sample_count; i++) const_cast<bool&>(sd[i].LOOP) = false;
    }
    if (old) delete old;   // struct only; its samples were freed above (minor arr leak: TODO)
    sl.inst = loaded; sl.instIndex = instIndex; sl.lastUsed = ++m_useCounter;
    return loaded;
}

int Sf2GmEngine::pickVoice() {
    for (int i = 0; i < m_numVoices; i++) if (!m_voice[i].isPlaying()) return i; // truly idle
    for (int i = 0; i < m_numVoices; i++) if (m_vNote[i] == -1) return i;         // released tail
    int best = 0; uint32_t bestAge = UINT32_MAX;                                  // steal oldest
    for (int i = 0; i < m_numVoices; i++) if (m_vAge[i] < bestAge) { bestAge = m_vAge[i]; best = i; }
    return best;
}

void Sf2GmEngine::noteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (!m_voice || channel < 1 || channel > 16) return;
    if (velocity == 0) { noteOff(channel, note); return; }
    int idx = resolveInstrument(channel, note);
    AudioSynthWavetable::instrument_data* data = ensureInstrument(idx);
    if (!data) return;

    // scaleTuning: compress the note's distance from root when the instrument asks for
    // it (pitched percussion / SFX use 50). scale==100 -> identical to playNote(note).
    int scale = (idx >= 0 && idx < m_instCount) ? m_instScale[idx] : 100;
    int root  = (idx >= 0 && idx < m_instCount) ? m_instRoot[idx]  : 60;
    float bend = m_bend[channel];
    float effNote = root + ((float)note + bend - root) * (scale / 100.0f);

    int v = pickVoice();
    m_voice[v].setInstrument(*data);
    m_voice[v].amplitude(m_gain);
    if (scale == 100 && bend == 0.0f) m_voice[v].playNote(note, velocity);
    else m_voice[v].playNoteFreq(note, noteBendToFreq(effNote), velocity);
    m_vNote[v] = (int8_t)note; m_vChan[v] = channel; m_vAge[v] = ++m_ageCounter;
    m_vBaseNote[v] = (float)note; m_vInst[v] = data;
    m_vScale[v] = (int16_t)scale; m_vRoot[v] = (int16_t)root;
}

void Sf2GmEngine::noteOff(uint8_t channel, uint8_t note) {
    if (!m_voice) return;
    for (int i = 0; i < m_numVoices; i++)
        if (m_vNote[i] == (int8_t)note && m_vChan[i] == channel) {
            m_voice[i].stop();     // begin release; tail keeps rendering, pins its instrument
            m_vNote[i] = -1;
        }
}

void Sf2GmEngine::programChange(uint8_t channel, uint8_t program) {
    if (channel < 1 || channel > 16 || program > 127) return;
    m_program[channel] = program;   // resolved lazily on next noteOn
}

void Sf2GmEngine::pitchBend(uint8_t channel, float semitones) {
    if (channel < 1 || channel > 16) return;
    if (channel == kDrumChannel) return;      // drums don't pitch-bend
    m_bend[channel] = semitones;
    if (!m_voice) return;
    for (int i = 0; i < m_numVoices; i++)
        if (m_vChan[i] == channel && m_voice[i].isPlaying()) {
            float eff = m_vRoot[i] + (m_vBaseNote[i] + semitones - m_vRoot[i]) * (m_vScale[i] / 100.0f);
            m_voice[i].setFrequency(noteBendToFreq(eff));
        }
}

void Sf2GmEngine::controlChange(uint8_t channel, uint8_t cc, uint8_t /*v*/) {
    if (cc == 120 || cc == 123) {   // All Sound Off / All Notes Off (this channel)
        if (!m_voice) return;
        for (int i = 0; i < m_numVoices; i++)
            if (m_vChan[i] == channel) { m_voice[i].stop(); m_vNote[i] = -1; }
    }
    // sustain / other CC: TODO
}

void Sf2GmEngine::allNotesOff() {
    if (!m_voice) return;
    for (int i = 0; i < m_numVoices; i++) { m_voice[i].stop(); m_vNote[i] = -1; }
}

void Sf2GmEngine::setGain(float g) {
    m_gain = (g < 0.0f) ? 0.0f : (g > 1.0f ? 1.0f : g);
    if (!m_voice) return;
    for (int i = 0; i < m_numVoices; i++) m_voice[i].amplitude(m_gain);
}

int Sf2GmEngine::activeVoices() const {
    if (!m_voice) return 0;
    int n = 0;
    for (int i = 0; i < m_numVoices; i++) if (m_voice[i].isPlaying()) n++;
    return n;
}

// Standard General MIDI program names (0..127).
static const char* const kGmNames[128] = {
    "Acoustic Grand Piano","Bright Acoustic Piano","Electric Grand Piano","Honky-tonk Piano",
    "Electric Piano 1","Electric Piano 2","Harpsichord","Clavinet",
    "Celesta","Glockenspiel","Music Box","Vibraphone","Marimba","Xylophone","Tubular Bells","Dulcimer",
    "Drawbar Organ","Percussive Organ","Rock Organ","Church Organ","Reed Organ",
    "Accordion","Harmonica","Tango Accordion",
    "Acoustic Guitar (nylon)","Acoustic Guitar (steel)","Electric Guitar (jazz)","Electric Guitar (clean)",
    "Electric Guitar (muted)","Overdriven Guitar","Distortion Guitar","Guitar Harmonics",
    "Acoustic Bass","Electric Bass (finger)","Electric Bass (pick)","Fretless Bass",
    "Slap Bass 1","Slap Bass 2","Synth Bass 1","Synth Bass 2",
    "Violin","Viola","Cello","Contrabass","Tremolo Strings","Pizzicato Strings","Orchestral Harp","Timpani",
    "String Ensemble 1","String Ensemble 2","Synth Strings 1","Synth Strings 2",
    "Choir Aahs","Voice Oohs","Synth Voice","Orchestra Hit",
    "Trumpet","Trombone","Tuba","Muted Trumpet","French Horn","Brass Section","Synth Brass 1","Synth Brass 2",
    "Soprano Sax","Alto Sax","Tenor Sax","Baritone Sax","Oboe","English Horn","Bassoon","Clarinet",
    "Piccolo","Flute","Recorder","Pan Flute","Blown Bottle","Shakuhachi","Whistle","Ocarina",
    "Lead 1 (square)","Lead 2 (sawtooth)","Lead 3 (calliope)","Lead 4 (chiff)",
    "Lead 5 (charang)","Lead 6 (voice)","Lead 7 (fifths)","Lead 8 (bass + lead)",
    "Pad 1 (new age)","Pad 2 (warm)","Pad 3 (polysynth)","Pad 4 (choir)",
    "Pad 5 (bowed)","Pad 6 (metallic)","Pad 7 (halo)","Pad 8 (sweep)",
    "FX 1 (rain)","FX 2 (soundtrack)","FX 3 (crystal)","FX 4 (atmosphere)",
    "FX 5 (brightness)","FX 6 (goblins)","FX 7 (echoes)","FX 8 (sci-fi)",
    "Sitar","Banjo","Shamisen","Koto","Kalimba","Bagpipe","Fiddle","Shanai",
    "Tinkle Bell","Agogo","Steel Drums","Woodblock","Taiko Drum","Melodic Tom","Synth Drum","Reverse Cymbal",
    "Guitar Fret Noise","Breath Noise","Seashore","Bird Tweet","Telephone Ring","Helicopter","Applause","Gunshot"
};

const char* Sf2GmEngine::melodicName(int gm) const {
    if (gm < 0 || gm > 127) return "";
    return kGmNames[gm];
}
