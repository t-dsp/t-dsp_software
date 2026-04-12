// T-DSP TAC5212 Audio Shield Adaptor — small mixer MVP v1.
//
// This is the "thin wiring" main.cpp per the roadmap M12 plan, scoped
// to MVP v1 per ~/.claude/memory/decisions_mvp_v1_scope.md. It instantiates
// the TDspMixer framework, wires the existing stock I16 audio graph
// (USB in + line in + stereo PDM mic → main mixer → DAC + USB capture),
// registers a Tac5212Panel for the /codec/tac5212/... OSC subtree, and
// runs the framework in loop().
//
// 6 input channels, mapped 1:1 to the small mixer hardware:
//   /ch/01 — USB L           (host USB input, left)
//   /ch/02 — USB R           (host USB input, right)
//   /ch/03 — Line L          (TAC5212 ADC CH1)
//   /ch/04 — Line R          (TAC5212 ADC CH2)
//   /ch/05 — Mic L           (onboard PDM mic, left channel)
//   /ch/06 — Mic R           (onboard PDM mic, right channel)
//
// Main output goes to the headphone DAC (TAC5212 OUT1/OUT2 in HP driver
// mode, locked in during Phase 1) AND to USB capture so the host can
// record the mix. USB inputs are EXCLUDED from the capture path to
// prevent self-monitoring.
//
// Codec initialization still uses the hand-rolled setupCodec() flow from
// Phase 1. Migrating to lib/TAC5212's typed API is a follow-on refactor
// (M11 Part A). The Tac5212Panel uses lib/TAC5212 at runtime for OSC-
// driven changes, so the two coexist on the same I2C bus.

#include <Arduino.h>
#include <Audio.h>
#include <Wire.h>

#include <OSCMessage.h>
#include <OSCBundle.h>

#include "tac5212_regs.h"
#include <TAC5212.h>
#include <TDspMixer.h>

#include "Tac5212Panel.h"

// Teensy pin driving EN_HELD_HIGH on the TAC5212 module → enables the LDO
const int TAC5212_EN_PIN = 35;

// ============================================================================
// Stock I16 Audio Library objects — the signal graph
// ============================================================================

AudioInputUSB        usbIn;
AudioInputTDM        tdmIn;

// Per-channel peak/RMS taps for meters. 6 channels, 2 analyzers each.
AudioAnalyzePeak     peakCh1;  // USB L
AudioAnalyzePeak     peakCh2;  // USB R
AudioAnalyzePeak     peakCh3;  // Line L
AudioAnalyzePeak     peakCh4;  // Line R
AudioAnalyzePeak     peakCh5;  // Mic L
AudioAnalyzePeak     peakCh6;  // Mic R
AudioAnalyzeRMS      rmsCh1;
AudioAnalyzeRMS      rmsCh2;
AudioAnalyzeRMS      rmsCh3;
AudioAnalyzeRMS      rmsCh4;
AudioAnalyzeRMS      rmsCh5;
AudioAnalyzeRMS      rmsCh6;

// Main bus stereo meter taps — post-main-fader, PRE-hostvol. This is
// the X32-style "main LR meter" reading: reflects the engineer's fader
// moves but NOT the Windows volume slider attenuation. Pre-hostvol is
// the correct tap because the Windows slider is an end-user output
// trim, not a mix-engine change.
AudioAnalyzePeak     peakMainL;
AudioAnalyzePeak     peakMainR;
AudioAnalyzeRMS      rmsMainL;
AudioAnalyzeRMS      rmsMainR;

// Host (Windows-volume) meter taps — post-hostvol, so they show the
// actual level headed to the DAC (main fader × hostvol × hostvolEnable).
// The engineer can compare these against the main-bus meters to see
// how much Windows volume is attenuating things.
AudioAnalyzePeak     peakHostL;
AudioAnalyzePeak     peakHostR;
AudioAnalyzeRMS      rmsHostL;
AudioAnalyzeRMS      rmsHostR;

// Main bus stereo FFT taps — sit on the same pre-hostvol node as the
// peak/RMS meter taps, so the spectrum analyzer view reflects the mix
// engineer's faders but NOT Windows volume attenuation. FFT1024 gives
// us 512 bins at ~86 Hz resolution (44.1 kHz / 1024 * 2), new frame
// every ~11.6 ms. SpectrumEngine polls them at ~30 Hz.
AudioAnalyzeFFT1024  fftMainL;
AudioAnalyzeFFT1024  fftMainR;

// PDM mic combiners — 32-bit PDM split across two 16-bit TDM slots, so
// each PDM mic (L, R) needs a 2-input mixer that re-combines the high
// and low halves with correct scaling.
AudioMixer4          pdmMixL;
AudioMixer4          pdmMixR;

