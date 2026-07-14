// spike_midi_plaits — MIDI-IN driving the AUTHENTIC Mutable Instruments Plaits
// macro-oscillator (Emilie Gillet, MIT), a polyphonic MPE-aware voice pool:
//
//   MIDI IN (pin 0) / serial demo ─▶ Plaits2Sink ─▶ voice[0] AudioSynthPlaits ─┐
//                                     alloc + MPE     voice[1] ...              ├─▶ mix
//                                                     voice[2] ...              │  (AudioMixer4)
//                                                     voice[3] ...              ┘      │
//                                              AudioOutputTDM (SAI1) ─▶ TAC5212 ─▶ HP out
//
// Plaits is a "macro oscillator": one of 16 synthesis models (VA, waveshaping,
// FM, granular, additive, wavetable, chord, speech, swarm, noise, particle,
// string, modal, and 3 drum models) sculpted by HARMONICS / TIMBRE / MORPH +
// an LPG (DECAY + colour). Each of our N voices is one real Plaits Voice, so
// the pool is genuinely polyphonic and MPE-expressive (per-note bend/pressure/
// timbre). Unlike ymfm, Plaits has no idle gate — every voice renders every
// block — so watch cpuMax as kNumVoices scales.
//
// The codec / TDM / ESP32-kit setup is copied verbatim from spike_midi_ymfm_opm
// (known-good on this board); only the synth section is new.
//
// Bring-up path:
//   1. 'p' plays a C-major chord — proves the pool sounds.
//   2. 'A' sweeps ALL 16 engines, a note on each — proves every model renders.
//   3. 'e'/'E' pick an engine; 'h/t/y/k/l' tweak the macros; play the DIN.
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
#include <TDspPlaits2.h>     // AudioSynthPlaits + Plaits2Sink (authentic Plaits)

constexpr int     TAC5212_EN_PIN      = 35;     // shared SHDNZ, active-low
constexpr uint8_t TAC5212_I2C_ADDRESS = 0x51;

// Physical MIDI IN on the schematic's MIDI_RX (Teensy pin 0 = Serial1 RX)
// through the H11L1 optoisolator. The library drives Serial1 at 31250 baud.
MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI);

// --- Audio graph ------------------------------------------------------------
// tdmClk (SAI1 TDM input) is constructed FIRST so it owns update_responsibility
// exactly as in spike_midi_ymfm_opm; tdmOut is the actual DAC feed. tdmClk is
// left unconnected — it only keeps the SAI1 clock/update wiring identical.
AudioInputTDM          tdmClk;               // SAI1 TDM clock + update driver
AudioOutputTDM         tdmOut;               // SAI1 TDM -> TAC5212 DAC (ch0=L, ch1=R)

// Plaits has no idle gate: each voice renders every block, so 4 voices ~= 4x a
// single Plaits' CPU whether or not notes are held. Start at 4 (fits AudioMixer4
// exactly) and dial from the measured cpuMax.
constexpr int kNumVoices = 4;
AudioSynthPlaits       g_voice[kNumVoices];   // one real Plaits Voice each (mono out)
AudioMixer4            mix;                    // sum the voices to mono
AudioAnalyzePeak       peakOut;

AudioConnection c_v0(g_voice[0], 0, mix, 0);
AudioConnection c_v1(g_voice[1], 0, mix, 1);
AudioConnection c_v2(g_voice[2], 0, mix, 2);
AudioConnection c_v3(g_voice[3], 0, mix, 3);
AudioConnection c_outL(mix, 0, tdmOut, 0);
AudioConnection c_outR(mix, 0, tdmOut, 1);
AudioConnection c_pk (mix, 0, peakOut, 0);

Plaits2Sink::VoicePorts g_ports[kNumVoices] = {
    { &g_voice[0] }, { &g_voice[1] }, { &g_voice[2] }, { &g_voice[3] },
};
Plaits2Sink g_sink(g_ports, kNumVoices);

// --- Macro / mode state (mirrors the sink so the UI can print + step) --------
static uint8_t g_engine  = 0;
static float   g_harm    = 0.5f;
static float   g_timbre  = 0.5f;
static float   g_morph   = 0.5f;
static float   g_decay   = 0.5f;
static float   g_lpg     = 0.5f;

static bool    g_mpeMode      = false;
static uint8_t g_mpeMaster    = 1;       // MPE master channel (notes there are global)
static float   g_memberRange  = 48.0f;   // member-channel pitch-bend range (semitones)
static float   g_normalRange  = 2.0f;    // plain-keyboard pitch-bend range (semitones)

static uint32_t g_noteOnCount = 0;

