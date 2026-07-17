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

// The per-voice runtime state that used to live in the g_song*/g_song2* globals, now owned per track.
struct TrackState {
    char   name[64];       // display name of the loaded song  (was g_curSong{,2}Name)
    char   arg[100];       // replay arg for loop/restart       (was g_curSong{,2}Arg)
    bool   loop;           // end-of-song = repeat              (was g_loop / g_song2Loop)
    bool   wasPlaying;     // for the loop-restart edge         (was g_song{,2}WasPlaying)
    float  bpm;            // song native tempo                 (was g_song{,2}Bpm)
    uint8_t bpb;           // song beats/bar                    (was g_song{,2}Bpb)
    double loopBeats;      // exact synced loop length          (was g_song{,2}LoopBeats)
    bool   launchPending;  // armed for the next bar edge       (was g_song{,2}LaunchPending)
    // Pre-loaded launch (parse off the beat, fire instant on the downbeat) — was g_song2Pre*.
    const tdsp::MidiFileEvent *preEv;
    uint32_t preCount;
    double   preLoopBeats;
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
    TrackState st;
    TrackCaps  caps;
};