// Main stereo mixer. Slot assignments:
//   mixL[0] = ch/01 USB L    mixR[0] = ch/02 USB R
//   mixL[1] = ch/05 Mic L    mixR[1] = ch/06 Mic R
//   mixL[2] = ch/03 Line L   mixR[2] = ch/04 Line R
//   slot 3 unused
AudioMixer4          mixL;
AudioMixer4          mixR;

// Two-stage main bus chain:
//   mixL → mainAmpL (faderL × on) → hostvolAmpL (hostvol bypass) → DAC
// The meter taps sit at the output of mainAmpL/R, so they see the
// post-fader / pre-hostvol signal. SignalGraphBinding drives both
// stages from MixerModel via applyMain().
AudioAmplifier       mainAmpL;
AudioAmplifier       mainAmpR;
AudioAmplifier       hostvolAmpL;
AudioAmplifier       hostvolAmpR;

// Capture mixers — line + PDM → USB out. USB in intentionally excluded
// to prevent self-monitoring.
AudioMixer4          captureL;
AudioMixer4          captureR;

// Listenback monitor attenuators — driven by the Windows recording-device
// volume slider via the FU 0x30 capture-side Feature Unit. Inserted on the
// MONITOR branch of each capture source (line + mic) so the user can pull
// down what they hear in their headphones without affecting the record
// level the DAW receives. USB-in monitoring is intentionally NOT routed
// through these (its level is controlled by the Windows playback slider
// via hostvolAmpL/R), and the captureL/R → usbOut record path stays at
// unity (record send is independent).
//
// Each tdmIn / pdmMix source tees into THREE branches:
//   1. captureL/R (record send, unity, untouched)
//   2. peak/rms taps (meter, unity, untouched)
//   3. monLine* / monMic* → mixL/R (THIS branch, attenuated by capHostVol)
//
// Default gain is unity (1.0); pollCaptureHostVolume() applies the live
// value once Windows pushes its first SET_CUR.
AudioAmplifier       monLineL;
AudioAmplifier       monLineR;
AudioAmplifier       monMicL;
AudioAmplifier       monMicR;

AudioOutputTDM       tdmOut;
AudioOutputUSB       usbOut;

// ============================================================================
// AudioConnections — wire the graph
// ============================================================================

// USB input → main mixer slot 0 + peak/RMS taps
AudioConnection      c_usbL_mix    (usbIn, 0, mixL, 0);
AudioConnection      c_usbL_peak   (usbIn, 0, peakCh1, 0);
AudioConnection      c_usbL_rms    (usbIn, 0, rmsCh1, 0);
AudioConnection      c_usbR_mix    (usbIn, 1, mixR, 0);
AudioConnection      c_usbR_peak   (usbIn, 1, peakCh2, 0);
AudioConnection      c_usbR_rms    (usbIn, 1, rmsCh2, 0);

// Line input (ADC) → record send (unity) + peak/RMS taps + listenback
// monitor branch (through monLine* attenuator → main mixer slot 2). The
// record send and meter taps draw directly from tdmIn so they're
// unaffected by the listenback gain — DAW gets full signal regardless
// of what the user sets the headphone monitor to.
AudioConnection      c_lineL_cap   (tdmIn, 0, captureL, 0);
AudioConnection      c_lineL_peak  (tdmIn, 0, peakCh3, 0);
AudioConnection      c_lineL_rms   (tdmIn, 0, rmsCh3, 0);
AudioConnection      c_lineL_mon   (tdmIn, 0, monLineL, 0);
AudioConnection      c_lineL_mix   (monLineL, 0, mixL, 2);
AudioConnection      c_lineR_cap   (tdmIn, 2, captureR, 0);
AudioConnection      c_lineR_peak  (tdmIn, 2, peakCh4, 0);
AudioConnection      c_lineR_rms   (tdmIn, 2, rmsCh4, 0);
AudioConnection      c_lineR_mon   (tdmIn, 2, monLineR, 0);
AudioConnection      c_lineR_mix   (monLineR, 0, mixR, 2);

// PDM mic: split across TDM slots 2+3 = Teensy ch 4,5,6,7, then combined
AudioConnection      c_pdmL0       (tdmIn, 4, pdmMixL, 0);
AudioConnection      c_pdmL1       (tdmIn, 5, pdmMixL, 1);
AudioConnection      c_pdmR0       (tdmIn, 6, pdmMixR, 0);
AudioConnection      c_pdmR1       (tdmIn, 7, pdmMixR, 1);

