// spike_midi_rings — MIDI-IN driving a Mutable-Rings-style modal/string
// resonator (DaisySP ModalVoice + StringVoice, MIT), a polyphonic MPE voice pool:
//
//   MIDI IN (pin 0) / serial demo ─▶ RingsSink ─▶ voice[0] AudioSynthRings ─┐
//                                     alloc + MPE   voice[1] ...             ├─▶ mix
//                                                   voice[2] ...             │  (AudioMixer4)
//                                                   voice[3] ...             ┘      │
//                                            AudioOutputTDM (SAI1) ─▶ TAC5212 ─▶ HP out
//
// Rings is a physical-modelling resonator: an excitation rings a bank of tuned
// modes (Modal mode) or a plucked/bowed string (String mode). Each note gets its
// own resonator, so the pool is polyphonic + MPE (per-note pitch bend, pressure
// -> ring length, CC#74 -> brightness). DaisySP processes one sample per call.
//
// Codec / TDM / ESP32-kit setup copied verbatim from spike_midi_plaits.
//
// Bring-up path:
//   1. 'p' plays a C-major chord — proves the pool rings.
//   2. 'o' toggles Modal <-> String mode.
//   3. 's/S' structure, 'b/B' brightness, 'n/N' damping; play the DIN.
//   4. 'M' toggles MPE (member channels 2..16 get per-note expression).
//
// ESP32/kit:  r=reset->app  g=flash mode  @BOOTAPP@=exit flash  U=Teensy prog.

#include <Arduino.h>
#include <Wire.h>
#include "tdsp_hw_config.h"
#include <Audio.h>
#include <TAC5212.h>
#include <TDspProgrammingKit.h>
#include <MIDI.h>
#include <TDspRings.h>       // AudioSynthRings + RingsSink (DaisySP modal/string)

constexpr int     TAC5212_EN_PIN      = 35;     // shared SHDNZ, active-low
constexpr uint8_t TAC5212_I2C_ADDRESS = 0x51;

MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI);

// --- Audio graph ------------------------------------------------------------
AudioInputTDM          tdmClk;               // SAI1 TDM clock + update driver
AudioOutputTDM         tdmOut;               // SAI1 TDM -> TAC5212 DAC (ch0=L, ch1=R)

// DaisySP's modal resonator costs ~32% CPU per voice and runs every block
// (no idle gate), so 4 voices overruns the audio ISR (>100%) and hangs the whole
// board. 3 voices (~95%) is the standalone ceiling; the mix-kit (BT + S/PDIF on
// top) will use fewer. TODO: gate Process on "still ringing" to make idle voices
// free and reclaim polyphony.
constexpr int kNumVoices = 3;
AudioSynthRings        g_voice[kNumVoices];   // one resonator each (mono out)
AudioMixer4            mix;
AudioAnalyzePeak       peakOut;

AudioConnection c_v0(g_voice[0], 0, mix, 0);
AudioConnection c_v1(g_voice[1], 0, mix, 1);
AudioConnection c_v2(g_voice[2], 0, mix, 2);
AudioConnection c_outL(mix, 0, tdmOut, 0);
AudioConnection c_outR(mix, 0, tdmOut, 1);
AudioConnection c_pk (mix, 0, peakOut, 0);

RingsSink::VoicePorts g_ports[kNumVoices] = {
    { &g_voice[0] }, { &g_voice[1] }, { &g_voice[2] },
};
RingsSink g_sink(g_ports, kNumVoices);

// --- Macro / mode state ------------------------------------------------------
static uint8_t g_mode       = 0;    // 0 Modal, 1 String
static float   g_structure  = 0.5f;
static float   g_brightness = 0.5f;
static float   g_damping    = 0.7f; // longer default ring

static bool    g_mpeMode      = false;
static uint8_t g_mpeMaster    = 1;
static float   g_memberRange  = 48.0f;
static float   g_normalRange  = 2.0f;

static uint32_t g_noteOnCount = 0;

tac5212::TAC5212 g_codec(Wire);
TDspProgrammingKit kit;
elapsedMillis hb;

