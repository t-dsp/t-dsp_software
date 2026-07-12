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
static bool g_flash  = false;   // Teensy-driven download + raw passthrough (no DTR emulation)

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
    // HARDWARE BUG this works around (see schematic): IO0 has no proper pull-up
    // (only the ESP32's weak internal ~45k) but carries C32 (0.1uF), so at COLD
    // power-up IO0 rises ~4.5ms while EN rises ~1ms -> the ESP32 samples IO0 LOW at
    // the EN edge and cold-boots into ROM DOWNLOAD mode (silent, no "T-DSP"). A reset
    // at steady state (IO0 already 3.3V) boots the app fine -- hence "reset works,
    // power-cycle fails". The real fix is a 10k pull-up on IO0; this is the firmware
    // belt-and-suspenders so it recovers on boot without the resistor.
    esp.begin();               // claim EN/IO0 (OUTPUT) + open Serial7
    esp.setBootLow(false);     // drive IO0 HIGH now, and HOLD it through both resets
    delay(400);                // let the 5V/3V3 rail FULLY settle before resetting
    for (int i = 0; i < 2; ++i) {      // reset into the app TWICE (insurance)
        esp.assertReset(true);         // EN LOW  (pin37 LOW)
        delay(1000);                   // hold LONG (0.1uF caps + reset min) -- generous
        esp.assertReset(false);        // EN HIGH (pin37 HIGH) -> boots app; IO0 held HIGH
        delay(250);                    // POR latches IO0=1 (app), not download
    }
    // HARDWARE BUG #2 (metered 2026-07-03): ESP32_EN has NO working pull-up on this
    // board -- when the Teensy stops driving pin37, EN collapses to 0V and the ESP32
    // drops straight back into reset. (pin37 HIGH -> EN 3.3V; released -> EN 0V.) So we
    // must NOT release: KEEP driving EN HIGH + IO0 HIGH forever to hold the chip out of
    // reset and in the app. (Bridge mode re-claims the pins for flashing.)
    esp.setBootLow(false);             // IO0 HIGH, held
    esp.assertReset(false);            // EN  HIGH, held (pin37 stays OUTPUT HIGH)
    Serial.printf("[esp] boot-into-app (%s): 2x EN-reset, then EN+IO0 driven HIGH and HELD (no release)\n", why);
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

    // LATE, SETTLED reset -- the automatic "press BOOT for you". At cold power-up the
    // board's buses/peripherals ramp together and can pull ESP32 boot straps wrong, so
    // it boots into download mode ("power-cycle fails, manual reset works"). Now that
    // the Teensy + all its pins/buses are configured and the rail is settled, reset the
    // ESP32 into its app -- same conditions as the manual BOOT/EN tap that works.
    Serial.println("[setup] settle 2.5s, then LATE ESP32 reset (auto 'press BOOT')...");
    Serial.flush();
    delay(2500);
    espBootApp("post-setup settle");
}

// Self-reset the Teensy (returns to setup() -> espBootApp boots the ESP32 into its app).
// Used to leave flash/bridge mode without a physical power-cycle.
static void rebootTeensy() { SCB_AIRCR = 0x05FA0004; while (1) {} }

// Fast LED blink to signal "in bridge/flash passthrough" (vs the slow run-mode heartbeat).
static inline void flashModeBlink() {
    static uint32_t t = 0; static bool on = false;
    if ((uint32_t)(millis() - t) >= 70) { t = millis(); on = !on; digitalWriteFast(LED_BUILTIN, on); }
}

