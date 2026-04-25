// OscDispatcher — routes incoming OSC messages to mixer-domain handlers,
// codec panels, and subscription machinery.
//
// Address tree for MVP v1 (matches the web_dev_surface client at
// projects/t-dsp_tac5212_audio_shield_adaptor/tools/web_dev_surface/):
//
//   /ch/NN/mix/fader f     — channel fader, 0..1 normalized
//   /ch/NN/mix/on i        — channel on/mute (X32 idiom: on=0 is muted)
//   /ch/NN/mix/solo i      — channel solo (solo-in-place)
//   /ch/NN/config/name s   — channel label (firmware echoes on change)
//   /ch/NN/preamp/hpf/on i — per-channel HPF enable
//   /ch/NN/preamp/hpf/f f  — per-channel HPF cutoff (Hz)
//   /ch/NN/config/link i   — stereo link (odd channel only)
//   /main/st/mix/faderL f  — main L fader (propagates to R when linked)
//   /main/st/mix/faderR f  — main R fader (propagates to L when linked)
//   /main/st/mix/link i    — main stereo link (default on)
//   /main/st/mix/on i      — main mute (shared L/R)
//   /main/st/hostvol/enable i — Windows slider pre-main attenuator toggle
//   /main/st/hostvol/value f  — read-only; firmware echoes from usbIn.volume()
//   /meters/input b        — blob stream, subscription-managed
//   /sub sis               — addSub (interval, lifetime, pattern) - accepted but
//                            currently no-op; meters always stream if enabled
//   /codec/<model>/...     — routed to registered CodecPanel
//   /info                  — read-only device identity
//
// Every write handler:
//   1. Mutates the MixerModel.
//   2. Calls SignalGraphBinding::applyChannel / applyMain / etc.
//   3. Echoes the new value back on the same address via the reply bundle.
//
// The echo is how multiple clients stay in sync. When Client A moves a
// fader, Client B sees the echo and updates its UI.

#pragma once

#include <stdint.h>

class OSCMessage;
class OSCBundle;

namespace tdsp {

class MixerModel;
class SignalGraphBinding;
class CodecPanel;
class MeterEngine;
class SpectrumEngine;

class OscDispatcher {
public:
    OscDispatcher();

    void setModel(MixerModel *model)                 { _model = model; }
    void setBinding(SignalGraphBinding *binding)     { _binding = binding; }
    void setMeterEngine(MeterEngine *engine)         { _meterEngine = engine; }
    void setSpectrumEngine(SpectrumEngine *engine)   { _spectrumEngine = engine; }
    // Append a codec panel. Each panel claims the /codec/<modelName>/...
    // subtree. Up to kMaxCodecPanels panels can coexist (one per chip).
    // Re-registration of the same pointer is a no-op; otherwise the
    // call is dropped if the table is full.
    void registerCodecPanel(CodecPanel *panel);

    // Route an incoming OSCMessage. Called from SlipOscTransport's OSC
    // handler callback. The reply bundle accumulates echoes and is
    // flushed by the caller after this returns.
    void route(OSCMessage &msg, OSCBundle &reply);

    // Broadcast helpers — called when the model changes from a non-OSC
    // source (e.g., Windows volume slider moved) so subscribed clients
    // see the update without being the originator.
    void broadcastMainFaderL(OSCBundle &reply);
    void broadcastMainFaderR(OSCBundle &reply);
    void broadcastMainLink(OSCBundle &reply);
    void broadcastMainOn(OSCBundle &reply);
    void broadcastMainHostvolValue(OSCBundle &reply);
    void broadcastMainLoop(OSCBundle &reply);
    void broadcastChannelFader(int n, OSCBundle &reply);
    void broadcastChannelOn(int n, OSCBundle &reply);
    void broadcastChannelSolo(int n, OSCBundle &reply);
    void broadcastChannelName(int n, OSCBundle &reply);
    void broadcastChannelRecSend(int n, OSCBundle &reply);
    void broadcastMetersInput(OSCBundle &reply,
                              const float *peakRmsPairs, int pairCount);
    void broadcastMetersOutput(OSCBundle &reply,
                               const float *peakRmsPairs, int pairCount);
    void broadcastMetersHost(OSCBundle &reply,
                             const float *peakRmsPairs, int pairCount);
    // Pack a raw byte array as a /spectrum/main blob (uint8 dB bytes,
    // 512 L then 512 R for the stereo main bus FFT). No byte-swapping:
    // the payload is raw bytes, not big-endian float pairs.
    void broadcastMainSpectrum(OSCBundle &reply,
                               const uint8_t *bytes, int byteCount);

private:
    static constexpr int kMaxCodecPanels = 4;
    MixerModel         *_model          = nullptr;
    SignalGraphBinding *_binding        = nullptr;
    CodecPanel         *_codecPanels[kMaxCodecPanels] = {};
    int                 _codecPanelCount = 0;
    MeterEngine        *_meterEngine    = nullptr;
    SpectrumEngine     *_spectrumEngine = nullptr;

    // Per-leaf handlers. Called from route() after matching the address.
    // Each mutates the model, calls binding, and appends echo to reply.
    void handleChannelFader(int n, OSCMessage &msg, OSCBundle &reply);
    void handleChannelOn(int n, OSCMessage &msg, OSCBundle &reply);
    void handleChannelSolo(int n, OSCMessage &msg, OSCBundle &reply);
    void handleChannelName(int n, OSCMessage &msg, OSCBundle &reply);
    void handleChannelLink(int n, OSCMessage &msg, OSCBundle &reply);
    void handleChannelHpfOn(int n, OSCMessage &msg, OSCBundle &reply);
    void handleChannelHpfFreq(int n, OSCMessage &msg, OSCBundle &reply);
    void handleMainFaderL(OSCMessage &msg, OSCBundle &reply);
    void handleMainFaderR(OSCMessage &msg, OSCBundle &reply);
    void handleMainLink(OSCMessage &msg, OSCBundle &reply);
    void handleMainOn(OSCMessage &msg, OSCBundle &reply);
    void handleMainHostvolEnable(OSCMessage &msg, OSCBundle &reply);
    void handleMainLoop(OSCMessage &msg, OSCBundle &reply);
    void handleChannelRecSend(int n, OSCMessage &msg, OSCBundle &reply);
    void handleSub(OSCMessage &msg, OSCBundle &reply);
    void handleInfo(OSCMessage &msg, OSCBundle &reply);

    // Address-pattern parser: extracts the channel number from an address
    // of the form "/ch/NN/...", returning the number or 0 if no match.
    // `outSuffix` is set to the address starting at the position after
    // "/ch/NN".
    static int parseChannelAddress(const char *address, const char **outSuffix);
};

}  // namespace tdsp