static void hardResetCodecPower() {
    pinMode(TAC5212_EN_PIN, OUTPUT);
    digitalWrite(TAC5212_EN_PIN, LOW);  delay(5);
    digitalWrite(TAC5212_EN_PIN, HIGH); delay(10);
}

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
    g_codec.writeRegister(0, /*INTF_CFG1*/ 0x10, 0x00);
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

static void pushGlobals() {
    g_sink.setMode(g_mode);
    g_sink.setStructure(g_structure);
    g_sink.setBrightness(g_brightness);
    g_sink.setDamping(g_damping);
}
static void printState() {
    Serial.printf("[rings] mode=%s  structure=%.2f brightness=%.2f damping=%.2f  midi=%s\n",
                  AudioSynthRings::modeName(g_mode),
                  g_structure, g_brightness, g_damping, g_mpeMode ? "MPE" : "poly");
}

// --- MIDI handlers -----------------------------------------------------------
static void onNoteOff(byte ch, byte note, byte vel);

static void onNoteOn(byte ch, byte note, byte vel) {
    if (vel == 0) { onNoteOff(ch, note, 0); return; }
    g_sink.onNoteOn(ch, note, vel);
    g_noteOnCount++;
}
static void onNoteOff(byte ch, byte note, byte /*vel*/) {
    g_sink.onNoteOff(ch, note, 0);
}
static void onPitchBend(byte ch, int bend) {
    float range = g_mpeMode ? ((ch == g_mpeMaster) ? g_normalRange : g_memberRange)
                            : g_normalRange;
    g_sink.onPitchBend(ch, ((float)bend / 8192.0f) * range);
}
static void onAfterTouch(byte ch, byte pressure) {
    g_sink.onPressure(ch, (float)pressure / 127.0f);
}
static void onProgramChange(byte /*ch*/, byte prog) {
    g_mode = prog % AudioSynthRings::kNumModes;
    pushGlobals();
    printState();
}
static void onControlChange(byte ch, byte cc, byte val) {
    if (cc == 74) g_sink.onTimbre(ch, (float)val / 127.0f);
    if (cc == 123 && val == 0) g_sink.onAllNotesOff(0);
}

static bool delayOrKey(uint16_t ms) {
    uint32_t t0 = millis();
    while (millis() - t0 < ms) { if (Serial.available()) { Serial.read(); return true; } }
    return false;
}

static void playChord() {
    static const uint8_t chord[] = { 60, 64, 67, 72 };
    Serial.println("[cmd] C-major chord, 1200ms (let it ring)");
    for (uint8_t n : chord) g_sink.onNoteOn(1, n, 100);
    delayOrKey(1200);
    for (uint8_t n : chord) g_sink.onNoteOff(1, n, 0);
}

static void setMpeMode(bool on) {
    g_sink.onAllNotesOff(0);
    g_mpeMode = on;
    g_sink.setMasterChannel(on ? g_mpeMaster : 0);
    if (on) Serial.printf("[mpe] ON  master=ch%d  member-bend=+/-%.0f semi — play ch2..16\n",
                          g_mpeMaster, g_memberRange);
    else    Serial.println("[mpe] OFF -> plain poly (omni; on-screen keys on ch1 play)");
}

