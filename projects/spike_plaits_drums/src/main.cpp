// spike_plaits_drums — a NO-PSRAM synthesized drum kit from the AUTHENTIC
// Mutable Instruments Plaits drum models (Emilie Gillet, MIT). A candidate
// peer/replacement for the 5-sound OPLL rhythm voice (DrumVoice.h): Plaits'
// three analog drum models cover a full GM ch10 kit when mapped + retuned.
//
//   MIDI IN (pin 0, ch10) / serial demo ─▶ drumPatch(note) ─▶ voice pool ─┐
//     each voice = AudioSynthPlaits set live to engine 13/14/15           │
//       13 Analog Bass Drum · 14 Analog Snare · 15 Hi-Hat                 ├─▶ mixA/mixB
//                                                                          ┘   ─▶ TDM ─▶ TAC5212
//
// GM drum map (drumPatch): kick/toms/low-perc -> Bass Drum (retuned), snare/
// clap/rim -> Snare, hats/cymbals -> Hi-Hat (short vs long DECAY). A small
// round-robin voice POOL keeps overlapping hits from cutting each other; each
// hit reconfigures its voice's model+pitch+decay then pulses the trigger.
//
// The codec / TDM / ESP32-kit setup is copied verbatim from spike_midi_plaits
// (known-good on this board); only the note->drum mapping section is new.
//
// ESP32/kit:  r=reset->app  g=flash mode  @BOOTAPP@=exit flash  U=Teensy prog.

#include <Arduino.h>
#include <Wire.h>
#include "tdsp_hw_config.h"
#include <Audio.h>
#include <TAC5212.h>
#include <TDspProgrammingKit.h>
#include <MIDI.h>
#include <TDspPlaits2.h>     // AudioSynthPlaits (authentic Plaits; engines 13/14/15 = drums)

constexpr int     TAC5212_EN_PIN      = 35;     // shared SHDNZ, active-low
constexpr uint8_t TAC5212_I2C_ADDRESS = 0x51;

MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI);

// --- Audio graph ------------------------------------------------------------
// tdmClk constructed FIRST so it owns update_responsibility (as in the ymfm/
// plaits spikes). 6 drum voices need two mixers: mixA sums v0..3, mixB sums
// mixA + v4 + v5, then feeds the DAC (mono, duplicated L/R).
AudioInputTDM          tdmClk;
AudioOutputTDM         tdmOut;

constexpr int kNumVoices = 6;                    // drum polyphony (overlapping hits)
AudioSynthPlaits       g_voice[kNumVoices];
AudioMixer4            mixA;                      // v0..v3
AudioMixer4            mixB;                      // mixA + v4 + v5
AudioAnalyzePeak       peakOut;

AudioConnection c_a0(g_voice[0], 0, mixA, 0);
AudioConnection c_a1(g_voice[1], 0, mixA, 1);
AudioConnection c_a2(g_voice[2], 0, mixA, 2);
AudioConnection c_a3(g_voice[3], 0, mixA, 3);
AudioConnection c_bA(mixA,       0, mixB, 0);
AudioConnection c_b4(g_voice[4], 0, mixB, 1);
AudioConnection c_b5(g_voice[5], 0, mixB, 2);
AudioConnection c_outL(mixB, 0, tdmOut, 0);
AudioConnection c_outR(mixB, 0, tdmOut, 1);
AudioConnection c_pk (mixB, 0, peakOut, 0);

// --- GM drum map ------------------------------------------------------------
// Each GM ch10 note -> a Plaits drum model + pitch + macro shape + LPG decay.
// engine: 13 Bass Drum, 14 Snare, 15 Hi-Hat. note is the Plaits base pitch
// (fractional MIDI). decay is the LPG tail length (0..1) — the main hat/cymbal
// differentiator. harm/timbre/morph shape each model (mid = neutral).
struct DrumPatch {
    int8_t  engine;
    float   note;
    float   harm, timbre, morph, decay;
};

