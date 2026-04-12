// CodecPanel — interface for codec-specific OSC subtrees.
//
// Each example project implements a CodecPanel subclass for its codec
// (e.g. Tac5212Panel, AK4558Panel, WM8731Panel). The subclass owns the
// /codec/<model>/... subtree and translates OSC leaves into calls on the
// project's codec driver library.
//
// TDspMixer's OscDispatcher routes any /codec/<model>/... message to the
// registered CodecPanel whose modelName() matches <model>. The panel is
// responsible for its entire subtree — TDspMixer does not know what codec
// is in use and has no opinions about its controls beyond the namespace.
//
// The interface is minimal so that codec panels can be implemented in the
// project directory without pulling any TDspMixer implementation details.
//
// Note: CNMAT/OSC's OSCMessage is forward-declared here to avoid pulling
// <OSCMessage.h> into every consumer. The implementation file includes it.

#pragma once

#include <stdint.h>

class OSCMessage;
class OSCBundle;

namespace tdsp {

class CodecPanel {
public:
    virtual ~CodecPanel() = default;

    // Return the codec model identifier used in the OSC address path.
    // For a panel rooted at /codec/tac5212/, this returns "tac5212".
    // The dispatcher uses this to match incoming addresses.
    virtual const char *modelName() const = 0;

    // Called by OscDispatcher when a /codec/<modelName>/... message
    // arrives. `addrOffset` points at the first character *after* the
    // /codec/<modelName> prefix in msg.getAddress(); the panel looks at
    // that remaining subpath and dispatches internally.
    //
    // `reply` is the outgoing bundle for this dispatch round — the panel
    // appends any reply messages (echoes, status replies, error bundles)
    // to it, and the dispatcher flushes the bundle after the route returns.
    virtual void route(OSCMessage &msg, int addrOffset, OSCBundle &reply) = 0;

    // Append the panel's current state to a /snapshot reply bundle. Called
    // by the sketch's broadcastSnapshot() in response to a /snapshot request
    // from a freshly-connected client. Each override should emit one OSC
    // message per leaf the client UI cares about, in the same format the
    // panel would echo on a write — so the client's existing /codec/...
    // inbound routes pick them up without special-case handling.
    //
    // Default is empty: panels can adopt snapshot support tab-by-tab without
    // breaking other codec drivers. A leaf the panel can't read back from
    // the chip (e.g. PDM stubs) should just be omitted.
    virtual void snapshot(OSCBundle &reply) { (void)reply; }

    // Hard-mute / unmute the codec's analog output. Used by the sketch's
    // boot sequence (mute → load settings → applyAll → unmute) so audio
    // stays silent until in-memory state has been pushed into the audio
    // graph. Default is no-op for codecs without a hardware mute primitive
    // — those rely on the audio graph being initialized to silent gains.
    //
    // Implementations must be safe to call multiple times and from any
    // point after the codec has been initialized. unmuteOutput() is the
    // boot gate release; once called, the device is producing audio.
    virtual void muteOutput() {}
    virtual void unmuteOutput() {}
};

}  // namespace tdsp
