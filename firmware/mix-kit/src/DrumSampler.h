// DrumSampler.h — no-PSRAM SD-STREAMING drum kit (build flag: TDSP_DRUM_SD).
//
// The streaming sibling of DrumVoice.h (OPLL rhythm) / DrumPlaits.h (synth models) /
// DrumTsf.h (resident SF2). Instead of a synth or a PSRAM-resident font, this plays
// one WAV file per ch10 GM note STRAIGHT off BUILTIN_SDCARD — 8 polyphonic voices of
// newdigate/teensy-variable-playback (AudioPlaySdResmp, already in lib/), each streaming
// its file on demand. That fits the no-PSRAM local board (SN 18402920, COM4), which
// can't hold a TSF drum font, so it can play the sampled "…From Mars" kits. A kit is a
// subfolder of /drums; the leading integer of each filename is the GM note it fires.
//
//   g_drumPlayer (ch10) -> g_drumSamplerSink -> 8x AudioPlaySdResmp (int16 stereo)
//       -> AudioMixer4 chain -> AudioConvert_I16toF32 L/R -> outL/outR slot 2
//
// One-shots: a struck drum rings its whole sample; ch10 note-offs are DROPPED (no
// envelope), EXCEPT the 42/44/46 hi-hat CHOKE class (a new hat stops a ringing hat).
// MUTUALLY EXCLUSIVE with TDSP_DRUM_VOICE / TDSP_DRUM_TSF / TDSP_DRUM_PLAITS (all own the
// drum mix slot) — main.cpp #errors if more than one is defined. Included by main.cpp
// AFTER outL/outR + g_drumPlayer + the melodic backend exist.
//
// Content layout (tools/build_mars_kits.py, copied to the card via a card reader):
//   /drums/<kit>/<gmnote>-<name>.wav      e.g. /drums/808_clean/36-Bass_Drum.wav
//   WAV = 48 kHz / 16-bit / stereo (matches AUDIO_SAMPLE_RATE_EXACT; the lib is int16-only).
#pragma once
#ifdef TDSP_DRUM_SD
#include <Audio.h>
#include <SD.h>
#include <MidiSink.h>
// Two interchangeable streaming backends behind ONE audio graph + one sink:
//   DEFAULT — teensy-variable-playback's AudioPlaySdResmp (proven, the working build).
//   TDSP_DRUM_DFD — the dfd Direct-From-Disk engine (lib/dfd): a resident head + streamed body
//     ring per voice, so the trigger/restart path never waits on SD (planning/dfd-sampler).
// Both are int16-stereo AudioStream nodes, so the mixer tree + connections below are identical;
// only the per-note trigger + voice allocation differ (see the TDSP_DRUM_DFD branches).
#ifdef TDSP_DRUM_DFD
#include <dfd/backends/teensy/AudioPlayDfd.h>
#include <dfd/backends/teensy/SdSource.h>
#include <dfd/backends/teensy/PsramAllocator.h>
using DrumSdVoice = dfd::AudioPlayDfd;
// Head/ring/history sizing in interleaved int16 SAMPLES (a stereo frame = 2 samples). H MUST
// exceed the worst-case SD stall (DESIGN §1/§7): this board's @DRUMSTRESS showed 0 underruns at
// the old 85 ms ring, so the empirical worst stall is < 85 ms. The generous 64 KB/voice default
// below is for a PSRAM build; the no-PSRAM env (teensy41_dexed_pool_nobt_drumsd_dfd) OVERRIDES
// these smaller to fit the ~144 KB OCRAM heap — see platformio.ini for the budget math.
#ifndef TDSP_DFD_HEAD_SAMPLES
#define TDSP_DFD_HEAD_SAMPLES 32768   // 64 KB/voice head (~340 ms stereo) — PSRAM-blessed default
#endif
#ifndef TDSP_DFD_RING_SAMPLES
#define TDSP_DFD_RING_SAMPLES 4096    // 8 KB/voice body ring (~43 ms stereo)
#endif
#ifndef TDSP_DFD_HISTORY_SAMPLES
#define TDSP_DFD_HISTORY_SAMPLES 0    // no backward look-behind yet (stutter is phase 3)
#endif
#ifdef TDSP_TVP_UNDERRUN_COUNT
// DFD builds don't include teensy-variable-playback (which declares this), so forward-declare the
// underrun tally here; the definition is at the file end (guarded the same way).
namespace newdigate { extern volatile uint32_t g_tvpUnderruns; }
#endif
#else
#include <TeensyVariablePlayback.h>   // AudioPlaySdResmp (LDF auto-finds lib/teensy-variable-playback)
using DrumSdVoice = AudioPlaySdResmp;
#endif