void loop() {
    // Flash mode: the Teensy already put the ESP32 in download; raw USB<->Serial7
    // passthrough with NO reset emulation (esptool runs --before no_reset).
    if (g_flash) {
        flashModeBlink();                 // rapid heartbeat = in flash passthrough
        // USB -> ESP32, watching for the soft-reboot escape token "@BOOTAPP@" so we can
        // leave flash mode + boot the ESP32 into its app WITHOUT a physical power-cycle.
        static const char MAGIC[] = "@BOOTAPP@";
        static uint8_t mi = 0;
        while (Serial.available()) {
            uint8_t b = (uint8_t)Serial.read();
            esp.uart().write(b);
            if (b == (uint8_t)MAGIC[mi]) { if (MAGIC[++mi] == '\0') rebootTeensy(); }
            else mi = (b == (uint8_t)MAGIC[0]) ? 1 : 0;
        }
        while (esp.uart().available()) Serial.write((uint8_t)esp.uart().read());
        return;
    }
    // Bridge mode: USB <-> ESP32 passthrough for esptool. Reset the Teensy to exit.
    if (g_bridge) { flashModeBlink(); esp.bridgeTask(Serial); return; }

    if (Serial.available()) {
        int c = Serial.read();
        if (c == 'r') { Serial.println("[cmd] resetting ESP32 -> app"); espBootApp("cmd"); }
        else if (c == 'z') {   // DIAGNOSTIC: drive EN (pin 37) LOW as a plain GPIO, hold 5s
            Serial.println("[test] pin37(EN) -> OUTPUT, LOW, hold 5s. Meter: Teensy pin37 pad "
                           "should read ~0V; ESP32 EN ~0V if the trace is good. If the ESP32 is "
                           "streaming, its audio should DROP the whole 5s if 37->EN works.");
            Serial.flush();
            pinMode(37, OUTPUT); digitalWrite(37, LOW);
            delay(5000);
            digitalWrite(37, HIGH); delayMicroseconds(50); pinMode(37, INPUT);  // release hi-Z
            Serial.println("[test] pin37 released (hi-Z).");
        }
        else if (c == 'y') {   // DIAGNOSTIC: drive EN (pin 37) HIGH, hold 5s (test INVERTED EN)
            Serial.println("[test] pin37 -> OUTPUT, HIGH, hold 5s. If EN is INVERTED, the ESP32's "
                           "EN goes LOW -> audio should CUT OUT the whole 5s, EN meter ~0V.");
            Serial.flush();
            pinMode(37, OUTPUT); digitalWrite(37, HIGH);
            delay(5000);
            digitalWrite(37, LOW); delayMicroseconds(50); pinMode(37, INPUT);
            Serial.println("[test] pin37 released (hi-Z).");
        }
        else if (c == 'Z') {   // DIAGNOSTIC: same for IO0 (pin 36)
            Serial.println("[test] pin36(IO0) -> OUTPUT, LOW, hold 5s (meter Teensy pin36 & ESP32 IO0).");
            Serial.flush();
            pinMode(36, OUTPUT); digitalWrite(36, LOW);
            delay(5000);
            digitalWrite(36, HIGH); delayMicroseconds(50); pinMode(36, INPUT);
            Serial.println("[test] pin36 released (hi-Z).");
        }
        else if (c == 'P') { esp.uart().write('p'); Serial.println("[cmd] -> ESP32: ENTER pairing mode"); }
        else if (c == 'F') { esp.uart().write('f'); Serial.println("[cmd] -> ESP32: FORGET bond + pairing mode"); }
        else if (c == 'U') {   // jump to HalfKay bootloader (Teensy PROGRAM mode) -- no button
            Serial.println("[cmd] -> Teensy PROGRAM MODE (HalfKay). Upload now: teensy_loader_cli -w (no button).");
            Serial.flush();
            delay(50);
            _reboot_Teensyduino_();        // software jump into the bootloader
        }
        else if (c == 'g') {   // Teensy-driven DOWNLOAD entry + raw passthrough (deterministic flash)
            Serial.println("[cmd] FLASH MODE: Teensy drove ESP32 into DOWNLOAD; raw passthrough now. "
                           "Run: esptool --before no_reset --after no_reset --baud 115200. "
                           "Reset the Teensy to return to audio.");
            Serial.flush();
            NVIC_SET_PRIORITY(IRQ_LPUART7, 64);   // UART ISR above audio DMA (see 'b')
            // Teensy drives IO0 low across an EN pulse -> ROM download. IO0 is native on
            // pin36; EN reaches the chip via the pin37->EN jumper. VERIFY + RETRY: a manual
            // jumper can be intermittent, so re-pulse until the ROM actually talks. (No
            // esp.begin() here -- Serial7 is already open; re-init could eat the banner.)
            bool inDl = false;
            for (int a = 1; a <= 5 && !inDl; a++) {
                pinMode(36, OUTPUT); digitalWrite(36, LOW);   // IO0 LOW (select download)
                pinMode(37, OUTPUT); digitalWrite(37, LOW);   // EN LOW (reset)
                delay(400);                                   // hold (EN cap C31)
                while (esp.uart().available()) esp.uart().read();   // flush pre-reset noise
                digitalWrite(37, HIGH);                       // EN HIGH, IO0 low -> latch download
                int n = 0; uint32_t t0 = millis();
                while ((uint32_t)(millis() - t0) < 700) {     // let the ROM banner arrive
                    while (esp.uart().available()) { esp.uart().read(); n++; }
                }
                digitalWrite(36, HIGH);                       // release IO0 (strap already latched)
                Serial.printf("[g] reset attempt %d: %d ROM bytes\n", a, n);
                inDl = (n > 4);                               // ROM spoke -> reset landed
            }
            Serial.println(inDl ? "[g] ESP32 in DOWNLOAD -- passthrough ON."
                                : "[g] WARN: no ROM response (check EN jumper) -- passthrough ON anyway.");
            AudioNoInterrupts();   // halt the heavy audio update so loop()/passthrough
                                   // isn't CPU-starved (cpuMax was ~213%) -> no byte drops
            g_flash = true;
        }
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