// Map a GM drum note to a patch. Falls back to a mid-tom for anything unlisted
// so a dense groove never goes silent. Retunes the two pitched models (BD/SD)
// to fake toms/percussion Plaits has no dedicated model for.
static DrumPatch drumPatch(uint8_t n) {
    switch (n) {
        // --- Bass drum / kick ---
        case 35: return { 13, 33.0f, 0.5f, 0.4f, 0.5f, 0.55f };   // Acoustic Bass Drum
        case 36: return { 13, 36.0f, 0.6f, 0.5f, 0.5f, 0.50f };   // Bass Drum 1
        // --- Snare / clap / rim ---
        case 37: return { 14, 64.0f, 0.4f, 0.7f, 0.3f, 0.18f };   // Side Stick
        case 38: return { 14, 57.0f, 0.5f, 0.5f, 0.5f, 0.40f };   // Acoustic Snare
        case 39: return { 14, 60.0f, 0.4f, 0.85f,0.6f, 0.35f };   // Hand Clap (noisy)
        case 40: return { 14, 60.0f, 0.6f, 0.5f, 0.5f, 0.38f };   // Electric Snare
        // --- Toms (Bass Drum model, retuned up the kit) ---
        case 41: return { 13, 43.0f, 0.4f, 0.4f, 0.4f, 0.60f };   // Low Floor Tom
        case 43: return { 13, 47.0f, 0.4f, 0.4f, 0.4f, 0.58f };   // High Floor Tom
        case 45: return { 13, 50.0f, 0.4f, 0.4f, 0.4f, 0.55f };   // Low Tom
        case 47: return { 13, 53.0f, 0.4f, 0.4f, 0.4f, 0.52f };   // Low-Mid Tom
        case 48: return { 13, 57.0f, 0.4f, 0.4f, 0.4f, 0.50f };   // Hi-Mid Tom
        case 50: return { 13, 60.0f, 0.4f, 0.4f, 0.4f, 0.48f };   // High Tom
        // --- Hi-hats (Hi-Hat model, short vs long decay) ---
        case 42: return { 15, 72.0f, 0.5f, 0.5f, 0.5f, 0.14f };   // Closed Hi-Hat
        case 44: return { 15, 72.0f, 0.5f, 0.5f, 0.5f, 0.12f };   // Pedal Hi-Hat
        case 46: return { 15, 72.0f, 0.5f, 0.6f, 0.5f, 0.55f };   // Open Hi-Hat
        // --- Cymbals (Hi-Hat model, bright + long) ---
        case 49: return { 15, 76.0f, 0.6f, 0.85f,0.6f, 0.90f };   // Crash 1
        case 57: return { 15, 78.0f, 0.6f, 0.85f,0.6f, 0.88f };   // Crash 2
        case 51: return { 15, 80.0f, 0.6f, 0.80f,0.6f, 0.80f };   // Ride
        case 59: return { 15, 80.0f, 0.6f, 0.80f,0.6f, 0.78f };   // Ride 2
        case 53: return { 15, 84.0f, 0.6f, 0.75f,0.6f, 0.70f };   // Ride Bell
        case 52: return { 15, 74.0f, 0.6f, 0.90f,0.6f, 0.85f };   // China
        case 55: return { 15, 82.0f, 0.6f, 0.85f,0.6f, 0.60f };   // Splash
        // --- Hand percussion ---
        case 54: return { 15, 88.0f, 0.5f, 0.6f, 0.5f, 0.20f };   // Tambourine
        case 56: return { 13, 72.0f, 0.7f, 0.3f, 0.3f, 0.18f };   // Cowbell (pitched)
        case 60: return { 13, 62.0f, 0.4f, 0.4f, 0.4f, 0.30f };   // Hi Bongo
        case 61: return { 13, 58.0f, 0.4f, 0.4f, 0.4f, 0.32f };   // Low Bongo
        case 62: return { 13, 60.0f, 0.4f, 0.5f, 0.4f, 0.28f };   // Mute Hi Conga
        case 63: return { 13, 56.0f, 0.4f, 0.5f, 0.4f, 0.34f };   // Open Hi Conga
        case 75: return { 15, 90.0f, 0.6f, 0.4f, 0.5f, 0.12f };   // Claves
        case 76: return { 13, 74.0f, 0.7f, 0.3f, 0.3f, 0.14f };   // Hi Wood Block
        case 77: return { 13, 70.0f, 0.7f, 0.3f, 0.3f, 0.16f };   // Low Wood Block
        default: return { 13, 52.0f, 0.4f, 0.4f, 0.4f, 0.45f };   // fallback: mid tom
    }
}

// --- Voice pool -------------------------------------------------------------
// Round-robin allocation. Each entry remembers which GM note it's currently
// sounding (for note-off) and when to drop its trigger (drums are one-shots:
// pulse gate high, then low a few ms later; the LPG decay makes the tail).
struct VoiceSlot { uint8_t note; uint32_t releaseAt; bool active; };
static VoiceSlot g_slot[kNumVoices] = {};
static int       g_rr = 0;
static uint32_t  g_hits = 0;

static void hitDrum(uint8_t note, uint8_t vel) {
    const DrumPatch p = drumPatch(note);
    const int v = g_rr;
    g_rr = (g_rr + 1) % kNumVoices;

    AudioSynthPlaits &vc = g_voice[v];
    vc.setEngineIndex(p.engine);
    vc.setNote(p.note);
    vc.setHarmonics(p.harm);
    vc.setTimbre(p.timbre);
    vc.setMorph(p.morph);
    vc.setDecay(p.decay);
    vc.setLpgColour(0.5f);
    vc.setLevel(vel / 127.0f);
    vc.gate(true);                                   // rising edge fires the drum

    g_slot[v] = { note, millis() + 6, true };        // drop the trigger ~6 ms later
    g_hits++;
}