// The output mix slot the drum kit lands on. Default 2 (S/PDIF-in dropped via TDSP_NO_SPDIF_IN
// frees it), matching the other drum voices. A build that parks another engine on slot 2 can
// override with -D TDSP_DRUM_SLOT=<n>.
#ifndef TDSP_DRUM_SLOT
#define TDSP_DRUM_SLOT 2
#endif

#ifndef TDSP_DRUM_SD_VOICES
#define TDSP_DRUM_SD_VOICES 8          // 8 simultaneous one-shots (kick+snare+hats+toms+cymbals overlap)
#endif

// The audio graph (the 2x4 mixer tree below) is HARDWIRED to 8 players — the AudioConnections
// reference g_drumSdV[0..7] — so this object array is ALWAYS 8. TDSP_DRUM_SD_VOICES caps only how
// many voices are ever TRIGGERED (heap ~28 KB per sounding voice), NOT the object count: shrinking
// this array below 8 would bind the voice-4..7 connections to out-of-bounds objects and the audio
// ISR would walk corrupt connections -> silent reset. Keep them decoupled.
static constexpr int kDrumSdMaxVoices = 8;
DrumSdVoice           g_drumSdV[kDrumSdMaxVoices];   // AudioPlaySdResmp (default) or dfd::AudioPlayDfd
// Two-stage int16 mixer tree per channel: mix[0]/mix[1] sum voices 0-3 / 4-7, mix[2] sums the two.
AudioMixer4           g_drumSdMixL[3], g_drumSdMixR[3];
AudioConvert_I16toF32 g_drumSdToF32L, g_drumSdToF32R;

// Voices 0-3 -> stage-1 mixer 0; voices 4-7 -> stage-1 mixer 1 (L on out 0, R on out 1).
static AudioConnection c_dsv0L(g_drumSdV[0], 0, g_drumSdMixL[0], 0), c_dsv0R(g_drumSdV[0], 1, g_drumSdMixR[0], 0);
static AudioConnection c_dsv1L(g_drumSdV[1], 0, g_drumSdMixL[0], 1), c_dsv1R(g_drumSdV[1], 1, g_drumSdMixR[0], 1);
static AudioConnection c_dsv2L(g_drumSdV[2], 0, g_drumSdMixL[0], 2), c_dsv2R(g_drumSdV[2], 1, g_drumSdMixR[0], 2);
static AudioConnection c_dsv3L(g_drumSdV[3], 0, g_drumSdMixL[0], 3), c_dsv3R(g_drumSdV[3], 1, g_drumSdMixR[0], 3);
static AudioConnection c_dsv4L(g_drumSdV[4], 0, g_drumSdMixL[1], 0), c_dsv4R(g_drumSdV[4], 1, g_drumSdMixR[1], 0);
static AudioConnection c_dsv5L(g_drumSdV[5], 0, g_drumSdMixL[1], 1), c_dsv5R(g_drumSdV[5], 1, g_drumSdMixR[1], 1);
static AudioConnection c_dsv6L(g_drumSdV[6], 0, g_drumSdMixL[1], 2), c_dsv6R(g_drumSdV[6], 1, g_drumSdMixR[1], 2);
static AudioConnection c_dsv7L(g_drumSdV[7], 0, g_drumSdMixL[1], 3), c_dsv7R(g_drumSdV[7], 1, g_drumSdMixR[1], 3);
// Stage-2 sum -> I16toF32 -> the free drum mix slot.
static AudioConnection     c_dsL_s0(g_drumSdMixL[0], 0, g_drumSdMixL[2], 0), c_dsL_s1(g_drumSdMixL[1], 0, g_drumSdMixL[2], 1);
static AudioConnection     c_dsR_s0(g_drumSdMixR[0], 0, g_drumSdMixR[2], 0), c_dsR_s1(g_drumSdMixR[1], 0, g_drumSdMixR[2], 1);
static AudioConnection     c_dsL_cv(g_drumSdMixL[2], 0, g_drumSdToF32L, 0), c_dsR_cv(g_drumSdMixR[2], 0, g_drumSdToF32R, 0);
static AudioConnection_F32 c_ds_outL(g_drumSdToF32L, 0, outL, TDSP_DRUM_SLOT);
static AudioConnection_F32 c_ds_outR(g_drumSdToF32R, 0, outR, TDSP_DRUM_SLOT);

