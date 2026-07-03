// spike_esp32_bt_spdif_mix — mix Bluetooth + S/PDIF into the TAC5212 DAC.
//
//   (A) phone --A2DP--> ESP32 (I2S master, 44.1k) --> Teensy SAI2 slave (pin 5)
//         --> AsyncAudioInput<AsyncAudioInputI2S2_16bitslave>  (BT, async-resampled)
//   (B) tone --> S/PDIF OUT (pin 14 optical) --[loopback cable]--> S/PDIF IN (pin 15)
//         --> AsyncAudioInputSPDIF3  (S/PDIF, async-resampled)
//   mix (A)+(B) --> AudioOutputTDM (SAI1) --> TAC5212 DAC --> OUT1/OUT2.
//
// Two async resamplers coexist: each locks a foreign clock (the ESP32 crystal,
// the looped-back S/PDIF crystal) to the Teensy audio clock, so neither source
// scratches. Proven pieces: BT path = spike_esp32_i2s_dac (lib/TDspAsyncI2S);
// S/PDIF path = spike_spdif_alex_dac (lib/Audio AsyncAudioInputSPDIF3).
//
// Wiring (teensy41_digital_audio_board):
//   ESP32 BCLK(GPIO26)->pin4  WS(GPIO16)->pin3  DOUT(GPIO25)->pin5 (SAI2 slave in)
//   TAC5212 DAC <- SAI1 TDM (pins 21/20/23 clocks, 7 = OUT1A data)
//   Optical TOSLINK: pin 14 (S/PDIF OUT) -> pin 15 (S/PDIF IN) loopback cable

#include <Arduino.h>
#include <Wire.h>
#include "tdsp_hw_config.h"
#include <Audio.h>
#include <TAC5212.h>
#include <TDspEsp32.h>
// BT async I2S resampler (ESP32 free-running clock -> Teensy rate). See
// lib/TDspAsyncI2S/async_input.h and project memory reference_async_i2s_resampler.
#include "async_input.h"
#include "input_i2s2_16bit.h"

constexpr int     TAC5212_EN_PIN      = 35;     // shared SHDNZ, active-low
constexpr uint8_t TAC5212_I2C_ADDRESS = 0x51;

// --- Audio graph -----------------------------------------------------------
// Declaration order matters: tdmClk (SAI1 TDM input) is constructed FIRST so it
// owns update_responsibility (an input drives update_all(); a lone output does
// not). It ties the graph's update rate to the codec's SAI1 clock domain. The
// two async inputs and the S/PDIF output run their update() when called but
// never claim update-responsibility.
AudioInputTDM          tdmClk;               // SAI1 TDM clock + update driver
AudioOutputTDM         tdmOut;               // SAI1 TDM -> TAC5212 DAC (ch0=L, ch1=R)

// (B) S/PDIF optical self-test loop. spdifIn is async (never drives update);
// spdifOut sends the tone out the optical port. Same params as spike_spdif_alex_dac.
AsyncAudioInputSPDIF3  spdifIn(false, false, 100, 20, 80);  // optical IN, pin 15
AudioOutputSPDIF3      spdifOut;                            // optical OUT, pin 14
AudioSynthWaveformSine spdifTone;                          // tone sent out the optical port

// (A) Bluetooth: SAI2 slave <- ESP32 I2S (pin 5), async-resampled ESP32 -> Teensy rate.
AsyncAudioInput<AsyncAudioInputI2S2_16bitslave> btIn(false, false, 100, 20, 80);

AudioSynthWaveformSine testTone;             // local DAC self-test source
AudioMixer4            outL, outR;           // mix: 0=BT, 1=local tone, 2=S/PDIF-in
AudioAnalyzePeak       peakBt;               // Bluetooth input level
AudioAnalyzePeak       peakSpdif;            // S/PDIF input level
AudioAnalyzePeak       peakOut;              // signal into the DAC