// PDM combiners → record send (unity) + peak/RMS taps + listenback
// monitor branch (through monMic* attenuator → main mixer slot 1). Same
// pattern as the line inputs above: record and meters stay unity, only
// the headphone-monitor branch is attenuated.
AudioConnection      c_micL_cap    (pdmMixL, 0, captureL, 1);
AudioConnection      c_micL_peak   (pdmMixL, 0, peakCh5, 0);
AudioConnection      c_micL_rms    (pdmMixL, 0, rmsCh5, 0);
AudioConnection      c_micL_mon    (pdmMixL, 0, monMicL, 0);
AudioConnection      c_micL_mix    (monMicL, 0, mixL, 1);
AudioConnection      c_micR_cap    (pdmMixR, 0, captureR, 1);
AudioConnection      c_micR_peak   (pdmMixR, 0, peakCh6, 0);
AudioConnection      c_micR_rms    (pdmMixR, 0, rmsCh6, 0);
AudioConnection      c_micR_mon    (pdmMixR, 0, monMicR, 0);
AudioConnection      c_micR_mix    (monMicR, 0, mixR, 1);

// Main mixers → main fader amps → hostvol amps → DAC (TDM out slots 0, 2)
AudioConnection      c_mainL_amp   (mixL, 0, mainAmpL, 0);
AudioConnection      c_mainR_amp   (mixR, 0, mainAmpR, 0);
AudioConnection      c_mainL_hv    (mainAmpL, 0, hostvolAmpL, 0);
AudioConnection      c_mainR_hv    (mainAmpR, 0, hostvolAmpR, 0);
AudioConnection      c_mainL_dac   (hostvolAmpL, 0, tdmOut, 0);
AudioConnection      c_mainR_dac   (hostvolAmpR, 0, tdmOut, 2);

// Main bus meter taps — on the FADER amp output, pre-hostvol. So the
// meter tracks the main fader but is unaffected by Windows volume.
AudioConnection      c_mainL_peak  (mainAmpL, 0, peakMainL, 0);
AudioConnection      c_mainL_rms   (mainAmpL, 0, rmsMainL, 0);
AudioConnection      c_mainR_peak  (mainAmpR, 0, peakMainR, 0);
AudioConnection      c_mainR_rms   (mainAmpR, 0, rmsMainR, 0);

// Main bus FFT taps — same tap node as the peak/RMS meters above so
// the spectrum reflects the engineer's fader but not Windows volume.
// Each FFT1024 instance runs continuously (the stock Audio lib offers
// no clean enable/disable); it only costs network + CPU when the
// SpectrumEngine is actually polling and emitting blobs.
AudioConnection      c_mainL_fft   (mainAmpL, 0, fftMainL, 0);
AudioConnection      c_mainR_fft   (mainAmpR, 0, fftMainR, 0);

// Host meter taps — on the hostvol amp output, POST-hostvol. So the
// meter shows what the DAC actually receives, including Windows volume
// attenuation. Compare vs the main meters to see hostvol in action.
AudioConnection      c_hostL_peak  (hostvolAmpL, 0, peakHostL, 0);
AudioConnection      c_hostL_rms   (hostvolAmpL, 0, rmsHostL, 0);
AudioConnection      c_hostR_peak  (hostvolAmpR, 0, peakHostR, 0);
AudioConnection      c_hostR_rms   (hostvolAmpR, 0, rmsHostR, 0);

// Capture mixers → USB out (host recording)
AudioConnection      c_capL_usb    (captureL, 0, usbOut, 0);
AudioConnection      c_capR_usb    (captureR, 0, usbOut, 1);

// ============================================================================
// TDspMixer framework objects
// ============================================================================

tdsp::MixerModel          g_model;
tdsp::SignalGraphBinding  g_binding;
tdsp::OscDispatcher       g_dispatcher;
tdsp::SlipOscTransport    g_transport;
tdsp::MeterEngine         g_meters;
tdsp::SpectrumEngine      g_spectrum;

// lib/TAC5212 codec instance used by Tac5212Panel for runtime register
// access. The chip is physically initialized by setupCodecHandRolled()
// below; g_codec is NOT .begin()-ed because that would re-reset the
// chip and wipe the hand-rolled init state. Both APIs touch the same
// I2C address, which is safe as long as we don't interleave writes
// to the same register from both sides.
tac5212::TAC5212          g_codec(Wire);

Tac5212Panel              g_codecPanel(g_codec);

// ============================================================================
// Hand-rolled codec init (Phase 1 path, preserved as the working audio config)
// ============================================================================