// The ch10 groove/live-drum sink + the kit/path model. Each note-ON triggers a one-shot on a
// picked voice; note-OFF is ignored (drums ring out) except the hi-hat choke. A "kit" is a
// subfolder of /drums; setKit() scans it into path[note] by the leading integer of each WAV.
class DrumSamplerSink : public tdsp::MidiSink {
public:
    // TRIGGERED voices — clamped to the 8 wired objects. Fewer = less peak heap; the extra wired
    // players (kVoices..7) simply never get a playWav() so they stay silent.
    static constexpr int kVoices     = (TDSP_DRUM_SD_VOICES < kDrumSdMaxVoices) ? TDSP_DRUM_SD_VOICES : kDrumSdMaxVoices;
    static constexpr int kNumNotes   = 128;
    static constexpr int kPathLen    = 80;   // "/drums/<folder>/<n>-<name>.wav"
    static constexpr int kMaxKits    = 128;  // the full "…From Mars" library is ~92 kit folders
    static constexpr int kKitNameLen = 25;   // <=24 chars + NUL

    DrumSamplerSink() { for (int v = 0; v < kVoices; ++v) _voiceNote[v] = -1; }

    // ---- MidiSink -----------------------------------------------------------
    // A struck drum plays its whole sample; only trigger on a real note-ON (vel > 0).
    void onNoteOn(uint8_t /*ch*/, uint8_t note, uint8_t vel) override {
        if (vel == 0) return;                       // note-off form -> ignore (one-shots ring out)
        if (note >= kNumNotes) return;
        const char *path = _path[note];
        if (!path[0]) return;                       // no sample mapped for this GM note
        // Hi-hat choke: closed(42)/pedal(44)/open(46) are one exclusive class — a new hat SOFT-
        // releases any ringing hat (a short fade, not a hard cut, so open->closed doesn't snap).
        if (isHat(note))
            for (int v = 0; v < kVoices; ++v)
                if (isHat(_voiceNote[v]) && g_drumSdV[v].isPlaying()) voiceSoftStop(g_drumSdV[v]);

        // Pick a FRESH voice for the new hit so a retrigger never hard-cuts the ringing one:
        // prefer a truly idle voice; only steal (least-remaining tail) when every voice is busy.
        int v = idleVoice();
        if (v < 0) v = pickStealVoice();
        // Declick the OUTGOING same-note hit (retrigger): fade its tail out on ITS OWN voice while
        // the new hit starts on `v`. The ~5 ms overlap is inaudible; the alternative is the snap.
        for (int w = 0; w < kVoices; ++w)
            if (w != v && _voiceNote[w] == (int8_t)note && g_drumSdV[w].isPlaying()) voiceSoftStop(g_drumSdV[w]);

        DrumSdVoice &pl = g_drumSdV[v];
        pl.stop();                                  // clean any tail on a stolen voice
        // Trigger from the PERSISTENT per-note source (opened once in setKit) — no per-hit SD.open()
        // FAT lookup. One voice per note (pickVoice) => one reader per handle. (@DRUMSTRESS's
        // synthetic burst over-reports underruns — its busy-wait doesn't refill like a real groove's
        // loop(), so don't trust that number; watch @DRUMJIT under a real groove instead.)
#ifdef TDSP_DRUM_DFD
        // DFD: the resident head is loaded here (loop() context, off the audio ISR) so the AUDIO
        // path never touches SD; the body streams behind it via serviceVoices(). One-shot, rate 1.0.
        if (!_noteSrc[note].ok() || !pl.play(_noteSrc[note], /*loop=*/false)) { _voiceNote[v] = -1; return; }
#else
        pl.enableInterpolation(false);              // one-shots play at native rate — no resample
        if (!_noteFile[note] || !pl.playWavHandle(_noteFile[note])) { _voiceNote[v] = -1; return; }
        pl.setPlaybackRate(1.0f);
#endif
        _voiceNote[v]    = (int8_t)note;
        _voiceStartMs[v] = millis();
    }
    // Drop note-offs (one-shots ring out); the hi-hat choke on note-ON handles hat muting.
    void onNoteOff(uint8_t, uint8_t, uint8_t) override {}
    // ch10 program change = kit select (prog = folder index; see drumKitProg()).
    void onProgramChange(uint8_t ch, uint8_t prog) override { if (ch == 10) setKit(prog); }
    void onAllNotesOff(uint8_t) override { for (int v = 0; v < kVoices; ++v) { g_drumSdV[v].stop(); _voiceNote[v] = -1; } }

