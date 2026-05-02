// Adc6140Panel — CodecPanel subclass for the TLV320ADC6140.
//
// Routes the /codec/adc6140/... OSC subtree into lib/TLV320ADC6140 calls.
//
// Leaves handled (per-channel N = 1..4):
//   /ch/N/gain f         0..42   analog PGA, 1 dB steps
//   /ch/N/dvol f        -100..+27 digital volume, 0.5 dB steps
//   /ch/N/impedance s    "2.5k" | "10k" | "20k"
//   /ch/N/coupling s     "ac" | "dc"
//   /ch/N/intype s       "mic" | "line"
//   /ch/N/source s       "differential" | "single_ended"
//   /ch/N/dre i          0|1
//   /ch/N/gaincal f     -0.8..+0.7 gain trim, 0.1 dB steps
//
// Chip-global leaves:
//   /micbias s          "off" | "vref" | "vref_boosted" | "avdd"
//   /fullscale s        "2v75" | "2v5" | "1v375"
//   /hpf s              "programmable" | "12hz" | "96hz" | "384hz"
//   /decimfilt s        "linear" | "low_latency" | "ultra_low_latency"
//   /chsum s            "off" | "pairs" | "quad"
//   /mode s             "dre" | "agc"
//   /dre/level i        -12..-66 (6 dB steps)
//   /dre/maxgain i      2..30 (even)
//   /agc/target i       -6..-36 (2 dB steps)
//   /agc/maxgain i      3..42 (3 dB steps)
//   /info               device info
//   /status             read-only chip status
//   /reg/set ii         raw write (page 0 only)
//   /reg/get i          raw read  (page 0 only)

#pragma once

#include <TDspMixer.h>
#include <TLV320ADC6140.h>

class OSCMessage;
class OSCBundle;

class Adc6140Panel : public tdsp::CodecPanel {
public:
    explicit Adc6140Panel(tlv320adc6140::TLV320ADC6140 &codec) : _codec(codec) {}

    const char *modelName() const override { return "adc6140"; }

    void route(OSCMessage &msg, int addrOffset, OSCBundle &reply) override;
    void snapshot(OSCBundle &reply) override;

    // No hardware mute primitive for a record-only ADC — the audio
    // graph is silent at boot because the per-channel fader amps start
    // at zero gain, and the DAC-side muting is owned by TAC5212.
    void muteOutput()   override {}
    void unmuteOutput() override {}

private:
    tlv320adc6140::TLV320ADC6140 &_codec;

    void handleChannelGain    (int n, OSCMessage &msg, OSCBundle &reply);
    void handleChannelDvol    (int n, OSCMessage &msg, OSCBundle &reply);
    void handleChannelImpedance(int n, OSCMessage &msg, OSCBundle &reply);
    void handleChannelCoupling (int n, OSCMessage &msg, OSCBundle &reply);
    void handleChannelInType   (int n, OSCMessage &msg, OSCBundle &reply);
    void handleChannelSource   (int n, OSCMessage &msg, OSCBundle &reply);
    void handleChannelDre      (int n, OSCMessage &msg, OSCBundle &reply);
    void handleChannelGainCal  (int n, OSCMessage &msg, OSCBundle &reply);

    void handleMicbias    (OSCMessage &msg, OSCBundle &reply);
    void handleFullscale  (OSCMessage &msg, OSCBundle &reply);
    void handleHpf        (OSCMessage &msg, OSCBundle &reply);
    void handleDecimFilt  (OSCMessage &msg, OSCBundle &reply);
    void handleChSum      (OSCMessage &msg, OSCBundle &reply);
    void handleMode       (OSCMessage &msg, OSCBundle &reply);
    void handleDreLevel   (OSCMessage &msg, OSCBundle &reply);
    void handleDreMaxGain (OSCMessage &msg, OSCBundle &reply);
    void handleAgcTarget  (OSCMessage &msg, OSCBundle &reply);
    void handleAgcMaxGain (OSCMessage &msg, OSCBundle &reply);

    void handleInfo   (OSCMessage &msg, OSCBundle &reply);
    void handleStatus (OSCMessage &msg, OSCBundle &reply);
    void handleRegSet (OSCMessage &msg, OSCBundle &reply);
    void handleRegGet (OSCMessage &msg, OSCBundle &reply);
};