static void writeReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(TAC5212_I2C_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

static uint8_t readReg(uint8_t reg) {
    Wire.beginTransmission(TAC5212_I2C_ADDR);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom(TAC5212_I2C_ADDR, (uint8_t)1);
    return Wire.available() ? Wire.read() : 0xFF;
}

static void codecResetAndWake() {
    writeReg(REG_SW_RESET, 0x01);
    delay(100);
    writeReg(REG_SLEEP_CFG, SLEEP_CFG_WAKE);
    delay(100);
}

static void codecConfigureSerialInterface() {
    writeReg(REG_PASI_CFG0, PASI_CFG0_TDM_32_BCLK_INV);
    writeReg(REG_INTF_CFG1, INTF_CFG1_DOUT_PASI);
    writeReg(REG_INTF_CFG2, INTF_CFG2_DIN_ENABLE);
    writeReg(REG_PASI_RX_CFG0, PASI_OFFSET_1);
    writeReg(REG_PASI_TX_CFG2, PASI_OFFSET_1);
}

static void codecConfigureSlotMappings() {
    writeReg(REG_RX_CH1_SLOT, slot(0));
    writeReg(REG_RX_CH2_SLOT, slot(1));
    writeReg(REG_TX_CH1_SLOT, slot(0));
    writeReg(REG_TX_CH2_SLOT, slot(1));
}

static void codecConfigurePdmMic() {
    writeReg(REG_GPIO1_CFG, GPIO1_PDM_CLK);
    writeReg(REG_GPI1_CFG,  GPI1_INPUT);
    writeReg(REG_INTF_CFG4, INTF_CFG4_GPI1_PDM_3_4);
    writeReg(REG_TX_CH3_SLOT, slot(2));
    writeReg(REG_TX_CH4_SLOT, slot(3));
}

static void codecConfigureAdcInputs() {
    writeReg(REG_ADC_CH1_CFG0, ADC_CFG0_SE_INP_ONLY);
    writeReg(REG_ADC_CH2_CFG0, ADC_CFG0_SE_INP_ONLY);
}

static void codecConfigureDacOutputs() {
    writeReg(REG_OUT1_CFG0, OUT_SRC_DAC | OUT_ROUTE_MONO_SE_P);
    writeReg(REG_OUT1_CFG1, OUT_CFG1_HP_0DB);
    writeReg(REG_OUT1_CFG2, OUT_CFG1_HP_0DB);
    writeReg(REG_OUT2_CFG0, OUT_SRC_DAC | OUT_ROUTE_MONO_SE_P);
    writeReg(REG_OUT2_CFG1, OUT_CFG1_HP_0DB);
    writeReg(REG_OUT2_CFG2, OUT_CFG1_HP_0DB);
}

static void codecMuteDacVolume() {
    // Boot the DAC at the mute code (volume == 0) so the chip powers up
    // silent. The DAC volume registers are written BEFORE codecPowerUp()
    // turns on the DAC channel via PWR_CFG, so when audio first starts
    // flowing the analog output is already at -inf. setup() releases the
    // boot gate later by calling g_codecPanel.unmuteOutput(), which
    // writes DAC_VOL_0DB into all four registers.
    writeReg(REG_DAC_L1_VOL, 0);
    writeReg(REG_DAC_R1_VOL, 0);
    writeReg(REG_DAC_L2_VOL, 0);
    writeReg(REG_DAC_R2_VOL, 0);
}

static void codecPowerUp() {
    writeReg(REG_CH_EN,   CH_EN_ALL);
    writeReg(REG_PWR_CFG, PWR_CFG_ADC_DAC_MICBIAS);
    delay(100);
}

static void setupCodecHandRolled() {
    Serial.println("Initializing TAC5212 (Phase 1 hand-rolled path)...");
    codecResetAndWake();
    codecConfigureSerialInterface();
    codecConfigureSlotMappings();
    codecConfigurePdmMic();
    codecConfigureAdcInputs();
    codecConfigureDacOutputs();
    codecMuteDacVolume();  // boot gate: DAC powers up muted (volume == 0)
    codecPowerUp();
    Serial.print("DEV_STS0: 0x");
    Serial.println(readReg(REG_DEV_STS0), HEX);
    Serial.println("Codec ready: TDM + PDM + ADC");
}

// ============================================================================
// TDspMixer integration callbacks
// ============================================================================

// Forward decl — defined below near pollCaptureHostVolume so it can
// see the sketch-local statics (s_lastCapVolRaw etc), but called here.
static void broadcastSnapshot(OSCBundle &reply);

// Forward decls — defined later in the file alongside their static
// state. setup() calls these once at the end of init to read the
// initial USB Feature Unit values into the audio graph BEFORE the
// boot-gate release (g_codecPanel.unmuteOutput()), so the first
// audio frame out of the DAC reflects whatever Windows has already
// pushed instead of the cold-boot defaults.
static void pollHostVolume();
static void pollCaptureHostVolume();

// OSC frame arrived from the transport. Build a reply bundle, route into
// the dispatcher (which handles /ch/..., /main/..., /sub, /info, and
// forwards /codec/tac5212/* to Tac5212Panel), and flush the reply back
// via SLIP if anything was added.
//
// One address is intercepted BEFORE dispatching: /snapshot (no args)
// triggers a full-state dump and bypasses the dispatcher entirely,
// since the dispatcher doesn't know about the sketch-local capture-
// side hostvol state. A client sends /snapshot on connect to catch
// up to live firmware state without waiting for the next change.
static void onOscMessage(OSCMessage &msg, void *userData) {
    (void)userData;

    char address[32];
    int addrLen = msg.getAddress(address, 0, sizeof(address) - 1);
    if (addrLen < 0) addrLen = 0;
    address[addrLen] = '\0';

    if (strcmp(address, "/snapshot") == 0) {
        OSCBundle reply;
        broadcastSnapshot(reply);
        if (reply.size() > 0) {
            g_transport.sendBundle(reply);
        }
        return;
    }

    OSCBundle reply;
    g_dispatcher.route(msg, reply);
    if (reply.size() > 0) {
        g_transport.sendBundle(reply);
    }
}

// CLI line arrived (plain ASCII). For MVP v1 we only recognize one
// command: "s" for status dump. The legacy Phase 1 CLI (u/p/i/m/l/+/-/
// arrow keys) has been removed — volume control moves to OSC via the
// web_dev_surface client or raw OSC.
static void onCliLine(char *line, int length, void *userData) {
    (void)userData;
    (void)length;
    if (!line) return;
    if (line[0] == 's' || line[0] == 'S') {
        Serial.println("\n--- Status ---");
        Serial.print("Audio Mem: ");
        Serial.print(AudioMemoryUsage());
        Serial.print("/");
        Serial.println(AudioMemoryUsageMax());
        Serial.print("CPU: ");
        Serial.print(AudioProcessorUsage(), 2);
        Serial.println("%");
        Serial.print("USB host vol raw: ");
        Serial.print(usbIn.volume(), 4);
        Serial.print("  scaled: ");
        Serial.println(g_model.main().hostvolValue, 3);
        // Capture-side feature unit (USB FU 0x30) — driven by the Windows
        // recording-device slider. Added by the teensy4 core patch on
        // branch teensy4-usb-audio-capture-feature-unit. If this prints
        // 0.5000 / mute=0 right after enumeration, the descriptor is
        // accepted; if it changes when you drag the slider in Windows
        // Sound -> Recording -> Properties -> Levels, the SET_CUR
        // dispatch is routing to the right entity ID.
        Serial.print("USB capture vol raw: ");
        Serial.print(usbOut.volume(), 4);
        Serial.print("  mute: ");
        Serial.println(usbOut.mute() ? 1 : 0);
        for (int n = 1; n <= tdsp::kChannelCount; ++n) {
            Serial.print("  ");
            Serial.print(g_model.channel(n).name);
            Serial.print("  fader=");
            Serial.print(g_model.channel(n).fader, 3);
            Serial.print("  on=");
            Serial.print(g_model.channel(n).on ? 1 : 0);
            Serial.print("  solo=");
            Serial.println(g_model.channel(n).solo ? 1 : 0);
        }
        Serial.print("Main  faderL=");
        Serial.print(g_model.main().faderL, 3);
        Serial.print("  faderR=");
        Serial.print(g_model.main().faderR, 3);
        Serial.print("  link=");
        Serial.print(g_model.main().link ? 1 : 0);
        Serial.print("  on=");
        Serial.print(g_model.main().on ? 1 : 0);
        Serial.print("  hostvol.enable=");
        Serial.print(g_model.main().hostvolEnable ? 1 : 0);
        Serial.print("  hostvol.value=");
        Serial.println(g_model.main().hostvolValue, 3);
        Serial.print("DEV_STS0: 0x");
        Serial.println(readReg(REG_DEV_STS0), HEX);
        Serial.println("--------------\n");
    }
    // Unknown CLI input is silently ignored for MVP.
}

// ============================================================================
// setup()
// ============================================================================

void setup() {
    pinMode(TAC5212_EN_PIN, OUTPUT);
    digitalWrite(TAC5212_EN_PIN, HIGH);

    Serial.begin(115200);
    pinMode(LED_BUILTIN, OUTPUT);

    while (!Serial && millis() < 3000) {
        digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
        delay(100);
    }

    Serial.println("================================");
    Serial.println("  T-DSP TAC5212 Audio Shield");
    Serial.println("  MVP v1 (TDspMixer + WebSerial)");
    Serial.println("================================");

    delay(100);
    Wire.begin();

    Wire.beginTransmission(TAC5212_I2C_ADDR);
    if (Wire.endTransmission() != 0) {
        Serial.println("TAC5212 not found!");
        while (1) { delay(100); }
    }

    // Codec init via the Phase 1 hand-rolled path. After this, g_codec
    // (lib/TAC5212) is ALSO valid for runtime register access through
    // Tac5212Panel without a re-init. Never call g_codec.begin() — that
    // would SW-reset the chip and wipe the hand-rolled init.
    setupCodecHandRolled();

    AudioMemory(64);

    // PDM mic amplitude trim: 32-bit PDM split across two 16-bit slots
    // — high 16 bits need a 16x boost, low 16 bits need a 1/65536
    // scaling so they add up to the full 32-bit value.
    const float kPdmGain = 16.0f;
    pdmMixL.gain(0, kPdmGain);
    pdmMixL.gain(1, kPdmGain / 65536.0f);
    pdmMixR.gain(0, kPdmGain);
    pdmMixR.gain(1, kPdmGain / 65536.0f);

    // Capture mixers at unity by default.
    captureL.gain(0, 1.0f);
    captureL.gain(1, 1.0f);
    captureR.gain(0, 1.0f);
    captureR.gain(1, 1.0f);

    // Listenback monitor amps default to unity. pollCaptureHostVolume()
    // will overwrite them as soon as Windows pushes a SET_CUR for FU 0x30.
    monLineL.gain(1.0f);
    monLineR.gain(1.0f);
    monMicL.gain(1.0f);
    monMicR.gain(1.0f);

    // Force the capture-side Feature Unit's cold-boot value to 100% so
    // the headphone monitor is at unity until Windows tells us otherwise.
    // The Teensy core defaults this to FEATURE_MAX_VOLUME/2 = 128 (matches
    // the playback FU pattern), which at our linear taper would silently
    // attenuate listenback by ~6 dB on every cold boot. This also means
    // GET_CUR returns FEATURE_MAX_VOLUME on first query, so Windows shows
    // the recording slider at 100% on enumeration. Set BEFORE the first
    // pollCaptureHostVolume() call so the change-detection logic doesn't
    // fire a spurious /usb/cap/hostvol/value 0.502 broadcast.
    AudioOutputUSB::features.volume = FEATURE_MAX_VOLUME;

    // --- Wire TDspMixer to the audio graph ---

    g_binding.setModel(&g_model);

    // Register each channel with its main-mixer slot. Per-channel HPF
    // objects are nullptr for MVP — the model tracks HPF state but the
    // audio path doesn't have biquads wired in yet. Follow-on commit
    // adds the biquad chain once everything else is green.
    g_binding.setChannel(1, &mixL, 0, nullptr);  // USB L
    g_binding.setChannel(2, &mixR, 0, nullptr);  // USB R
    g_binding.setChannel(3, &mixL, 2, nullptr);  // Line L
    g_binding.setChannel(4, &mixR, 2, nullptr);  // Line R
    g_binding.setChannel(5, &mixL, 1, nullptr);  // Mic L
    g_binding.setChannel(6, &mixR, 1, nullptr);  // Mic R
    g_binding.setMain(&mainAmpL, &mainAmpR);
    g_binding.setMainHostvol(&hostvolAmpL, &hostvolAmpR);
    g_binding.applyAll();  // push initial model defaults into audio objects

    g_dispatcher.setModel(&g_model);
    g_dispatcher.setBinding(&g_binding);
    g_dispatcher.setMeterEngine(&g_meters);
    g_dispatcher.setSpectrumEngine(&g_spectrum);
    g_dispatcher.registerCodecPanel(&g_codecPanel);

    g_transport.begin(115200);
    g_transport.setOscMessageHandler(&onOscMessage, nullptr);
    g_transport.setCliLineHandler(&onCliLine, nullptr);

    g_meters.setDispatcher(&g_dispatcher);
    g_meters.setChannel(1, &peakCh1, &rmsCh1);
    g_meters.setChannel(2, &peakCh2, &rmsCh2);
    g_meters.setChannel(3, &peakCh3, &rmsCh3);
    g_meters.setChannel(4, &peakCh4, &rmsCh4);
    g_meters.setChannel(5, &peakCh5, &rmsCh5);
    g_meters.setChannel(6, &peakCh6, &rmsCh6);
    g_meters.setMain(&peakMainL, &rmsMainL, &peakMainR, &rmsMainR);
    g_meters.setHost(&peakHostL, &rmsHostL, &peakHostR, &rmsHostR);

    g_spectrum.setDispatcher(&g_dispatcher);
    g_spectrum.setChannels(&fftMainL, &fftMainR);

    // --- Boot-gate release ---
    //
    // The TAC5212 DAC has been muted since codecMuteDacVolume() ran inside
    // setupCodecHandRolled() — every analog output has been silent through
    // the entire init. Before un-muting, capture the current USB Feature
    // Unit values (playback FU 0x101 = Windows master volume slider,
    // capture FU 0x30 = Windows recording-device slider) and push them
    // into the audio graph so the first audio frame out of the DAC
    // reflects the actual host state instead of the cold-boot defaults.
    //
    // If the USB host hasn't enumerated yet (cold cable plug, slow host),
    // the polled values are the cold-boot defaults — playback FU at 50%
    // (usb_audio.cpp default), capture FU at 100% (the override applied
    // above). The next pollHostVolume() / pollCaptureHostVolume() in
    // loop() will overwrite both as soon as Windows pushes its real
    // SET_CUR. That brief window is bounded by USB enumeration time
    // (milliseconds), and during it the audio graph is at the loaded /
    // applied state — better than the unloaded state we had before
    // this gate existed.
    pollHostVolume();
    pollCaptureHostVolume();
    g_codecPanel.unmuteOutput();

    Serial.println("\nReady!");
    Serial.println("  6 input channels: USB L/R, Line L/R, Mic L/R");
    Serial.println("  Main stereo -> DAC + USB capture");
    Serial.println("  Connect with the WebSerial app in tools/web_dev_surface/");
    Serial.println("  or type 's' in the serial monitor for status.");
}

// ============================================================================
// Host volume tracking (square-law taper preserved from Phase 1)
// ============================================================================

static float s_lastUsbVolRaw = -1.0f;

static void pollHostVolume() {
    const float raw = usbIn.volume();
    if (raw == s_lastUsbVolRaw) return;
    s_lastUsbVolRaw = raw;

    // Square-law taper: 50% slider → -12 dB, 25% → -24 dB. Phase 1
    // experimentation settled on this as "close enough to a real log-
    // taper pot" without the cost of an actual logf() per poll.
    float scaled = raw * raw;
    if (scaled > 1.0f) scaled = 1.0f;

    if (g_model.setMainHostvolValue(scaled)) {
        g_binding.applyMain();
        // Echo /main/st/hostvol/value to any subscribed clients.
        OSCBundle reply;
        g_dispatcher.broadcastMainHostvolValue(reply);
        if (reply.size() > 0) g_transport.sendBundle(reply);
    }
}

// ============================================================================
// USB capture-side host volume tracking (Windows recording-device slider)
// ============================================================================
//
// Companion to pollHostVolume() above, but for the OTHER USB Audio Class
// Feature Unit added by the teensy4 core patch on branch
// teensy4-usb-audio-capture-feature-unit. FU 0x30 lives on the device->host
// capture path; Windows binds it to the recording-device "Levels" tab
// slider. Dragging that slider sends SET_CUR over the USB control endpoint,
// which lands in AudioOutputUSB::features (volume + mute).
//
// We do not (yet) apply this gain to the audio path. The whole reason it
// exists is so the user can control listenback level from Windows. Wiring
// it into the capture mixer is a follow-on once the value is verified to
// be flowing correctly. For now we just broadcast it so the web dev
// surface can render it as a read-only "CAP HOST" strip and prove the
// SET_CUR routing works end to end.
//
// Wire format:
//   /usb/cap/hostvol/value f <0..1>
//   /usb/cap/hostvol/mute  i <0|1>
//
// Both addresses fire ONLY on change (rising-edge style). The dev surface
// signal store de-dupes too, so this just keeps the byte rate down.

static float s_lastCapVolRaw = -1.0f;
static int   s_lastCapMute   = -1;

// Compute the actual gain to apply to the listenback monitor amps from
// the raw FU 0x30 volume + mute state. Linear taper — NOT the square-law
// that pollHostVolume uses for the playback side. Reason: the sources
// this attenuates are line-level mic/line inputs, which are already
// quiet. Square-law gives ~-12 dB at 50% slider, dropping a mic below
// audibility; linear gives -6 dB at 50%, keeping usable range across
// the whole slider travel. The playback hostvol still uses square-law
// because it's attenuating the final DAC output which is much hotter —
// the tapers are tuned to the signal levels they act on. Mute folds
// into a hard 0.
static float captureMonitorGain(float rawVol, int rawMute) {
    if (rawMute) return 0.0f;
    if (rawVol < 0.0f) return 0.0f;
    if (rawVol > 1.0f) return 1.0f;
    return rawVol;
}

static void applyCaptureMonitorGain(float g) {
    monLineL.gain(g);
    monLineR.gain(g);
    monMicL.gain(g);
    monMicR.gain(g);
}

static void pollCaptureHostVolume() {
    const float rawVol = AudioOutputUSB::features.volume *
                         (1.0f / (float)FEATURE_MAX_VOLUME);
    const int   rawMute = AudioOutputUSB::features.mute ? 1 : 0;

    const bool valueChanged = (rawVol != s_lastCapVolRaw);
    const bool muteChanged  = (rawMute != s_lastCapMute);

    if (valueChanged) {
        s_lastCapVolRaw = rawVol;
        OSCMessage m("/usb/cap/hostvol/value");
        m.add(rawVol);
        OSCBundle reply;
        reply.add(m);
        g_transport.sendBundle(reply);
    }
    if (muteChanged) {
        s_lastCapMute = rawMute;
        OSCMessage m("/usb/cap/hostvol/mute");
        m.add(rawMute);
        OSCBundle reply;
        reply.add(m);
        g_transport.sendBundle(reply);
    }

    // Push the new gain into all four monitor amps when EITHER value or
    // mute changed — mute is a multiplier so a value-only change still
    // needs the gain re-applied at the current mute state, and vice versa.
    if (valueChanged || muteChanged) {
        applyCaptureMonitorGain(captureMonitorGain(rawVol, rawMute));
    }
}

// ============================================================================
// /snapshot — dump all current state on demand
// ============================================================================
//
// When a dev surface client connects mid-session there's no other way to
// populate its signals — the change-only broadcasts above only fire when
// something moves, and the initial state dump patterns in TDspMixer
// (individual broadcast* helpers) aren't wired together anywhere. This
// function gathers everything a fresh client needs to render the mixer
// correctly and sends it as one bundle.
//
// Triggered by an incoming OSC message at address "/snapshot" (no args).
// The onOscMessage handler intercepts that address before passing to
// g_dispatcher.route() so we don't need to extend OscDispatcher just for
// this sketch-local state (capture-side hostvol).
//
// Contents:
//   * Per-channel: fader, on, solo, name, link (odd channels)
//   * Main bus: faderL, faderR, link, on, hostvol/value
//   * Capture-side hostvol: /usb/cap/hostvol/{value,mute}
//   * Codec panel: whatever Tac5212Panel::snapshot() reads back from
//     the chip (see CodecPanel::snapshot in lib/TDspMixer/) — currently
//     /codec/tac5212/out/N/mode
//
// Meter state is NOT included — clients subscribe to meters separately
// via /sub addSub and the firmware streams them independently.
static void broadcastSnapshot(OSCBundle &reply) {
    // Channel state via the existing dispatcher broadcast helpers.
    for (int n = 1; n <= tdsp::kChannelCount; ++n) {
        g_dispatcher.broadcastChannelFader(n, reply);
        g_dispatcher.broadcastChannelOn(n, reply);
        g_dispatcher.broadcastChannelSolo(n, reply);
        g_dispatcher.broadcastChannelName(n, reply);
    }

    // Main bus state.
    g_dispatcher.broadcastMainFaderL(reply);
    g_dispatcher.broadcastMainFaderR(reply);
    g_dispatcher.broadcastMainLink(reply);
    g_dispatcher.broadcastMainOn(reply);
    g_dispatcher.broadcastMainHostvolValue(reply);

    // /main/st/hostvol/enable has no broadcast helper in the dispatcher
    // yet, emit it inline so clients can render the ENABLE button
    // correctly on reconnect.
    {
        OSCMessage m("/main/st/hostvol/enable");
        m.add((int32_t)(g_model.main().hostvolEnable ? 1 : 0));
        reply.add(m);
    }

    // Capture-side hostvol (sketch-local, not in MixerModel). Read
    // straight from AudioOutputUSB::features so the snapshot always
    // reflects whatever Windows most recently pushed, not the cached
    // s_lastCapVolRaw which could be stale between polls.
    {
        const float v = AudioOutputUSB::features.volume *
                        (1.0f / (float)FEATURE_MAX_VOLUME);
        OSCMessage m("/usb/cap/hostvol/value");
        m.add(v);
        reply.add(m);
    }
    {
        OSCMessage m("/usb/cap/hostvol/mute");
        m.add((int32_t)(AudioOutputUSB::features.mute ? 1 : 0));
        reply.add(m);
    }

    // Codec panel state — currently the Output tab. The panel reads back
    // from the chip via the lib/TAC5212 getters, so the values are the
    // real register state, not a cached "last write". Tabs without
    // getters yet (ADC, VREF/MICBIAS, PDM) are silently skipped — see
    // Tac5212Panel::snapshot() in this directory.
    g_codecPanel.snapshot(reply);
}

// ============================================================================
// loop()
// ============================================================================

void loop() {
    // If the USB CDC host disconnected (Chrome closed the serial
    // port), immediately disable all streaming engines. Without this,
    // stale meter + spectrum subscriptions from a previous session keep
    // trying to write ~36 KB/sec of OSC blobs into a CDC TX buffer
    // that nobody is reading. usb_serial_write() blocks waiting for
    // buffer space → loop() stalls → heartbeat LED stops → device
    // appears frozen. Disabling the engines stops the writes. The
    // client's connect() path re-subscribes after opening the port and
    // starting its readLoop, so the engines come back up cleanly.
    if (!Serial.dtr()) {
        if (g_meters.isEnabled())   g_meters.setEnabled(false);
        if (g_spectrum.isEnabled()) g_spectrum.setEnabled(false);
    }

    pollHostVolume();
    pollCaptureHostVolume();

    g_transport.poll();

    OSCBundle meterReply;
    if (g_meters.tick(meterReply) && meterReply.size() > 0) {
        g_transport.sendBundle(meterReply);
    }

    OSCBundle spectrumReply;
    if (g_spectrum.tick(spectrumReply) && spectrumReply.size() > 0) {
        g_transport.sendBundle(spectrumReply);
    }

    // Heartbeat LED — proves the main loop is running and not wedged.
    static unsigned long lastBlink = 0;
    if (millis() - lastBlink >= 500) {
        lastBlink = millis();
        digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
    }
}