// tone -> S/PDIF OUT (the loopback source)
AudioConnection c_txL    (spdifTone, 0, spdifOut, 0);
AudioConnection c_txR    (spdifTone, 0, spdifOut, 1);
// mixer input 0 = Bluetooth, 1 = local test tone, 2 = S/PDIF-in
AudioConnection c_btL    (btIn,      0, outL, 0);
AudioConnection c_btR    (btIn,      1, outR, 0);
AudioConnection c_toneL  (testTone,  0, outL, 1);
AudioConnection c_toneR  (testTone,  0, outR, 1);
AudioConnection c_spL    (spdifIn,   0, outL, 2);
AudioConnection c_spR    (spdifIn,   1, outR, 2);
// mix -> DAC (TDM ch0->slot0->OUT1/L, ch1->slot1->OUT2/R)
AudioConnection c_outL   (outL,      0, tdmOut, 0);
AudioConnection c_outR   (outR,      0, tdmOut, 1);
// meters
AudioConnection c_pkBt   (btIn,      0, peakBt,    0);
AudioConnection c_pkSp   (spdifIn,   0, peakSpdif, 0);
AudioConnection c_pkOut  (outL,      0, peakOut,   0);

tac5212::TAC5212 g_codec(Wire);

// ESP32 control/monitor over the no-touch link (Serial7 + EN/IO0). Pins 28/29/
// 36/37 don't overlap the audio pins, so it coexists with the audio graph. Reset
// the ESP32 into its A2DP app on boot and mirror its serial log to USB.
TDspEsp32 esp;
elapsedMillis hb;

static void hardResetCodecPower() {
    pinMode(TAC5212_EN_PIN, OUTPUT);
    digitalWrite(TAC5212_EN_PIN, LOW);  delay(5);
    digitalWrite(TAC5212_EN_PIN, HIGH); delay(10);
}

// I2C bus recovery: if a slave (or the ESP32 sharing SDA/SCL while it brown-out-
// loops) is holding SDA low, Wire.begin()+transactions can block forever and hang
// setup() (solid LED, no heartbeat). Before Wire.begin() we bit-bang: pulse SCL up
// to 9 times to clock the stuck slave past its byte, then issue a STOP. Teensy
// Wire0 pins: SDA=18, SCL=19. Bounded (no infinite loop) so setup() always proceeds.
static void i2cBusRecover(uint8_t sdaPin = 18, uint8_t sclPin = 19) {
    pinMode(sclPin, INPUT_PULLUP);
    pinMode(sdaPin, INPUT_PULLUP);
    delayMicroseconds(10);
    if (digitalRead(sdaPin) == HIGH) return;      // bus already free
    for (int i = 0; i < 9 && digitalRead(sdaPin) == LOW; ++i) {
        pinMode(sclPin, OUTPUT);
        digitalWrite(sclPin, LOW);  delayMicroseconds(5);
        pinMode(sclPin, INPUT_PULLUP);            // release, let it rise
        delayMicroseconds(5);
    }
    // STOP: SDA low->high while SCL high
    pinMode(sdaPin, OUTPUT); digitalWrite(sdaPin, LOW); delayMicroseconds(5);
    pinMode(sclPin, INPUT_PULLUP);                delayMicroseconds(5);
    pinMode(sdaPin, INPUT_PULLUP);                delayMicroseconds(5);
}

static bool g_codecOk = false;
static const char *g_codecMsg = "not run";

// Bridge mode: hand the USB serial to the ESP32 (esptool passthrough) so the
// ESP32 can be reflashed through the Teensy. Set by 'b'; cleared by Teensy reset.
static bool g_bridge = false;

// Output volume (dB), applied to both DAC channels. -20 dB default = safe level.
static float g_dvol = -20.0f;
static void applyVol() {
    g_codec.out(1).setDvol(g_dvol);
    g_codec.out(2).setDvol(g_dvol);
}