// Service the one-shot triggers: once a slot's hold elapses, release its gate
// so the model enters its decay tail (or is free to be re-triggered).
static void serviceVoices() {
    const uint32_t now = millis();
    for (int v = 0; v < kNumVoices; ++v) {
        if (g_slot[v].active && (int32_t)(now - g_slot[v].releaseAt) >= 0) {
            g_voice[v].gate(false);
            g_slot[v].active = false;
        }
    }
}

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
static float g_dvol = -18.0f;
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

// --- MIDI (Serial1 DIN) -> drum kit -----------------------------------------
// Drums are one-shots, so note-off is ignored (the LPG decay ends the sound).
// Accept omni for easy testing; a real integration would gate on channel 10.
static void onNoteOn(byte ch, byte note, byte vel) {
    (void)ch;
    if (vel == 0) return;
    hitDrum(note, vel);
}

static bool delayOrKey(uint16_t ms) {
    uint32_t t0 = millis();
    while (millis() - t0 < ms) { serviceVoices(); if (Serial.available()) { Serial.read(); return true; } }
    return false;
}

// A one-bar rock groove at ~110 BPM (16th = ~136 ms): kick 1&3, snare 2&4,
// closed hat on every 8th, an open hat on the "and" of 4. Loops until a key.
static void playBeat() {
    Serial.println("[cmd] demo beat (kick/snare/hat) — press any key to stop");
    const uint16_t step = 135;   // ~110 BPM sixteenths
    // 16 steps; each entry lists the GM notes that fire on that step.
    static const uint8_t K = 36, S = 38, H = 42, O = 46;
    for (int bar = 0; bar < 4; ++bar) {
        for (int i = 0; i < 16; ++i) {
            if (i == 0 || i == 8)  hitDrum(K, 118);
            if (i == 4 || i == 12) hitDrum(S, 110);
            if ((i & 1) == 0)      hitDrum(i == 14 ? O : H, 78);   // 8th-note hats, open on the last "and"
            if (delayOrKey(step)) { return; }
        }
    }
}

void setup() {
    hardResetCodecPower();
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 1500) {}

    Serial.println();
    if (CrashReport) { Serial.println("!!! CRASH REPORT (previous run) !!!"); Serial.print(CrashReport); }
    Serial.println("=== spike_plaits_drums (Plaits drum models -> GM kit -> TAC5212) ===");
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
    } else {
        Wire.begin();
        Wire.setClock(100000);
        tdspMuxAutoSelectCodec(TAC5212_I2C_ADDRESS);
        setupCodec();
        Serial.println("[setup] codec init done"); Serial.flush();
    }

    AudioMemory(80);
    // Make-up: up to 6 drum voices can pile up; keep each below clip. mixB slot 0
    // is the SUM of mixA (4 voices) so give it more headroom than the two direct
    // voice slots (1,2).
    for (int i = 0; i < 4; i++) mixA.gain(i, 0.7f);
    mixB.gain(0, 0.6f); mixB.gain(1, 0.7f); mixB.gain(2, 0.7f);
    if (g_codecOk) applyVol();

    MIDI.begin(MIDI_CHANNEL_OMNI);
    MIDI.turnThruOff();
    MIDI.setHandleNoteOn(onNoteOn);

    Serial.println("running -- cmds: A=demo beat  1-9=fire a piece  +/-=vol  d=dump codec  i=re-init");
    Serial.println("      pieces: 1 kick  2 snare  3 closed-hat  4 open-hat  5 low-tom  6 hi-tom");
    Serial.println("              7 crash  8 ride  9 clap");
    Serial.println("      ESP32/kit:  r=reset  g=flash mode  @BOOTAPP@=exit flash  U=Teensy prog");

    Serial.println("[setup] settle 2.5s, then late kit.bootApp()..."); Serial.flush();
    delay(2500);
    kit.bootApp();
}

void loop() {
    if (kit.service(Serial)) return;
    serviceVoices();
    while (MIDI.read()) { /* onNoteOn fires */ }

    if (Serial.available()) {
        int c = Serial.read();
        if (!kit.handleChar(Serial, c)) {
            switch (c) {
                case 'A': playBeat(); break;
                case '1': hitDrum(36, 118); break;   // kick
                case '2': hitDrum(38, 112); break;   // snare
                case '3': hitDrum(42, 90);  break;   // closed hat
                case '4': hitDrum(46, 90);  break;   // open hat
                case '5': hitDrum(41, 110); break;   // low tom
                case '6': hitDrum(50, 110); break;   // high tom
                case '7': hitDrum(49, 100); break;   // crash
                case '8': hitDrum(51, 90);  break;   // ride
                case '9': hitDrum(39, 105); break;   // clap
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
        Serial.printf("alive up=%lus  codec=%s(%s)  hits=%lu  outPeak=%.3f  cpuMax=%.1f%% memMax=%u\n",
                      (unsigned long)(millis() / 1000), g_codecOk ? "OK" : "FAIL", g_codecMsg,
                      (unsigned long)g_hits, po, AudioProcessorUsageMax(), AudioMemoryUsageMax());
        AudioProcessorUsageMaxReset();
    }
}
