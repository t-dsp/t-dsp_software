// Track.h — one voice's whole stack as a single binding.
//
// A Track is a thin VIEW over the per-voice objects that already live at file scope in
// main.cpp (the Teensy-audio-adjacent statics don't move) plus the per-voice STATE moved in
// here, plus capability flags. The point: the parallel song*/song2* helper families collapse
// into ONE Track&-parameterized family that can never drift apart again (that drift is what
// produced the downbeat/timing bugs). See planning/tracks/DESIGN.md — this is Phase 1.
//
// Phase 1 keeps the audio graph and the wire protocol unchanged; drums + N slots are later phases.
#pragma once

#include <stdint.h>

// Which physical MIDI INPUT DEVICE a live event came from (Phase 3, Thread C). The box can
// have several inputs at once — DIN MIDI-in (Serial1), a USB-host controller (LinnStrument),
// Bluetooth-MIDI (via the ESP32), a MIDI-over-serial source — and each Track subscribes to the
// ones that should feed it. The MidiHub (main.cpp) tags every event with its source and fans it
// only to the Tracks whose subscription matches, so "which device feeds this synth" is a field
// read, never an audio-graph repatch. Values 1.. are used as bit positions in Track.liveSrcMask.
enum MidiSourceId : uint8_t { SrcNone = 0, SrcDin = 1, SrcUsbHost = 2, SrcBtMidi = 3, SrcSerial = 4, SrcCount = 5 };
static inline uint8_t srcBit(MidiSourceId s) { return (s == SrcNone) ? 0 : (uint8_t)(1u << s); }
static inline uint8_t srcMaskAllLocal() { return (uint8_t)(srcBit(SrcDin) | srcBit(SrcUsbHost)); }

namespace tdsp {
class  MidiFilePlayer;   // the song player (its own @SONG feed)
class  ArpFilter;        // the arp (bypassed = pass-through); may be null on a no-ARP2 build
class  MidiRouter;       // live-MIDI entry (voice 1 = g_router, voice 2 = the keyboard router)
class  PlayerFollower;   // retimes the player to the master BPM
class  MidiLooper;       // live-capture loop; may be null on a no-recorder build
class  MidiSink;         // the engine binding (g_synthSink / g_synthSinkB); never repointed
struct MidiFileEvent;    // one parsed MIDI event (the player's event stream)
}

// Which of the deliberately per-role behaviors this track performs. Default all-false =
// voice-2 behavior (touches no global mode/meter, bare prep). Voice 1 sets the first three; the
// drum track (Phase 2) sets the drum-* group. Adding a role = a flag here + a check at the caps site.
struct TrackCaps {
    bool ownsGlobalMode;   // applies the global MPE mode on start (applyMidiMode)          [voice 1]
    bool ownsMeter;        // owns the master meter — applyMeter() on start/stop/natural-end [voice 1]
    bool prepSpecial;      // prep spares ch10 while drums loop + resets the multitimbral audition trim [voice 1]
    bool splitGuarded;     // start is a no-op unless the pool split is enabled              [voice 2]
    // --- drum-track group (Phase 2); all false for the synth voices -------------------------------
    bool loopsSeamless;       // player.setLooping(true): a groove loops seamlessly (no re-arm, drum tails ring)
    bool ownsPatch;           // the track owns its patch (drum kit) -> ignore the file's program changes
    bool drumGated;           // start is a no-op unless the engine renders ch10 drums (drumEngineOk())
    bool appliesKit;          // prep applies the selected GM drum kit (program change on ch10)
    bool mutesSongDrums;      // while playing, mute the melodic song's own ch10 so the groove IS the beat
    bool tempoSourceWhenIdle; // starting on an idle transport snaps the master BPM to this content's native BPM
};

struct Track {
    tdsp::MidiFilePlayer *player;
    tdsp::ArpFilter      *arp;       // may be null (no TDSP_ARP2) -> player routes straight to sink
    tdsp::MidiRouter     *router;
    tdsp::PlayerFollower *follow;
    tdsp::MidiLooper     *looper;    // may be null (no TDSP_RECORDER)
    tdsp::MidiSink       *sink;      // the engine binding; fixed for the life of the build
    tdsp::MidiFileEvent  *buf;       // this track's event buffer (SD parse target)
    uint32_t              bufCap;
    uint16_t              chMask;     // channels the player emits: kMaskNoDrums (melodic voices) / 1<<9 (drum ch10)
    void (*setLevel)(int pct);       // backend hook: voice 1 -> synthSetSongVol, voice 2 -> synthSetVoice2Vol, drum -> vol%
    const char *tag;                 // log/serial tag: "song" (voice 1) / "song2" (voice 2). Voice 1's
                                     // "[song] <n> bpm" print is the one the app parses (Reset->song bpm).
    TrackCaps  caps;

    // Preload stash (trackPreload -> trackFire): the loaded event stream + how to launch it, so the
    // bar-edge fire is play()+sync with NO SD parse on the downbeat (this is voice 2's preload/fire
    // pattern, now shared by voice 1 too — the P1.4 improvement). Internal to the start path; nothing
    // outside trackPreload/trackFire reads it (it replaced the old g_song2Pre* globals).
    const tdsp::MidiFileEvent *preEv;   // stream to play (this track's buf, or a baked flash array)
    uint32_t preCount;                  // events in preEv
    double   preLoopBeats;              // exact loop length from the SD parse (0 => derive from ms)
    bool     preForceMode;              // fire always sets the global MIDI/MPE mode (a test song) — caps.ownsGlobalMode-gated
    bool     preMpe;                    // the mode to apply/force on fire

    // Per-voice STATE — POINTERS to the existing file-scope globals. Phase 1 BINDS the state (does
    // not move it), so the unified helpers read/write it through the track while every other reader
    // (@STATE, applyTempos, the position feed) keeps using the globals directly. A later cleanup can
    // fold these into the struct. Each points at g_song{,2}<field>.
    char    *name;          // g_curSong{,2}Name
    char    *arg;           // g_curSong{,2}Arg
    bool    *loop;          // g_loop / g_song2Loop
    bool    *wasPlaying;    // g_song{,2}WasPlaying
    float   *bpm;           // g_song{,2}Bpm
    uint8_t *bpb;           // g_song{,2}Bpb
    double  *loopBeats;     // g_song{,2}LoopBeats
    bool    *launchPending; // g_song{,2}LaunchPending

    // Live-MIDI SUBSCRIPTION (Phase 3, Thread C). Which input device(s) feed this track's live
    // router, and an optional channel filter. Switching = one field write (the MidiHub reads it
    // per event) — NO AudioConnection change, no loadVoice, no clock/player touch, so a synth can
    // change which keyboard it listens to mid-performance with zero dropout. liveSrcMask is a
    // bitmask over MidiSourceId (0 = no live input; srcMaskAllLocal() = DIN + USB-host). srcChMask
    // filters channels: 0 = all 16, else a one-hot 1<<(ch-1). The player's own feed is unaffected —
    // the synth always hears {its song} + {subscribed live input}. See main.cpp `midihub`.
    uint8_t  liveSrcMask;   // bits: (1<<SrcDin)|(1<<SrcUsbHost)|… ; 0 = none
    uint16_t srcChMask;     // 0 = all channels; else 1<<(ch-1)
};
