// spike_midi_ymfm_opm — MIDI-IN driving a ymfm YM2151 (OPM) FM synth.
//
//   MIDI DIN/TRS --opto(H11L1)--> pin 0 (Serial1 RX, 31250)
//     --> Arduino MIDI Library --> AudioSynthYmfmOPM (ymfm ym2151, 8ch x 4-op FM)
//     --> AudioOutputTDM (SAI1, int16) --> TAC5212 DAC --> OUT1/OUT2 (HP jack).
//
// The codec / TDM / ESP32-kit setup is copied verbatim from spike_midi_dexed
// (known-good on this board), so the only new moving parts are the OPM engine and
// its MIDI->register layer — if audio is silent it's the ymfm code, not plumbing.
//
// Bring-up path:
//   1. 'n' plays a local OPM note (middle C) — proves audio+OPM with NO MIDI.
//   2. 'c' plays a 4-note chord — proves the 8-voice allocator / polyphony.
//   3. 'W' plays a C-major scale demo.  'v' toggles the two starter voices.
//   4. Play a MIDI keyboard into the DIN — notes should sound + count in heartbeat.
//
// ESP32/kit:  r=reset->app  g=flash mode  @BOOTAPP@=exit flash  U=Teensy prog.

#include <Arduino.h>
#include <Wire.h>
#include "tdsp_hw_config.h"
#include <Audio.h>
#include <TAC5212.h>
#include <TDspProgrammingKit.h>
#include <MIDI.h>
#include <AudioSynthYmfmOPM.h>

constexpr int     TAC5212_EN_PIN      = 35;     // shared SHDNZ, active-low
constexpr uint8_t TAC5212_I2C_ADDRESS = 0x51;

// Physical MIDI IN on the schematic's MIDI_RX (Teensy pin 0 = Serial1 RX)
// through the H11L1 optoisolator. The library drives Serial1 at 31250 baud.
MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI);

// --- Audio graph ------------------------------------------------------------
// tdmClk (SAI1 TDM input) is constructed FIRST so it owns update_responsibility
// exactly as in spike_midi_dexed; tdmOut is the actual DAC feed. tdmClk is left
// unconnected — it exists only to keep the SAI1 clock/update wiring identical.
AudioInputTDM          tdmClk;               // SAI1 TDM clock + update driver
AudioOutputTDM         tdmOut;               // SAI1 TDM -> TAC5212 DAC (ch0=L, ch1=R)

AudioSynthYmfmOPM      g_opm;                // ymfm YM2151 (stereo: 0=L, 1=R)
AudioMixer4            outL, outR;           // slot 0 = OPM L / R
AudioAnalyzePeak       peakOut;

AudioConnection c_opmL  (g_opm,   0, outL, 0);
AudioConnection c_opmR  (g_opm,   1, outR, 0);
AudioConnection c_outL  (outL,    0, tdmOut, 0);
AudioConnection c_outR  (outR,    0, tdmOut, 1);
AudioConnection c_pkOut (outL,    0, peakOut, 0);

tac5212::TAC5212 g_codec(Wire);

// ESP32 control/flash — reusable kit (EN=37, IO0=36, Serial7). Non-overlapping
// pins; kept for touch-free Teensy flashing + LED heartbeat + ESP32 boot.
TDspProgrammingKit kit;
elapsedMillis hb;

// --- OPM / MIDI state -------------------------------------------------------
static const tdsp::ymfmopm::OpmVoice *kVoices[] = {
    &tdsp::ymfmopm::kAdditiveOrgan,
    &tdsp::ymfmopm::kElectricPiano,
};
static const int kNumVoices  = sizeof(kVoices) / sizeof(kVoices[0]);
static int       g_voiceIdx  = 0;
static uint32_t  g_noteOnCount = 0;      // lifetime note-on tally (heartbeat)

static void hardResetCodecPower() {
    pinMode(TAC5212_EN_PIN, OUTPUT);
    digitalWrite(TAC5212_EN_PIN, LOW);  delay(5);
    digitalWrite(TAC5212_EN_PIN, HIGH); delay(10);
}