FLASHMEM static void setupCodec() {
    Serial.println("Init TAC5212 (TDM, HP out)...");
    tdspMuxAutoSelectCodec(TAC5212_I2C_ADDRESS);
    tac5212::Result r = g_codec.begin(TAC5212_I2C_ADDRESS);
    g_codecOk = !r.isError();
    g_codecMsg = r.isError() ? (r.message ? r.message : "unknown") : "ok";
    if (r.isError()) { Serial.print("  begin failed: ");
        Serial.println(r.message ? r.message : "(unknown)"); return; }

    tac5212::TAC5212::SerialFormat sf;
    sf.format  = tac5212::TAC5212::Format::Tdm;      // proven stereo path (f32 shield)
    sf.wordLen = tac5212::TAC5212::WordLen::Bits16;  // match stock 16-bit TDM slots
    g_codec.setSerialFormat(sf);

    // Board bodge: DIN/DOUT swapped+shorted -> disable codec DOUT so it can't
    // fight the Teensy TX; DIN stays enabled (output-only).
    g_codec.writeRegister(0, /*INTF_CFG1*/ 0x10, 0x00);

    g_codec.setRxSlotOffset(1);
    g_codec.setRxChannelSlot(1, 0);              // DAC CH1 (OUT1) <- slot 0 (L)
    g_codec.setRxChannelSlot(2, 1);              // DAC CH2 (OUT2) <- slot 1 (R)
    g_codec.out(1).setMode(tac5212::OutMode::HpDriver);
    g_codec.out(2).setMode(tac5212::OutMode::HpDriver);
    g_codec.out(1).setDvol(-128.0f);             // boot-muted
    g_codec.out(2).setDvol(-128.0f);
    g_codec.setChannelEnable(/*inMask=*/0x0, /*outMask=*/0xC);
    g_codec.powerDac(true);
    delay(100);
    g_codec.setDspAvddSelect(true);
}

// mixer helper: 0=BT, 1=local test tone, 2=S/PDIF-in
static void setMix(float bt, float tone, float spdif) {
    outL.gain(0, bt);   outR.gain(0, bt);
    outL.gain(1, tone); outR.gain(1, tone);
    outL.gain(2, spdif); outR.gain(2, spdif);
}

// Robustly boot the ESP32 into its app over the Teensy strap pins (EN=37, IO0=36).
// Two board realities force this exact sequence:
//   * EN and IO0 each have a 0.1uF cap (C31/C32) -> slow edges, so hold EN low LONG.
//   * When the ESP32's own USB is unplugged its CP210x is DEAD and pulls IO0 toward
//     download mode. So the Teensy must KEEP DRIVING IO0 HIGH -- through the reset,
//     through the EN release, and forever after (never hi-Z / esp.release() it) --
//     or the ESP32 samples IO0 low and drops into ROM download instead of the app.
static void espBootApp(const char *why) {
    // Give the ESP32 ONE clean reset-into-app pulse, then get off its strap pins.
    esp.begin();               // claim EN/IO0 as OUTPUT + open Serial7
    esp.setBootLow(false);     // IO0 HIGH = "boot the app" strap
    delay(100);                // settle IO0 high against the 0.1uF cap
    esp.assertReset(true);     // EN LOW = reset
    delay(300);                // hold (caps + reset min)
    esp.assertReset(false);    // EN HIGH = release -> app boots (IO0 high)
    delay(100);                // let it sample the strap while still driven
    // CRITICAL (per hardware): RELEASE EN/IO0 to hi-Z during normal operation. The
    // ESP32's own pull-ups hold EN/IO0 high (runs the app), and nothing fights the
    // CP210x auto-reset or the BOOT/EN buttons -> clean flashing + clean reset. The
    // Teensy re-claims these pins only in bridge mode. (release() keeps Serial7 up.)
    esp.release();
    Serial.printf("[esp] boot-into-app (%s) done -> EN/IO0 released to hi-Z (ESP32 free-runs)\n", why);
}

