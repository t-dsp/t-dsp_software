// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.
//
// ITempoFollower — the interface a module implements to follow the master clock.
//
// The Conductor owns the tempo/transport; anything that must react to tempo or
// transport changes registers as a follower and gets called back. Every method
// has a no-op default, so a follower overrides only what it cares about.
//
// The two built-in follower kinds:
//   * scale followers  — a MidiFilePlayer that retimes itself (see PlayerFollower)
//   * tick  followers  — the arp, which instead consumes 24-PPQN ticks directly
//                        off the Conductor's Clock (Conductor::setTickHook). A
//                        tick follower usually needs NO ITempoFollower at all.
//
// See lib/TDspTempo/README.md for the per-module consumption recipes.

#pragma once

namespace tdsp {

class ITempoFollower {
public:
    virtual ~ITempoFollower() = default;

    // Master tempo changed (or the follower was just registered). Retime
    // yourself to `masterBpm`. Called once on addFollower() to seed state.
    virtual void onBpm(float masterBpm) { (void)masterBpm; }

    // Transport Start: realign to bar 1, beat 1. The Conductor has already
    // zeroed the shared Clock, so tick consumers share this downbeat.
    virtual void onStart() {}

    // Transport Stop.
    virtual void onStop() {}

    // Fired once per bar boundary (4/4 by default). Use for bar-quantized
    // actions — e.g. an arp that restarts its pattern on the downbeat, or a
    // looper that arms recording on the next bar.
    virtual void onBarEdge() {}
};

}  // namespace tdsp