// I2C bus recovery: bit-bang SCL to free a stuck slave before Wire.begin(), so
// setup() can never hang. Wire0: SDA=18, SCL=19. (Verbatim from spike_midi_dexed.)
static void i2cBusRecover(uint8_t sdaPin = 18, uint8_t sclPin = 19) {
    pinMode(sclPin, INPUT_PULLUP);
    pinMode(sdaPin, INPUT_PULLUP);
    delayMicroseconds(10);
    if (digitalRead(sdaPin) == HIGH) return;
    for (int i = 0; i < 9 && digitalRead(sdaPin) == LOW; ++i) {
        pinMode(sclPin, OUTPUT);
        digitalWrite(sclPin, LOW);  delayMicroseconds(5);
        pinMode(sclPin, INPUT_PULLUP);
        delayMicroseconds(5);
    }
    pinMode(sdaPin, OUTPUT); digitalWrite(sdaPin, LOW); delayMicroseconds(5);
    pinMode(sclPin, INPUT_PULLUP);                delayMicroseconds(5);
    pinMode(sdaPin, INPUT_PULLUP);                delayMicroseconds(5);
}

static bool g_codecOk = false;
static const char *g_codecMsg = "not run";

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
    sf.format  = tac5212::TAC5212::Format::Tdm;
    sf.wordLen = tac5212::TAC5212::WordLen::Bits16;
    g_codec.setSerialFormat(sf);
    g_codec.writeRegister(0, /*INTF_CFG1*/ 0x10, 0x00);   // board bodge: disable codec DOUT
    g_codec.setRxSlotOffset(1);
    g_codec.setRxChannelSlot(1, 0);
    g_codec.setRxChannelSlot(2, 1);
    g_codec.out(1).setMode(tac5212::OutMode::HpDriver);
    g_codec.out(2).setMode(tac5212::OutMode::HpDriver);
    g_codec.out(1).setDvol(-128.0f);
    g_codec.out(2).setDvol(-128.0f);
    g_codec.setChannelEnable(/*inMask=*/0x0, /*outMask=*/0xC);
    g_codec.powerDac(true);
    delay(100);
    g_codec.setDspAvddSelect(true);
}

static void applyVoice(int idx) {
    if (idx < 0) idx = kNumVoices - 1;
    if (idx >= kNumVoices) idx = 0;
    g_voiceIdx = idx;
    g_opm.allNotesOff();
    g_opm.setVoice(*kVoices[idx]);
    Serial.printf("[opm] voice %d = %s\n", idx, kVoices[idx]->name);
}

// --- MIDI handlers (Serial1 DIN) --------------------------------------------
static void onNoteOn(byte ch, byte note, byte vel) {
    if (vel == 0) { g_opm.noteOff(note); return; }   // running-status note-off
    g_opm.noteOn(note, vel);
    g_noteOnCount++;
    Serial.printf("[midi] noteOn  ch%-2d n%-3d v%-3d\n", ch, note, vel);
}
static void onNoteOff(byte ch, byte note, byte /*vel*/) {
    g_opm.noteOff(note);
    Serial.printf("[midi] noteOff ch%-2d n%-3d\n", ch, note);
}
static void onControlChange(byte /*ch*/, byte cc, byte val) {
    if (cc == 123 && val == 0) g_opm.allNotesOff();      // all notes off
}

// Blocking C-major scale demo (interruptible by any serial key).
static bool delayOrKey(uint16_t ms) {
    uint32_t t0 = millis();
    while (millis() - t0 < ms) { if (Serial.available()) { Serial.read(); return true; } }
    return false;
}
static void playScale() {
    static const uint8_t scale[] = { 60, 62, 64, 65, 67, 69, 71, 72 };  // C4..C5
    Serial.println("[cmd] C-major scale (press any key to stop)");
    for (uint8_t n : scale) {
        g_opm.noteOn(n, 100);
        if (delayOrKey(220)) { g_opm.allNotesOff(); return; }
        g_opm.noteOff(n);
    }
}