    // ---- kits ---------------------------------------------------------------
    // List the subfolders of /drums into _kitFolders[]. Cold path (boot / @DRUMKIT).
    void scanKits() {
        _numKits = 0;
        File d = SD.open("/drums");
        if (!d) return;
        if (!d.isDirectory()) { d.close(); return; }
        for (File f = d.openNextFile(); f && _numKits < kMaxKits; f = d.openNextFile()) {
            if (f.isDirectory()) {
                const char *nm = f.name();
                if (nm && nm[0] && nm[0] != '.') {
                    strncpy(_kitFolders[_numKits], nm, kKitNameLen - 1);
                    _kitFolders[_numKits][kKitNameLen - 1] = 0;
                    ++_numKits;
                }
            }
            f.close();
        }
        d.close();
        Serial.printf("[drumsd] %d kit folder(s) under /drums\n", _numKits);
    }

    // Load kit `i`: clear path[] and scan /drums/<folder> for <n>-*.wav, mapping each to GM note n.
    void setKit(int i) {
        if (_numKits <= 0) return;
        if (i < 0) i = 0;
        if (i >= _numKits) i = _numKits - 1;
        _curKit = i;
        onAllNotesOff(0);                           // stop any ringing voice from the old kit
        for (int n = 0; n < kNumNotes; ++n) {       // drop the old kit's paths + persistent handles
            _path[n][0] = 0;
            if (_noteFile[n]) _noteFile[n].close();
        }

        char dirPath[kPathLen];
        snprintf(dirPath, sizeof dirPath, "/drums/%s", _kitFolders[i]);
        File d = SD.open(dirPath);
        if (!d || !d.isDirectory()) { if (d) d.close(); Serial.printf("[drumsd] kit dir missing: %s\n", dirPath); return; }
        int mapped = 0;
        for (File f = d.openNextFile(); f; f = d.openNextFile()) {
            const char *nm = f.name();
            if (!f.isDirectory() && nm && nm[0] != '.' && endsWithWav(nm)) {
                int note = leadingInt(nm);
                if (note >= 0 && note < kNumNotes && !_path[note][0]) {   // first file for a note wins
                    snprintf(_path[note], kPathLen, "%s/%s", dirPath, nm);
                    ++mapped;
                }
            }
            f.close();
        }
        d.close();
        // Open a PERSISTENT handle per mapped note so triggers stream from it (seek 0) with NO per-hit
        // SD.open() FAT lookup — the residual drum-skip source. ~a dozen handles per kit. The DFD
        // backend also parses each WAV header ONCE here (into the per-note SdSource) so a trigger
        // only seeks+reads — never re-parses.
        for (int n = 0; n < kNumNotes; ++n)
            if (_path[n][0]) {
                _noteFile[n] = SD.open(_path[n]);
#ifdef TDSP_DRUM_DFD
                if (_noteFile[n]) _noteSrc[n].open(_noteFile[n]);   // &_noteFile[n] is a stable array slot
#endif
            }
        Serial.printf("[drumsd] kit \"%s\": %d note sample(s)\n", _kitFolders[i], mapped);
    }

    int         numKits()    const { return _numKits; }
    const char *kitName(int i) const { return (i >= 0 && i < _numKits) ? _kitFolders[i] : ""; }
    int         currentKit() const { return _curKit; }

