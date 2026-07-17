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

namespace tdsp {
class  MidiFilePlayer;   // the song player (its own @SONG feed)
class  ArpFilter;        // the arp (bypassed = pass-through); may be null on a no-ARP2 build
class  MidiRouter;       // live-MIDI entry (voice 1 = g_router, voice 2 = the keyboard router)
class  PlayerFollower;   // retimes the player to the master BPM
class  MidiLooper;       // live-capture loop; may be null on a no-recorder build
class  MidiSink;         // the engine binding (g_synthSink / g_synthSinkB); never repointed
struct MidiFileEvent;    // one parsed MIDI event (the player's event stream)
}

// Which of the deliberately voice-1-ONLY behaviors this track performs. Default all-false =
// today's voice-2 behavior (touches no global mode/meter, bare prep). Voice 1 sets them true.
struct TrackCaps {
    bool ownsGlobalMode;   // applies the global MPE mode on start (applyMidiMode)
    bool ownsMeter;        // owns the master meter — applyMeter() on start/stop/natural-end
    bool prepSpecial;      // prep spares ch10 while drums loop + resets the multitimbral audition trim
    bool splitGuarded;     // start is a no-op unless the pool split is enabled (voice 2 on a split build)
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
    void (*setLevel)(int pct);       // backend hook: voice 1 -> synthSetSongVol, voice 2 -> synthSetVoice2Vol
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
};
