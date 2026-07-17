// spike_midi_va — MIDI-IN driving a DaisySP virtual-analog synth (Electrosmith,
// MIT): 2 PolyBLEP oscillators -> Moog ladder filter -> ADSR, per voice, as an
// MPE-aware polyphonic pool.
//
//   MIDI IN (pin 0) / serial ─▶ DaisyVaSink ─▶ 8 x AudioSynthDaisyVa ─▶ mixer
//                                alloc + MPE                           tree ─▶ TDM ─▶ TAC5212
//
// VA voices are cheap (osc+filter+ADSR ~ a few % each), so this runs 8 voices.
// Per-note MPE: bend -> pitch, pressure -> level, CC#74 -> filter cutoff. A small
// preset bank (Saw Lead / Detuned Saws / Square Reed / Soft Tri / Acid Bass) is
// the "instrument" picker. Codec/TDM/ESP32-kit plumbing copied from spike_midi_rings.
//
// Bring-up:  'p' chord   '[' / ']' prev/next preset   'M' toggle MPE
// ESP32/kit: r=reset->app  g=flash mode  @BOOTAPP@=exit flash  U=Teensy prog.

#include <Arduino.h>
#include <Wire.h>
#include "tdsp_hw_config.h"
#include <Audio.h>
#include <TAC5212.h>
#include <TDspProgrammingKit.h>
#include <MIDI.h>
#include <TDspDaisyVa.h>    // AudioSynthDaisyVa + DaisyVaSink

constexpr int     TAC5212_EN_PIN      = 35;
constexpr uint8_t TAC5212_I2C_ADDRESS = 0x51;

MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI);

// --- Audio graph ------------------------------------------------------------
AudioInputTDM          tdmClk;
AudioOutputTDM         tdmOut;

constexpr int kNumVoices = 8;
AudioSynthDaisyVa      g_voice[kNumVoices];
AudioMixer4            mixA, mixB, mixOut;   // 8 voices -> two 4-in mixers -> sum
AudioAnalyzePeak       peakOut;

AudioConnection c_v0(g_voice[0], 0, mixA, 0);
AudioConnection c_v1(g_voice[1], 0, mixA, 1);
AudioConnection c_v2(g_voice[2], 0, mixA, 2);
AudioConnection c_v3(g_voice[3], 0, mixA, 3);
AudioConnection c_v4(g_voice[4], 0, mixB, 0);
AudioConnection c_v5(g_voice[5], 0, mixB, 1);
AudioConnection c_v6(g_voice[6], 0, mixB, 2);
AudioConnection c_v7(g_voice[7], 0, mixB, 3);
AudioConnection c_mA(mixA, 0, mixOut, 0);
AudioConnection c_mB(mixB, 0, mixOut, 1);
AudioConnection c_outL(mixOut, 0, tdmOut, 0);
AudioConnection c_outR(mixOut, 0, tdmOut, 1);
AudioConnection c_pk (mixOut, 0, peakOut, 0);

DaisyVaSink::VoicePorts g_ports[kNumVoices] = {
    {&g_voice[0]}, {&g_voice[1]}, {&g_voice[2]}, {&g_voice[3]},
    {&g_voice[4]}, {&g_voice[5]}, {&g_voice[6]}, {&g_voice[7]},
};
DaisyVaSink g_sink(g_ports, kNumVoices);

// --- Mode state --------------------------------------------------------------
static int     g_preset  = 0;
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
static void applyVol() { g_codec.out(1).setDvol(g_dvol); g_codec.out(2).setDvol(g_dvol); }

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
    g_codec.writeRegister(0, 0x10, 0x00);
    g_codec.setRxSlotOffset(1);
    g_codec.setRxChannelSlot(1, 0);
    g_codec.setRxChannelSlot(2, 1);
    g_codec.out(1).setMode(tac5212::OutMode::HpDriver);
    g_codec.out(2).setMode(tac5212::OutMode::HpDriver);
    g_codec.out(1).setDvol(-128.0f);
    g_codec.out(2).setDvol(-128.0f);
    g_codec.setChannelEnable(0x0, 0xC);
    g_codec.powerDac(true);
    delay(100);
    g_codec.setDspAvddSelect(true);
}

static void printState() {
    Serial.printf("[va] preset %d \"%s\"  midi=%s\n",
                  g_preset, DaisyVaSink::presetName(g_preset), g_mpeMode ? "MPE" : "poly");
}

