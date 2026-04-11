// Tac5212Panel — codec-specific CodecPanel subclass for the TAC5212.
//
// Routes the /codec/tac5212/... subtree into lib/TAC5212 method calls.
// Lives in the project directory (not in lib/TDspMixer/) because codec
// panels are per-board — different examples with different codecs each
// have their own CodecPanel subclass.
//
// Address tree handled (must match tools/web_dev_surface/src/codec-panel-
// config.ts and planning/osc-mixer-foundation/02-osc-protocol.md):
//
//   /adc/N/mode s         "single_ended_inp" | "differential"
//   /adc/N/impedance s    "5k" | "10k" | "40k"
//   /adc/N/fullscale s    "2vrms" | "4vrms"
//   /adc/N/coupling s     "ac" | "dc_low" | "dc_rail_to_rail"
//   /adc/N/bw s           "24khz" | "96khz"
//   /adc/hpf i            0|1 (chip-global ADC HPF)
//   /vref/fscale s        "2.75v" | "2.5v" | "1.375v" | "1.25v" | "avdd"
//   /micbias/enable i     0|1
//   /micbias/level s      "2.75v" | "2.5v" | "1.375v" | "1.25v" | "avdd"
//   /out/N/mode s         "diff_line" | "se_line" | "hp_driver" | "receiver"
//   /pdm/enable i         0|1 (stubbed in library; panel still accepts it)
//   /pdm/source s         "gpi1" | "gpi2" (stubbed)
//   /reset                no args — soft reset the chip
//   /wake i               0|1
//   /info                 no args — reply with device info
//   /status               no args — reply with read-only status
//   /reg/set ii           raw register write (page=0 assumed)
//   /reg/get i            raw register read (page=0 assumed)
//
// Every handler echoes the new value to the reply bundle so the client
// sees its own write confirmed.

#pragma once

#include <TDspMixer.h>
#include <TAC5212.h>

class OSCMessage;
class OSCBundle;

class Tac5212Panel : public tdsp::CodecPanel {
public:
    explicit Tac5212Panel(tac5212::TAC5212 &codec) : _codec(codec) {}

    const char *modelName() const override { return "tac5212"; }

    // addrOffset points at the first char after "/codec/tac5212". For
    // "/codec/tac5212/adc/1/mode" that's "/adc/1/mode".
    void route(OSCMessage &msg, int addrOffset, OSCBundle &reply) override;

private:
    tac5212::TAC5212 &_codec;

    // Per-leaf handlers. Each parses msg args, calls a lib/TAC5212 method,
    // and appends an echo to reply.
    void handleAdcMode(int n, OSCMessage &msg, OSCBundle &reply);
    void handleAdcImpedance(int n, OSCMessage &msg, OSCBundle &reply);
    void handleAdcFullscale(int n, OSCMessage &msg, OSCBundle &reply);
    void handleAdcCoupling(int n, OSCMessage &msg, OSCBundle &reply);
    void handleAdcBw(int n, OSCMessage &msg, OSCBundle &reply);
    void handleAdcHpf(OSCMessage &msg, OSCBundle &reply);

    void handleVrefFscale(OSCMessage &msg, OSCBundle &reply);

    void handleMicbiasEnable(OSCMessage &msg, OSCBundle &reply);
    void handleMicbiasLevel(OSCMessage &msg, OSCBundle &reply);

    void handleOutMode(int n, OSCMessage &msg, OSCBundle &reply);

    void handlePdmEnable(OSCMessage &msg, OSCBundle &reply);
    void handlePdmSource(OSCMessage &msg, OSCBundle &reply);

    void handleReset(OSCMessage &msg, OSCBundle &reply);
    void handleWake(OSCMessage &msg, OSCBundle &reply);
    void handleInfo(OSCMessage &msg, OSCBundle &reply);
    void handleStatus(OSCMessage &msg, OSCBundle &reply);

    void handleRegSet(OSCMessage &msg, OSCBundle &reply);
    void handleRegGet(OSCMessage &msg, OSCBundle &reply);
};