void setup() {
    hardResetCodecPower();

    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 1500) {}

    Serial.println();
    if (CrashReport) { Serial.println("!!! CRASH REPORT (previous run) !!!"); Serial.print(CrashReport); }
    Serial.println("=== spike_midi_ymfm_opm (MIDI-IN -> ymfm YM2151/OPM -> TAC5212) ===");
    Serial.println("Physical MIDI IN on pin 0 (Serial1 RX, 31250) via the H11L1 opto.");

    // Pause audio while flashing the ESP32 (kit passthrough must not be starved).
    kit.onFlashEnter([] { AudioNoInterrupts(); });

    // Boot the ESP32 into its app FIRST (frees the shared I2C bus); also LED heartbeat.
    Serial.println("[setup] kit.begin() -> boot ESP32 (EN+IO0 held)..."); Serial.flush();
    kit.begin();
    delay(300);

    Serial.println("[setup] i2c bus recover..."); Serial.flush();
    i2cBusRecover();
    pinMode(18 /*SDA0*/, INPUT_PULLUP); delayMicroseconds(20);
    if (digitalRead(18) == LOW) {
        g_codecOk = false; g_codecMsg = "i2c wedged - skipped";
        Serial.println("[setup] !! I2C SDA STILL LOW -> SKIP codec init. Use 'i' later.");
        Serial.flush();
    } else {
        Serial.println("[setup] Wire.begin..."); Serial.flush();
        Wire.begin();
        Wire.setClock(100000);
        tdspMuxAutoSelectCodec(TAC5212_I2C_ADDRESS);
        setupCodec();
        Serial.println("[setup] codec init done"); Serial.flush();
    }

    AudioMemory(40);
    outL.gain(0, 1.0f);  outR.gain(0, 1.0f);
    if (g_codecOk) applyVol();

    // Bring up the OPM: reset the chip, compute the resample ratio, load a voice.
    g_opm.begin();
    applyVoice(g_voiceIdx);

    // MIDI: consume all channels (omni). Note-off is handled inside onNoteOn too
    // (vel==0) for controllers that use running-status note-offs.
    MIDI.begin(MIDI_CHANNEL_OMNI);
    MIDI.turnThruOff();      // no OUT->IN echo/feedback with a loopback cable
    MIDI.setHandleNoteOn(onNoteOn);
    MIDI.setHandleNoteOff(onNoteOff);
    MIDI.setHandleControlChange(onControlChange);

    Serial.println("running -- cmds: n=test note  c=chord  W=scale  v=next voice");
    Serial.println("                 +/-=vol  d=dump codec  i=re-init codec");
    Serial.println("      ESP32/kit:  r=reset  g=flash mode  @BOOTAPP@=exit flash  U=Teensy prog");

    // LATE settled reset of the ESP32 once everything's configured.
    Serial.println("[setup] settle 2.5s, then late kit.bootApp()..."); Serial.flush();
    delay(2500);
    kit.bootApp();
}

void loop() {
    // Flash-mode passthrough owns the loop (also handles @BOOTAPP@); in run mode
    // this ticks the slow LED heartbeat and returns false.
    if (kit.service(Serial)) return;

    while (MIDI.read()) { /* handlers fire per message */ }

    if (Serial.available()) {
        int c = Serial.read();
        if (!kit.handleChar(Serial, c)) {     // g / r / U handled by the kit
            if (c == 'n') {                    // local test note — no MIDI needed
                Serial.println("[cmd] test note: middle C, 400ms");
                g_opm.noteOn(60, 100); delay(400); g_opm.noteOff(60);
            }
            else if (c == 'c') {               // 4-note chord (tests polyphony)
                Serial.println("[cmd] C major chord (C E G C), 600ms");
                g_opm.noteOn(60, 100); g_opm.noteOn(64, 100);
                g_opm.noteOn(67, 100); g_opm.noteOn(72, 100);
                delay(600);
                g_opm.noteOff(60); g_opm.noteOff(64); g_opm.noteOff(67); g_opm.noteOff(72);
            }
            else if (c == 'W') { playScale(); }
            else if (c == 'v') { applyVoice(g_voiceIdx + 1); }
            else if (c == '+') { g_dvol += 3.0f; if (g_dvol > 0) g_dvol = 0;
                                 applyVol(); Serial.printf("[cmd] vol %.0f dB\n", g_dvol); }
            else if (c == '-') { g_dvol -= 3.0f; if (g_dvol < -60) g_dvol = -60;
                                 applyVol(); Serial.printf("[cmd] vol %.0f dB\n", g_dvol); }
            else if (c == 'd') { Serial.printf("[reg] CH_EN(76)=%02X PWR(78)=%02X\n",
                                 g_codec.readRegister(0, 0x76), g_codec.readRegister(0, 0x78)); }
            else if (c == 'i') { Serial.println("[cmd] re-init codec"); setupCodec(); applyVol();
                                 Serial.printf("[cmd] codec=%s (%s), vol %.0f dB\n",
                                               g_codecOk ? "OK" : "FAIL", g_codecMsg, g_dvol); }
        }
    }

    if (hb >= 1000) {
        hb = 0;
        float po = peakOut.available() ? peakOut.read() : 0.0f;
        Serial.printf("alive up=%lus  codec=%s(%s)  voice=%s  notes=%lu  outPeak=%.3f  "
                      "cpuMax=%.1f%% memMax=%u\n",
                      (unsigned long)(millis() / 1000),
                      g_codecOk ? "OK" : "FAIL", g_codecMsg, kVoices[g_voiceIdx]->name,
                      (unsigned long)g_noteOnCount, po,
                      AudioProcessorUsageMax(), AudioMemoryUsageMax());
        AudioProcessorUsageMaxReset();
    }
}