tac5212::TAC5212 g_codec(Wire);
TDspProgrammingKit kit;
elapsedMillis hb;

static void hardResetCodecPower() {
    pinMode(TAC5212_EN_PIN, OUTPUT);
    digitalWrite(TAC5212_EN_PIN, LOW);  delay(5);
    digitalWrite(TAC5212_EN_PIN, HIGH); delay(10);
}

// I2C bus recovery: bit-bang SCL to free a stuck slave before Wire.begin(), so
// setup() can never hang. Wire0: SDA=18, SCL=19. (Verbatim from the ymfm spike.)
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

// --- Macro plumbing ----------------------------------------------------------
static void pushEngine() { g_sink.setEngine(g_engine); }
static void printState() {
    Serial.printf("[plaits] engine %2d \"%s\"  harm=%.2f timbre=%.2f morph=%.2f decay=%.2f lpg=%.2f  mode=%s\n",
                  g_engine, AudioSynthPlaits::engineName(g_engine),
                  g_harm, g_timbre, g_morph, g_decay, g_lpg, g_mpeMode ? "MPE" : "poly");
}

// --- MIDI handlers (Serial1 DIN) -> Plaits2Sink ------------------------------
static void onNoteOff(byte ch, byte note, byte vel);   // fwd (onNoteOn calls it)

static void onNoteOn(byte ch, byte note, byte vel) {
    if (vel == 0) { onNoteOff(ch, note, 0); return; }      // running-status note-off
    g_sink.onNoteOn(ch, note, vel);
    g_noteOnCount++;
}
static void onNoteOff(byte ch, byte note, byte /*vel*/) {
    g_sink.onNoteOff(ch, note, 0);
}
static void onPitchBend(byte ch, int bend) {               // bend: -8192..+8191
    // The sink expects semitones already scaled (as MidiRouter does upstream in
    // the real app); do that scaling here for the raw DIN stream.
    float range = g_mpeMode ? ((ch == g_mpeMaster) ? g_normalRange : g_memberRange)
                            : g_normalRange;
    g_sink.onPitchBend(ch, ((float)bend / 8192.0f) * range);
}
static void onAfterTouch(byte ch, byte pressure) {         // channel pressure (MPE Z)
    g_sink.onPressure(ch, (float)pressure / 127.0f);
}
static void onProgramChange(byte ch, byte prog) {          // pick an engine live
    (void)ch;
    g_engine = prog % AudioSynthPlaits::kNumEngines;
    pushEngine();
    printState();
}
static void onControlChange(byte ch, byte cc, byte val) {
    if (cc == 74) g_sink.onTimbre(ch, (float)val / 127.0f);   // MPE timbre (CC74, Y)
    if (cc == 123 && val == 0) g_sink.onAllNotesOff(0);       // all-notes-off
}

// Blocking helpers for the serial demos (interruptible by any serial key).
static bool delayOrKey(uint16_t ms) {
    uint32_t t0 = millis();
    while (millis() - t0 < ms) { if (Serial.available()) { Serial.read(); return true; } }
    return false;
}

// A C-major chord across the voice pool (ch1 = omni-friendly).
static void playChord() {
    static const uint8_t chord[] = { 60, 64, 67, 72 };
    Serial.println("[cmd] C-major chord, 700ms");
    for (uint8_t n : chord) g_sink.onNoteOn(1, n, 100);
    if (!delayOrKey(700)) {}
    for (uint8_t n : chord) g_sink.onNoteOff(1, n, 0);
}