    // Reclaim voices whose SD stream has ended (so pickVoice's oldest-tracking stays honest). Cheap;
    // call once per main-loop pass.
    void pollVoices() {
#ifdef TDSP_DRUM_DFD
        // DFD stream pump: refill every sounding voice's body ring from loop() context (the SOLE
        // caller of SD reads on the audio side). Runs every loop() pass, well ahead of the ring
        // drain. Also mirror the aggregate underrun tally into g_tvpUnderruns for @DRUMSTRESS.
        serviceVoices();
#endif
        for (int v = 0; v < kVoices; ++v)
            if (_voiceNote[v] >= 0 && !g_drumSdV[v].isPlaying()) _voiceNote[v] = -1;
    }

#ifdef TDSP_DRUM_DFD
    void serviceVoices() {
        for (int v = 0; v < kVoices; ++v)
            if (g_drumSdV[v].isPlaying()) g_drumSdV[v].service();
#ifdef TDSP_TVP_UNDERRUN_COUNT
        uint32_t sum = 0;
        for (int v = 0; v < kVoices; ++v) sum += g_drumSdV[v].underruns();
        newdigate::g_tvpUnderruns = sum;           // defined at file end; matches @DRUMSTRESS's read
#endif
    }
#endif

    // --- diagnostics (SD polyphony stress test; see @DRUMSTRESS) --------------
    // Fire up to n distinct mapped notes AT ONCE (worst-case simultaneous streaming). Returns the
    // count actually triggered. Used to measure whether the no-PSRAM board can refill n voices' ring
    // buffers fast enough (g_tvpUnderruns climbs on a starve) — i.e. "is SD fast enough for n hits".
    int triggerN(int n) {
        if (n > kVoices) n = kVoices;
        int fired = 0;
        for (int note = 0; note < kNumNotes && fired < n; ++note)
            if (_path[note][0]) { onNoteOn(10, (uint8_t)note, 110); ++fired; }
        return fired;
    }
    int voicesPlaying() const {
        int c = 0; for (int v = 0; v < kVoices; ++v) if (g_drumSdV[v].isPlaying()) ++c; return c;
    }

private:
    static bool isHat(int note) { return note == 42 || note == 44 || note == 46; }
    static bool endsWithWav(const char *nm) {
        size_t L = strlen(nm);
        if (L < 4) return false;
        const char *e = nm + L - 4;
        return e[0] == '.' && (e[1] == 'w' || e[1] == 'W') && (e[2] == 'a' || e[2] == 'A') && (e[3] == 'v' || e[3] == 'V');
    }
    // Leading integer of a filename ("36-Bass_Drum.wav" -> 36). -1 if it doesn't start with a digit.
    static int leadingInt(const char *nm) {
        if (!nm || !isdigit((unsigned char)nm[0])) return -1;
        int n = 0;
        for (const char *p = nm; *p && isdigit((unsigned char)*p); ++p) n = n * 10 + (*p - '0');
        return n;
    }
    // Soft-stop (declick fade-out) on the DFD engine; a plain hard stop on the legacy backend
    // (AudioPlaySdResmp has no envelope). Keeps both builds compiling from one call site.
    static inline void voiceSoftStop(DrumSdVoice &pl) {
#ifdef TDSP_DRUM_DFD
        pl.softStop();
#else
        pl.stop();
#endif
    }

    // First idle (finished) voice, or -1 if all are sounding.
    int idleVoice() {
        for (int v = 0; v < kVoices; ++v) if (!g_drumSdV[v].isPlaying()) return v;
        return -1;
    }
    // All voices busy -> steal the one CLOSEST TO ITS NATURAL END (least remaining), so we clip the
    // least-audible tail (a crash/ride mid-decay is spared). Retrigger/choke declick is handled by
    // the caller via voiceSoftStop() on the outgoing voice; this is the last-resort hard steal.
    int pickStealVoice() {
        int best = 0; uint32_t bestRem = 0xFFFFFFFFu;
        for (int v = 0; v < kVoices; ++v) {
            const uint32_t len = g_drumSdV[v].lengthMillis(), pos = g_drumSdV[v].positionMillis();
            const uint32_t rem = len > pos ? len - pos : 0;
            if (rem < bestRem) { bestRem = rem; best = v; }
        }
        return best;
    }