void setup() {
    hardResetCodecPower();
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);

    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 1500) {}

    Serial.println();
    Serial.println("=== spike_esp32_bt_spdif_mix ===");
    Serial.println("MIX: (A) ESP32 A2DP  +  (B) S/PDIF optical loopback tone  -> TAC5212.");
    Serial.println("Connect a TOSLINK cable pin14(OUT)->pin15(IN). Pair 'T-DSP' and play.");

    // Boot the ESP32 into its A2DP app FIRST -- this is the order that reliably ran
    // the ESP32 in early bring-up. A clean reset pulse with IO0 high boots the app;
    // then RELEASE the EN/IO0 straps to hi-Z so the ESP32 runs free on its own
    // pull-ups (no Teensy push-pull fighting its reset button / CP210x). Give it a
    // moment to boot and free the shared I2C bus. (Holding it in reset across codec
    // init instead -- the previous approach -- left it stuck and silent.)
    Serial.println("[setup] esp boot into app (long reset, IO0 held)..."); Serial.flush();
    espBootApp("boot");
    delay(300);              // let the ESP32 boot its app and release the shared I2C bus

    // Recover a still-wedged bus BEFORE Wire.begin(). Breadcrumbs + flush so the
    // serial log shows the last step reached if anything still stalls.
    Serial.println("[setup] i2c bus recover..."); Serial.flush();
    i2cBusRecover();
    // GUARANTEE loop() (and thus bridge mode) is always reachable: if SDA is STILL
    // held low after recovery, the bus is wedged (typically the ESP32 holding it
    // during a brown-out). Skip the mux/codec init entirely -- those Wire calls
    // would block forever. Audio graph + USB commands + bridge still run; recover
    // the codec later with 'i' once the bus is free (e.g. after bridge-flashing the
    // ESP32, which holds it in reset and releases the bus).
    pinMode(18 /*SDA0*/, INPUT_PULLUP); delayMicroseconds(20);
    if (digitalRead(18) == LOW) {
        g_codecOk = false; g_codecMsg = "i2c wedged - skipped";
        Serial.println("[setup] !! I2C SDA STILL LOW -> SKIP codec init. "
                       "Bridge/audio still run; use 'i' after the bus frees.");
        Serial.flush();
    } else {
        Serial.println("[setup] Wire.begin..."); Serial.flush();
        Wire.begin();
        Wire.setClock(100000);
        Serial.println("[setup] mux select..."); Serial.flush();
        tdspMuxAutoSelectCodec(TAC5212_I2C_ADDRESS);
        Serial.println("[setup] codec init..."); Serial.flush();
        setupCodec();
        Serial.println("[setup] codec init done"); Serial.flush();
    }

    Serial.println("ESP32 already booting its A2DP app. Its serial log follows, prefixed [esp]:");

    AudioMemory(60);                 // two async resamplers + S/PDIF out + TDM + mixers
    setMix(1.0f, 0.0f, 1.0f);        // default: BT + S/PDIF mixed, local tone off
    testTone.frequency(440.0f);
    testTone.amplitude(0.0f);
    spdifTone.frequency(1000.0f);    // the tone we send out the optical port
    spdifTone.amplitude(0.25f);

    if (g_codecOk) applyVol();         // unmute (skip if codec/I2C was wedged -> would hang)

    Serial.println("running -- cmds: t=DACtone a=BT+SPDIF mix  s=SPDIF-only  m=BT-only");
    Serial.println("                 x=toggle SPDIF tone  r=reset ESP32  b=bridge  +/-=vol  d=dump");
    Serial.println("                 P=ESP32 pairing mode  F=ESP32 forget bond + pair");
}

