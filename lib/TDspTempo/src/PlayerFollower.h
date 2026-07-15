// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.
//
// PlayerFollower — makes a tdsp::MidiFilePlayer follow the master clock.
//
// A MidiFilePlayer times itself: it advances by real elapsed ms scaled by
// setTempoScale(). It is NOT tick-driven. So to make it follow the Conductor's
// master BPM, we retime it — scale = masterBpm / nativeBpm — whenever either
// the master tempo or the file's native tempo changes.
//
//   * nativeBpm = the tempo the file was authored at (SD .mid = real tempo from
//     the tempo meta; a baked/estimate stream = its best-known BPM).
//   * trim      = an optional per-player fine adjustment in percent (100 =
//     exactly the master BPM). Used for the drum-groove "speed" control so a
//     groove can sit slightly ahead/behind the beat without moving the master.
//
// Non-invasive: it holds a reference to an existing player and only calls the
// player's public setTempoScale(). No change to MidiFilePlayer.

#pragma once

#include <MidiFilePlayer.h>

#include "TempoFollower.h"

namespace tdsp {

class PlayerFollower : public ITempoFollower {
public:
    explicit PlayerFollower(MidiFilePlayer &player) : _p(player) {}

    // The file's authored tempo. Call this on every song/groove load.
    void  setNativeBpm(float bpm) { _native = (bpm > 1.0f) ? bpm : 1.0f; apply(); }
    float nativeBpm() const       { return _native; }

    // Fine trim in percent (100 = exactly master BPM). Optional; defaults off.
    void  setTrim(float pct) { _trim = (pct > 1.0f ? pct : 1.0f) * 0.01f; apply(); }
    float trimPct() const    { return _trim * 100.0f; }

    // Conductor callback: master tempo changed (or just registered).
    void onBpm(float masterBpm) override { _master = masterBpm; apply(); }

private:
    // MidiFilePlayer::setTempoScale() clamps to a sane [0.10, 4.00] range.
    void apply() { _p.setTempoScale((_master / _native) * _trim); }

    MidiFilePlayer &_p;
    float _native = 120.0f;
    float _master = 120.0f;
    float _trim   = 1.0f;
};

}  // namespace tdsp