    int8_t   _voiceNote[kVoices];
    uint32_t _voiceStartMs[kVoices] = {};
    char     _path[kNumNotes][kPathLen] = {};
    File     _noteFile[kNumNotes];    // persistent per-note open handles (setKit opens; triggers seek 0)
#ifdef TDSP_DRUM_DFD
    dfd::SdSource _noteSrc[kNumNotes]; // per-note DFD source (WAV header parsed once in setKit; wraps _noteFile)
#endif
    char     _kitFolders[kMaxKits][kKitNameLen] = {};
    int      _numKits = 0;
    int      _curKit  = 0;
};
DrumSamplerSink g_drumSamplerSink;

#ifdef TDSP_TVP_UNDERRUN_COUNT
// SD-streaming underrun tally, ++'d inside teensy-variable-playback's IndexableFile when a voice's
// buffer wasn't refilled in time (that sample plays as silence). @DRUMSTRESS resets + reads it.
// Lives in newdigate:: to match the extern in IndexableFile.h (which is inside that namespace).
namespace newdigate { volatile uint32_t g_tvpUnderruns = 0; }
#endif

// Bring up the SD drum kit: mixer gains 1.0, open the drum mix slot, scan /drums + load kit 0.
// Returns true only if at least one kit folder exists (mirrors drumTsfBegin()'s "font loaded?"
// contract, so setup() marks drums available exactly when there's something to play).
FLASHMEM static bool drumSamplerBegin() {
    for (int s = 0; s < 3; ++s)
        for (int in = 0; in < 4; ++in) { g_drumSdMixL[s].gain(in, 1.0f); g_drumSdMixR[s].gain(in, 1.0f); }
    outL.gain(TDSP_DRUM_SLOT, 0.62f); outR.gain(TDSP_DRUM_SLOT, 0.62f);   // drum bus make-up (mirrors the synth slot / TSF)
    extern bool g_sdReady;
    if (!g_sdReady) { Serial.println("[drumsd] no SD card -> drums idle (need /drums/<kit>/*.wav)"); return false; }
#ifdef TDSP_DRUM_DFD
    // Allocate each TRIGGERED voice's head + body ring ONCE, off the audio path (PSRAM if present,
    // else the OCRAM heap). Only kVoices allocate (objects kVoices..7 stay silent), so the heap
    // cost is kVoices*(head+ring). See platformio.ini for the no-PSRAM budget.
    static dfd::PsramAllocator s_dfdAlloc;
    const dfd::RegionConfig dfdCfg{ (uint32_t)TDSP_DFD_HEAD_SAMPLES,
                                    (uint32_t)TDSP_DFD_RING_SAMPLES,
                                    (uint32_t)TDSP_DFD_HISTORY_SAMPLES };
    int dfdOk = 0;
    for (int v = 0; v < DrumSamplerSink::kVoices; ++v)
        if (g_drumSdV[v].begin(s_dfdAlloc, dfdCfg)) ++dfdOk;
    Serial.printf("[drumsd] DFD backend: %d/%d voice(s) allocated (head %lu + ring %lu samples, %s)\n",
                  dfdOk, (int)DrumSamplerSink::kVoices, (unsigned long)TDSP_DFD_HEAD_SAMPLES,
                  (unsigned long)TDSP_DFD_RING_SAMPLES, s_dfdAlloc.hasPsram() ? "PSRAM" : "heap");
    if (dfdOk == 0) { Serial.println("[drumsd] DFD: no voices allocated (heap exhausted?) -> drums idle"); return false; }
#endif
    g_drumSamplerSink.scanKits();
    if (g_drumSamplerSink.numKits() == 0) { Serial.println("[drumsd] no /drums/<kit> folders -> drums idle"); return false; }
    g_drumSamplerSink.setKit(0);
    Serial.printf("[drumsd] %d-voice SD drum kit ready (ch10) -> mix slot %d, %d kit(s)\n",
                  TDSP_DRUM_SD_VOICES, TDSP_DRUM_SLOT, g_drumSamplerSink.numKits());
    return true;
}
#endif  // TDSP_DRUM_SD
