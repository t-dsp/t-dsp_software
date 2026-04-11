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
};

}  // namespace tdsp