// --- MIDI handlers -----------------------------------------------------------
static void onNoteOff(byte ch, byte note, byte vel);
static void onNoteOn(byte ch, byte note, byte vel) {
    if (vel == 0) { onNoteOff(ch, note, 0); return; }
    g_sink.onNoteOn(ch, note, vel);
    g_noteOnCount++;
}
static void onNoteOff(byte ch, byte note, byte /*vel*/) { g_sink.onNoteOff(ch, note, 0); }
static void onPitchBend(byte ch, int bend) {
    float range = g_mpeMode ? ((ch == g_mpeMaster) ? g_normalRange : g_memberRange) : g_normalRange;
    g_sink.onPitchBend(ch, ((float)bend / 8192.0f) * range);
}
static void onAfterTouch(byte ch, byte pressure) { g_sink.onPressure(ch, (float)pressure / 127.0f); }
static void onProgramChange(byte /*ch*/, byte prog) {
    g_preset = prog % DaisyVaSink::numPresets();
    g_sink.setPreset(g_preset);
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
    static const uint8_t chord[] = { 48, 55, 60, 64, 67, 72 };
    Serial.println("[cmd] 6-note chord, 900ms");
    for (uint8_t n : chord) g_sink.onNoteOn(1, n, 100);
    delayOrKey(900);
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
    if (CrashReport) { Serial.println("!!! CRASH REPORT !!!"); Serial.print(CrashReport); }
    Serial.println("=== spike_midi_va (DaisySP virtual-analog -> TAC5212) ===");
    Serial.println("Physical MIDI IN on pin 0 (Serial1 RX, 31250) via the H11L1 opto.");

    kit.onFlashEnter([] { AudioNoInterrupts(); });
    Serial.println("[setup] kit.begin()..."); Serial.flush();
    kit.begin();
    delay(300);

    Serial.println("[setup] i2c bus recover..."); Serial.flush();
    i2cBusRecover();
    pinMode(18, INPUT_PULLUP); delayMicroseconds(20);
    if (digitalRead(18) == LOW) {
        g_codecOk = false; g_codecMsg = "i2c wedged - skipped";
        Serial.println("[setup] !! I2C SDA STILL LOW -> SKIP codec init.");
    } else {
        Serial.println("[setup] Wire.begin..."); Serial.flush();
        Wire.begin();
        Wire.setClock(100000);
        tdspMuxAutoSelectCodec(TAC5212_I2C_ADDRESS);
        setupCodec();
        Serial.println("[setup] codec init done"); Serial.flush();
    }

    AudioMemory(80);
    // Init the DaisySP voices now (deferred out of ctors), ISR quiesced.
    AudioNoInterrupts();
    for (int i = 0; i < kNumVoices; ++i) g_voice[i].begin();
    AudioInterrupts();
    mixA.gain(0, 0.4f); mixA.gain(1, 0.4f); mixA.gain(2, 0.4f); mixA.gain(3, 0.4f);
    mixB.gain(0, 0.4f); mixB.gain(1, 0.4f); mixB.gain(2, 0.4f); mixB.gain(3, 0.4f);
    mixOut.gain(0, 1.0f); mixOut.gain(1, 1.0f);
    if (g_codecOk) applyVol();

    g_sink.setPreset(g_preset);
    g_sink.setMasterChannel(0);

    MIDI.begin(MIDI_CHANNEL_OMNI);
    MIDI.turnThruOff();
    MIDI.setHandleNoteOn(onNoteOn);
    MIDI.setHandleNoteOff(onNoteOff);
    MIDI.setHandleProgramChange(onProgramChange);
    MIDI.setHandleControlChange(onControlChange);
    MIDI.setHandlePitchBend(onPitchBend);
    MIDI.setHandleAfterTouchChannel(onAfterTouch);

    Serial.println("running -- cmds: p=chord  [ / ]=prev/next preset  M=toggle MPE");
    Serial.println("                 +/-=vol  d=dump codec  i=re-init codec");
    Serial.println("      ESP32/kit:  r=reset  g=flash mode  @BOOTAPP@=exit flash  U=Teensy prog");
    printState();

    Serial.println("[setup] settle 2.5s, then late kit.bootApp()..."); Serial.flush();
    delay(2500);
    kit.bootApp();
}

void loop() {
    if (kit.service(Serial)) return;
    while (MIDI.read()) {}

    if (Serial.available()) {
        int c = Serial.read();
        if (!kit.handleChar(Serial, c)) {
            switch (c) {
                case 'p': playChord(); break;
                case ']': g_preset = (g_preset + 1) % DaisyVaSink::numPresets();
                          g_sink.setPreset(g_preset); printState(); break;
                case '[': g_preset = (g_preset + DaisyVaSink::numPresets() - 1) % DaisyVaSink::numPresets();
                          g_sink.setPreset(g_preset); printState(); break;
                case 'M': setMpeMode(!g_mpeMode); break;
                case '+': g_dvol += 3.0f; if (g_dvol > 0) g_dvol = 0;
                          applyVol(); Serial.printf("[cmd] vol %.0f dB\n", g_dvol); break;
                case '-': g_dvol -= 3.0f; if (g_dvol < -60) g_dvol = -60;
                          applyVol(); Serial.printf("[cmd] vol %.0f dB\n", g_dvol); break;
                case 'd': Serial.printf("[reg] CH_EN(76)=%02X PWR(78)=%02X\n",
                          g_codec.readRegister(0, 0x76), g_codec.readRegister(0, 0x78)); break;
                case 'i': Serial.println("[cmd] re-init codec"); setupCodec(); applyVol(); break;
                default: break;
            }
        }
    }

    if (hb >= 1000) {
        hb = 0;
        float po = peakOut.available() ? peakOut.read() : 0.0f;
        Serial.printf("alive up=%lus  codec=%s(%s)  preset=%d\"%s\"  midi=%s  notes=%lu  "
                      "outPeak=%.3f  cpuMax=%.1f%% memMax=%u\n",
                      (unsigned long)(millis() / 1000),
                      g_codecOk ? "OK" : "FAIL", g_codecMsg,
                      g_preset, DaisyVaSink::presetName(g_preset), g_mpeMode ? "MPE" : "poly",
                      (unsigned long)g_noteOnCount,
                      po, AudioProcessorUsageMax(), AudioMemoryUsageMax());
        AudioProcessorUsageMaxReset();
    }
}