void loop() {
    // Bridge mode: USB <-> ESP32 passthrough for esptool. Reset the Teensy to exit.
    if (g_bridge) { esp.bridgeTask(Serial); return; }

    if (Serial.available()) {
        int c = Serial.read();
        if (c == 'r') { Serial.println("[cmd] resetting ESP32 -> app"); espBootApp("cmd"); }
        else if (c == 'P') { esp.uart().write('p'); Serial.println("[cmd] -> ESP32: ENTER pairing mode"); }
        else if (c == 'F') { esp.uart().write('f'); Serial.println("[cmd] -> ESP32: FORGET bond + pairing mode"); }
        else if (c == 'b') {
            Serial.println("[cmd] BRIDGE MODE: USB <-> ESP32 (esptool @115200). "
                           "Same USB device -> no re-enumeration. Reset the Teensy to return to audio.");
            Serial.flush();
            esp.begin();       // re-claim EN/IO0 as OUTPUT so bridgeTask can drive reset
                               // (they are hi-Z during normal operation, see espBootApp)
            // RELIABILITY: raise the Serial7 UART ISR ABOVE the audio SAI/SPDIF DMA
            // ISRs (default prio 128; update_all() runs inside the DMA ISR, see
            // input_tdm.cpp). Otherwise a DMA ISR delays the UART service, its small
            // hardware FIFO overflows, and a byte is dropped mid-flash ("No more
            // data" / "Invalid head of packet"). 64 < 128 so the UART preempts the
            // audio ISRs and its FIFO is always serviced in time. We reset the Teensy
            // to leave bridge mode, so there's no need to restore the priority.
            NVIC_SET_PRIORITY(IRQ_LPUART7, 64);
            g_bridge = true;
        }
        else if (c == 't') {          // local DAC self-test tone only
            testTone.amplitude(0.4f);
            setMix(0.0f, 1.0f, 0.0f);
            Serial.println("[cmd] local DAC tone 440Hz -> BOTH (BT+SPDIF muted)");
        }
        else if (c == 'a') {          // BT + S/PDIF mix (default)
            testTone.amplitude(0.0f);
            setMix(1.0f, 0.0f, 1.0f);
            Serial.println("[cmd] MIX: BT + S/PDIF");
        }
        else if (c == 's') {          // S/PDIF-in only
            testTone.amplitude(0.0f);
            setMix(0.0f, 0.0f, 1.0f);
            Serial.println("[cmd] S/PDIF-in only");
        }
        else if (c == 'm') {          // Bluetooth only
            testTone.amplitude(0.0f);
            setMix(1.0f, 0.0f, 0.0f);
            Serial.println("[cmd] Bluetooth only");
        }
        else if (c == 'x') {          // toggle the outgoing S/PDIF tone
            static bool on = true; on = !on;
            spdifTone.amplitude(on ? 0.25f : 0.0f);
            Serial.printf("[cmd] S/PDIF out tone %s\n", on ? "ON (0.25)" : "OFF");
        }
        else if (c == '+') { g_dvol += 3.0f; if (g_dvol > 0) g_dvol = 0;
            applyVol(); Serial.printf("[cmd] vol %.0f dB\n", g_dvol); }
        else if (c == '-') { g_dvol -= 3.0f; if (g_dvol < -60) g_dvol = -60;
            applyVol(); Serial.printf("[cmd] vol %.0f dB\n", g_dvol); }
        else if (c == 'd') {          // dump codec RX/out regs
            Serial.printf("[reg] RX_OFF(26)=%02X RX_CH1(28)=%02X RX_CH2(29)=%02X "
                          "CH_EN(76)=%02X PWR(78)=%02X\n",
                          g_codec.readRegister(0, 0x26), g_codec.readRegister(0, 0x28),
                          g_codec.readRegister(0, 0x29), g_codec.readRegister(0, 0x76),
                          g_codec.readRegister(0, 0x78));
        }
        else if (c == 'i') {          // re-init codec
            Serial.println("[cmd] re-init codec");
            setupCodec(); applyVol();
            Serial.printf("[cmd] codec=%s (%s), vol %.0f dB\n",
                          g_codecOk ? "OK" : "FAIL", g_codecMsg, g_dvol);
        }
    }

    // Mirror the ESP32's UART log to USB, line-buffered with an [esp] prefix.
    static char line[160];
    static size_t n = 0;
    while (esp.uart().available()) {
        char c = (char)esp.uart().read();
        if (c == '\n' || n >= sizeof(line) - 1) {
            line[n] = 0;
            if (n) Serial.printf("[esp] %s\n", line);
            n = 0;
        } else if (c != '\r') {
            line[n++] = c;
        }
    }

    if (hb >= 1000) {
        hb = 0;
        digitalToggle(LED_BUILTIN);
        float pbt = peakBt.available()    ? peakBt.read()    : 0.0f;
        float psp = peakSpdif.available() ? peakSpdif.read() : 0.0f;
        float po  = peakOut.available()   ? peakOut.read()   : 0.0f;
        Serial.printf("alive up=%lus  codec=%s(%s)  spdif=%s inFreq=%.0f  "
                      "btPeak=%.3f spdifPeak=%.3f outPeak=%.3f  cpuMax=%.1f%% memMax=%u\n",
                      (unsigned long)(millis() / 1000),
                      g_codecOk ? "OK" : "FAIL", g_codecMsg,
                      AsyncAudioInputSPDIF3::isLocked() ? "LOCKED" : "no-signal",
                      spdifIn.getInputFrequency(),
                      pbt, psp, po,
                      AudioProcessorUsageMax(), AudioMemoryUsageMax());
    }
}