void setup() {
    hardResetCodecPower();

    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 1500) {}

    Serial.println();
    if (CrashReport) { Serial.println("!!! CRASH REPORT (previous run) !!!"); Serial.print(CrashReport); }
    Serial.println("=== spike_midi_rings (DaisySP modal/string resonator -> TAC5212) ===");
    Serial.println("Physical MIDI IN on pin 0 (Serial1 RX, 31250) via the H11L1 opto.");

    kit.onFlashEnter([] { AudioNoInterrupts(); });

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

    AudioMemory(60);
    // Init the DaisySP voices now (NOT in their constructors) — after AudioMemory,
    // with the audio ISR quiesced, so DaisySP Init can't race the update ISR.
    AudioNoInterrupts();
    for (int i = 0; i < kNumVoices; ++i) g_voice[i].begin();
    AudioInterrupts();
    for (int i = 0; i < 4; i++) mix.gain(i, 0.5f);
    if (g_codecOk) applyVol();

    pushGlobals();
    g_sink.setMasterChannel(0);

    MIDI.begin(MIDI_CHANNEL_OMNI);
    MIDI.turnThruOff();
    MIDI.setHandleNoteOn(onNoteOn);
    MIDI.setHandleNoteOff(onNoteOff);
    MIDI.setHandleProgramChange(onProgramChange);
    MIDI.setHandleControlChange(onControlChange);
    MIDI.setHandlePitchBend(onPitchBend);
    MIDI.setHandleAfterTouchChannel(onAfterTouch);

    Serial.println("running -- cmds: p=chord  o=Modal/String mode  s/S=structure  b/B=brightness");
    Serial.println("                 n/N=damping  M=toggle MPE  +/-=vol  d=dump codec  i=re-init codec");
    Serial.println("      ESP32/kit:  r=reset  g=flash mode  @BOOTAPP@=exit flash  U=Teensy prog");
    printState();

    Serial.println("[setup] settle 2.5s, then late kit.bootApp()..."); Serial.flush();
    delay(2500);
    kit.bootApp();
}

static void nudge(float &v, float step, void (RingsSink::*setter)(float)) {
    v += step;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    (g_sink.*setter)(v);
    printState();
}

void loop() {
    if (kit.service(Serial)) return;

    while (MIDI.read()) {}

    if (Serial.available()) {
        int c = Serial.read();
        if (!kit.handleChar(Serial, c)) {
            switch (c) {
                case 'p': playChord(); break;
                case 'o': g_mode = (g_mode + 1) % AudioSynthRings::kNumModes;
                          g_sink.setMode(g_mode); printState(); break;
                case 's': nudge(g_structure,  -0.1f, &RingsSink::setStructure);  break;
                case 'S': nudge(g_structure,  +0.1f, &RingsSink::setStructure);  break;
                case 'b': nudge(g_brightness, -0.1f, &RingsSink::setBrightness); break;
                case 'B': nudge(g_brightness, +0.1f, &RingsSink::setBrightness); break;
                case 'n': nudge(g_damping,    -0.1f, &RingsSink::setDamping);    break;
                case 'N': nudge(g_damping,    +0.1f, &RingsSink::setDamping);    break;
                case 'M': setMpeMode(!g_mpeMode); break;
                case '+': g_dvol += 3.0f; if (g_dvol > 0) g_dvol = 0;
                          applyVol(); Serial.printf("[cmd] vol %.0f dB\n", g_dvol); break;
                case '-': g_dvol -= 3.0f; if (g_dvol < -60) g_dvol = -60;
                          applyVol(); Serial.printf("[cmd] vol %.0f dB\n", g_dvol); break;
                case 'd': Serial.printf("[reg] CH_EN(76)=%02X PWR(78)=%02X\n",
                          g_codec.readRegister(0, 0x76), g_codec.readRegister(0, 0x78)); break;
                case 'i': Serial.println("[cmd] re-init codec"); setupCodec(); applyVol();
                          Serial.printf("[cmd] codec=%s (%s), vol %.0f dB\n",
                                        g_codecOk ? "OK" : "FAIL", g_codecMsg, g_dvol); break;
                default: break;
            }
        }
    }

    if (hb >= 1000) {
        hb = 0;
        float po = peakOut.available() ? peakOut.read() : 0.0f;
        Serial.printf("alive up=%lus  codec=%s(%s)  mode=%s  midi=%s  notes=%lu  "
                      "outPeak=%.3f  cpuMax=%.1f%% memMax=%u\n",
                      (unsigned long)(millis() / 1000),
                      g_codecOk ? "OK" : "FAIL", g_codecMsg,
                      AudioSynthRings::modeName(g_mode), g_mpeMode ? "MPE" : "poly",
                      (unsigned long)g_noteOnCount,
                      po, AudioProcessorUsageMax(), AudioMemoryUsageMax());
        AudioProcessorUsageMaxReset();
    }
}