// The headline: play a note through every one of the 16 engines in turn, so you
// hear each model and confirm none crash / all render. Prints per-engine cpuMax.
static void sweepEngines() {
    Serial.println("[cmd] sweeping all 16 Plaits engines (press any key to stop)");
    for (int e = 0; e < AudioSynthPlaits::kNumEngines; ++e) {
        g_engine = e;
        pushEngine();
        AudioProcessorUsageMaxReset();
        g_sink.onNoteOn(1, 60, 110);
        bool stop = delayOrKey(650);
        g_sink.onNoteOff(1, 60, 0);
        Serial.printf("   engine %2d \"%-14s\"  cpuMax=%.1f%%  outPeak=%.3f\n",
                      e, AudioSynthPlaits::engineName(e),
                      AudioProcessorUsageMax(),
                      peakOut.available() ? peakOut.read() : 0.0f);
        if (stop) { g_sink.onAllNotesOff(0); return; }
        if (delayOrKey(120)) { g_sink.onAllNotesOff(0); return; }
    }
    printState();
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
    Serial.println("=== spike_midi_plaits (AUTHENTIC Plaits macro-oscillator -> TAC5212) ===");
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

    // Each Plaits voice allocates 1 block/update; give ample headroom.
    AudioMemory(60);
    // Per-voice make-up: 4 voices at full-scale could sum past clip, so keep each
    // low enough that a 4-voice pile-up stays under 1.0 while one note is usable.
    for (int i = 0; i < 4; i++) mix.gain(i, 0.4f);
    if (g_codecOk) applyVol();

    // Seed the sink macros.
    pushEngine();
    g_sink.setHarmonics(g_harm);
    g_sink.setTimbre(g_timbre);
    g_sink.setMorph(g_morph);
    g_sink.setDecay(g_decay);
    g_sink.setLpgColour(g_lpg);
    g_sink.setMasterChannel(0);   // omni: on-screen/DIN keys on ch1 play

    MIDI.begin(MIDI_CHANNEL_OMNI);
    MIDI.turnThruOff();
    MIDI.setHandleNoteOn(onNoteOn);
    MIDI.setHandleNoteOff(onNoteOff);
    MIDI.setHandleProgramChange(onProgramChange);
    MIDI.setHandleControlChange(onControlChange);
    MIDI.setHandlePitchBend(onPitchBend);
    MIDI.setHandleAfterTouchChannel(onAfterTouch);

    Serial.println("running -- cmds: p=chord  A=sweep all 16 engines  e/E=next/prev engine");
    Serial.println("                 h/H=harmonics  t/T=timbre  y/Y=morph  k/K=decay  l/L=lpg colour");
    Serial.println("                 M=toggle MPE  +/-=vol  d=dump codec  i=re-init codec");
    Serial.println("      ESP32/kit:  r=reset  g=flash mode  @BOOTAPP@=exit flash  U=Teensy prog");
    printState();

    Serial.println("[setup] settle 2.5s, then late kit.bootApp()..."); Serial.flush();
    delay(2500);
    kit.bootApp();
}

// Nudge a 0..1 macro by +/-step (clamped), push to the sink, print.
static void nudge(float &v, float step, void (Plaits2Sink::*setter)(float)) {
    v += step;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    (g_sink.*setter)(v);
    printState();
}

void loop() {
    if (kit.service(Serial)) return;

    while (MIDI.read()) { /* handlers fire per message */ }

    if (Serial.available()) {
        int c = Serial.read();
        if (!kit.handleChar(Serial, c)) {     // g / r / U handled by the kit
            switch (c) {
                case 'p': playChord(); break;
                case 'A': sweepEngines(); break;
                case 'e': g_engine = (g_engine + 1) % AudioSynthPlaits::kNumEngines; pushEngine(); printState(); break;
                case 'E': g_engine = (g_engine + AudioSynthPlaits::kNumEngines - 1) % AudioSynthPlaits::kNumEngines; pushEngine(); printState(); break;
                case 'h': nudge(g_harm,   -0.1f, &Plaits2Sink::setHarmonics); break;
                case 'H': nudge(g_harm,   +0.1f, &Plaits2Sink::setHarmonics); break;
                case 't': nudge(g_timbre, -0.1f, &Plaits2Sink::setTimbre);    break;
                case 'T': nudge(g_timbre, +0.1f, &Plaits2Sink::setTimbre);    break;
                case 'y': nudge(g_morph,  -0.1f, &Plaits2Sink::setMorph);     break;
                case 'Y': nudge(g_morph,  +0.1f, &Plaits2Sink::setMorph);     break;
                case 'k': nudge(g_decay,  -0.1f, &Plaits2Sink::setDecay);     break;
                case 'K': nudge(g_decay,  +0.1f, &Plaits2Sink::setDecay);     break;
                case 'l': nudge(g_lpg,    -0.1f, &Plaits2Sink::setLpgColour); break;
                case 'L': nudge(g_lpg,    +0.1f, &Plaits2Sink::setLpgColour); break;
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
        Serial.printf("alive up=%lus  codec=%s(%s)  mode=%s  engine=%d\"%s\"  notes=%lu  "
                      "outPeak=%.3f  cpuMax=%.1f%% memMax=%u\n",
                      (unsigned long)(millis() / 1000),
                      g_codecOk ? "OK" : "FAIL", g_codecMsg,
                      g_mpeMode ? "MPE" : "poly", g_engine, AudioSynthPlaits::engineName(g_engine),
                      (unsigned long)g_noteOnCount,
                      po, AudioProcessorUsageMax(), AudioMemoryUsageMax());
        AudioProcessorUsageMaxReset();
    }
}
