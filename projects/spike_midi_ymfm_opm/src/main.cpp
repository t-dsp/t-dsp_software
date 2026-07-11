// spike_midi_ymfm_opm — MIDI-IN driving a MULTITIMBRAL bank of ymfm YM2151 (OPM)
// FM synths. Four independent emulated OPM chips ("banks"), each with its own
// patch and 8 voices, are routed by MIDI channel and mixed to the DAC:
//
//   MIDI IN (pin 0) / serial demo ─▶ YmfmOpmMulti ─▶ bank0 (Organ)     ─┐
//                                     channel→bank    bank1 (E.Piano)   ├─▶ mixL/mixR
//                                                     bank2 (FM Bass)   │   (AudioMixer4)
//                                                     bank3 (Bell/Vibes)┘        │
//                                              AudioOutputTDM (SAI1) ─▶ TAC5212 ─▶ HP out
//
// Four banks = four distinct simultaneous timbres and up to 32-voice total
// polyphony. AudioSynthYmfmOPM's idle gate means a bank with no notes costs
// ~nothing, so running four is cheap until they actually play (watch cpuMax).
//
// The codec / TDM / ESP32-kit setup is copied verbatim from spike_midi_dexed
// (known-good on this board), so the only new moving parts are the OPM engines
// and the multitimbral manager.
//
// Bring-up path:
//   1. '1'..'4' play a note on each bank — proves all four chips sound + distinct.
//   2. 'm' plays a note on all four banks AT ONCE — the multi-bank headline.
//   3. 'c' plays an 8-note chord on one bank; 'W' spreads a scale across banks.
//   4. Play a MIDI keyboard into the DIN (omni → ch1 → bank0), or set its channel.
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
#include <YmfmOpmMulti.h>
#include <OpmBank.h>          // VOPM .opm parser
#include "opm_bank_data.h"    // baked demo bank (also parser test data)
#if TDSP_HAS_SDCARD
#include <SD.h>              // /ymfm/*.opm banks off the microSD
#endif

constexpr int     TAC5212_EN_PIN      = 35;     // shared SHDNZ, active-low
constexpr uint8_t TAC5212_I2C_ADDRESS = 0x51;

// Physical MIDI IN on the schematic's MIDI_RX (Teensy pin 0 = Serial1 RX)
// through the H11L1 optoisolator. The library drives Serial1 at 31250 baud.
MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI);

// --- Audio graph ------------------------------------------------------------
// tdmClk (SAI1 TDM input) is constructed FIRST so it owns update_responsibility
// exactly as in spike_midi_dexed; tdmOut is the actual DAC feed. tdmClk is left
// unconnected — it only keeps the SAI1 clock/update wiring identical.
AudioInputTDM          tdmClk;               // SAI1 TDM clock + update driver
AudioOutputTDM         tdmOut;               // SAI1 TDM -> TAC5212 DAC (ch0=L, ch1=R)

constexpr int kNumBanks = 4;
AudioSynthYmfmOPM      bank0, bank1, bank2, bank3;   // four independent OPM chips
AudioMixer4            mixL, mixR;                    // one input per bank (stereo)
AudioAnalyzePeak       peakOut;

// Each bank is stereo (output 0 = L, 1 = R) -> mixer input = bank index.
AudioConnection c_b0L(bank0, 0, mixL, 0);  AudioConnection c_b0R(bank0, 1, mixR, 0);
AudioConnection c_b1L(bank1, 0, mixL, 1);  AudioConnection c_b1R(bank1, 1, mixR, 1);
AudioConnection c_b2L(bank2, 0, mixL, 2);  AudioConnection c_b2R(bank2, 1, mixR, 2);
AudioConnection c_b3L(bank3, 0, mixL, 3);  AudioConnection c_b3R(bank3, 1, mixR, 3);
AudioConnection c_outL(mixL, 0, tdmOut, 0);
AudioConnection c_outR(mixR, 0, tdmOut, 1);
AudioConnection c_pkOut(mixL, 0, peakOut, 0);

AudioSynthYmfmOPM *g_bankPtrs[kNumBanks] = { &bank0, &bank1, &bank2, &bank3 };
tdsp::ymfmopm::YmfmOpmMulti g_multi(g_bankPtrs, kNumBanks);

// Per-bank default instruments (also the Program Change voice table).
static const tdsp::ymfmopm::OpmVoice *kVoices[] = {
    &tdsp::ymfmopm::kAdditiveOrgan,   // bank 0 / ch 1
    &tdsp::ymfmopm::kElectricPiano,   // bank 1 / ch 2
    &tdsp::ymfmopm::kFmBass,          // bank 2 / ch 3
    &tdsp::ymfmopm::kBellVibes,       // bank 3 / ch 4
};
static const int kNumVoices = sizeof(kVoices) / sizeof(kVoices[0]);

// --- Instrument catalog: baked .opm bank + SD /ymfm/*.opm -------------------
// All available instruments are parsed once into g_instr[] (baked bank first,
// then every /ymfm/*.opm on the card). Loading an instrument is then just
// setVoice(g_instr[i]) — the engine keeps its own copy. Drop VOPM .opm files
// into /ymfm on the SD card (via a reader) to add instruments with no rebuild.
static const int kMaxInstr = 64;
DMAMEM static tdsp::ymfmopm::OpmVoice g_instr[kMaxInstr];   // parsed instruments (OCRAM)
static int  g_numInstr = 0;
static bool g_sdReady  = false;
DMAMEM static char g_opmFileBuf[24000];   // scratch to read one .opm file (OCRAM)

static int  g_targetBank = 0;                       // which bank 'V' re-instruments
static int  g_bankInstr[kNumBanks] = { 0, 1, 2, 3 };// current catalog index per bank

static bool endsWithOpm(const char *s) {
    size_t n = strlen(s);
    return n > 4 && strcasecmp(s + n - 4, ".opm") == 0;
}

#if TDSP_HAS_SDCARD
// Read every *.opm in `dir`, parsing each file's voices onto the end of g_instr[].
static void scanYmfmDir(const char *dir) {
    File d = SD.open(dir);
    if (!d || !d.isDirectory()) { if (d) d.close(); return; }
    for (File f = d.openNextFile(); f && g_numInstr < kMaxInstr; f = d.openNextFile()) {
        const char *nm = f.name();
        if (!f.isDirectory() && nm && endsWithOpm(nm)) {
            size_t n = f.read(g_opmFileBuf, sizeof(g_opmFileBuf) - 1);
            g_opmFileBuf[n] = 0;
            int got = tdsp::ymfmopm::parseOpmBank(g_opmFileBuf, n,
                                                  g_instr + g_numInstr, kMaxInstr - g_numInstr);
            Serial.printf("[ymfm] %s: +%d instruments\n", nm, got);
            g_numInstr += got;
        }
        f.close();
    }
    d.close();
}
#endif

// Build the whole instrument catalog: baked bank, then SD /ymfm.
static void buildInstruments() {
    g_numInstr = tdsp::ymfmopm::parseOpmBank(kBakedOpmBank, strlen(kBakedOpmBank),
                                             g_instr, kMaxInstr);
    Serial.printf("[opm] baked bank parsed: %d instruments\n", g_numInstr);
#if TDSP_HAS_SDCARD
    if (g_sdReady) {
        if (!SD.exists("/ymfm")) SD.mkdir("/ymfm");   // a home to drop .opm banks into
        scanYmfmDir("/ymfm");
    }
#endif
    Serial.printf("[opm] catalog total: %d instruments\n", g_numInstr);
    for (int i = 0; i < g_numInstr; ++i) Serial.printf("   %2d: %s\n", i, g_instr[i].name);
}

// Load catalog instrument `idx` into `bank` (wraps).
static void loadInstrument(int bank, int idx) {
    if (g_numInstr == 0 || bank < 0 || bank >= kNumBanks) return;
    idx = ((idx % g_numInstr) + g_numInstr) % g_numInstr;
    g_bankInstr[bank] = idx;
    g_multi.setBankVoice(bank, g_instr[idx]);
    Serial.printf("[opm] bank %d (ch%d) = instrument %d \"%s\"\n", bank, bank + 1, idx, g_instr[idx].name);
}

tac5212::TAC5212 g_codec(Wire);

// ESP32 control/flash — reusable kit (EN=37, IO0=36, Serial7). Non-overlapping
// pins; kept for touch-free Teensy flashing + LED heartbeat + ESP32 boot.
TDspProgrammingKit kit;
elapsedMillis hb;

static uint32_t g_noteOnCount = 0;      // lifetime note-on tally (heartbeat)

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

// --- MIDI handlers (Serial1 DIN) -> multitimbral manager --------------------
static void onNoteOn(byte ch, byte note, byte vel) {
    if (vel == 0) { g_multi.noteOff(ch, note); return; }   // running-status note-off
    g_multi.noteOn(ch, note, vel);
    g_noteOnCount++;
    Serial.printf("[midi] noteOn  ch%-2d n%-3d v%-3d -> bank %d\n", ch, note, vel, g_multi.bankForChannel(ch));
}
static void onNoteOff(byte ch, byte note, byte /*vel*/) { g_multi.noteOff(ch, note); }
static void onProgramChange(byte ch, byte prog) {
    g_multi.programChange(ch, prog);
    Serial.printf("[midi] program ch%-2d -> %d (bank %d)\n", ch, prog, g_multi.bankForChannel(ch));
}
static void onControlChange(byte /*ch*/, byte cc, byte val) {
    if (cc == 123 && val == 0) g_multi.allNotesOff();      // all notes off
}

// Blocking helpers for the serial demos (interruptible by any serial key).
static bool delayOrKey(uint16_t ms) {
    uint32_t t0 = millis();
    while (millis() - t0 < ms) { if (Serial.available()) { Serial.read(); return true; } }
    return false;
}
// One quick note on a given channel (its bank's timbre).
static void demoNote(uint8_t ch, uint8_t note, uint16_t ms) {
    g_multi.noteOn(ch, note, 110);
    delayOrKey(ms);
    g_multi.noteOff(ch, note);
}
// A scale whose successive notes hop across the four banks, so you hear all four
// timbres interleaved from one melody.
static void playSpread() {
    static const uint8_t scale[] = { 60, 62, 64, 65, 67, 69, 71, 72 };
    Serial.println("[cmd] scale spread across 4 banks (press any key to stop)");
    for (int i = 0; i < (int)(sizeof(scale)); i++) {
        uint8_t ch = (uint8_t)(1 + (i % kNumBanks));   // ch 1..4 -> bank 0..3
        g_multi.noteOn(ch, scale[i], 110);
        if (delayOrKey(200)) { g_multi.allNotesOff(); return; }
        g_multi.noteOff(ch, scale[i]);
    }
}

void setup() {
    hardResetCodecPower();

    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 1500) {}

    Serial.println();
    if (CrashReport) { Serial.println("!!! CRASH REPORT (previous run) !!!"); Serial.print(CrashReport); }
    Serial.println("=== spike_midi_ymfm_opm (MULTITIMBRAL: 4x ymfm YM2151/OPM -> TAC5212) ===");
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

    // Bigger pool than a single bank: four engines each allocate 2 blocks/update.
    AudioMemory(80);
    // Per-bank make-up. Four banks summed at once must stay under full scale, so
    // 0.7 per input keeps a 4-bank fff pile-up (~4 x 0.34) just under 1.0 while a
    // single bank still lands at a usable level.
    for (int i = 0; i < kNumBanks; i++) { mixL.gain(i, 0.7f); mixR.gain(i, 0.7f); }
    if (g_codecOk) applyVol();

    // Bring up all four OPM banks.
    g_multi.begin();

    // SD card + instrument catalog: baked .opm bank, then /ymfm/*.opm off the card.
#if TDSP_HAS_SDCARD
    g_sdReady = SD.begin(BUILTIN_SDCARD);
    Serial.printf("[sd] card %s\n", g_sdReady ? "ready" : "not present");
#endif
    buildInstruments();

    // Seed the four banks from the catalog (fallback to the code presets if empty).
    if (g_numInstr >= kNumBanks) {
        for (int i = 0; i < kNumBanks; i++) loadInstrument(i, i);
    } else {
        for (int i = 0; i < kNumBanks; i++) g_multi.setBankVoice(i, *kVoices[i]);
    }
    g_multi.setVoiceTable(kVoices, kNumVoices);   // MIDI Program Change -> code preset

    // MIDI: consume all channels (omni). A plain keyboard sends ch1 -> bank 0;
    // set the keyboard's channel (or use Program Change) to reach other banks.
    MIDI.begin(MIDI_CHANNEL_OMNI);
    MIDI.turnThruOff();
    MIDI.setHandleNoteOn(onNoteOn);
    MIDI.setHandleNoteOff(onNoteOff);
    MIDI.setHandleProgramChange(onProgramChange);
    MIDI.setHandleControlChange(onControlChange);

    Serial.println("running -- cmds: 1/2/3/4=note on each bank  m=all banks at once");
    Serial.println("                 c=8-note chord (bank0)  W=scale spread across banks");
    Serial.println("                 b=select target bank  V=next instrument (on target bank)");
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
            if (c >= '1' && c <= '4') {        // note on one bank (its own timbre)
                uint8_t ch = (uint8_t)(c - '0');
                Serial.printf("[cmd] note on bank %d (ch%d), 500ms\n", ch - 1, ch);
                demoNote(ch, 60, 500);
            }
            else if (c == 'm') {               // ALL banks at once — the headline
                Serial.println("[cmd] all 4 banks at once (C E G C across banks), 800ms");
                g_multi.noteOn(1, 60, 110); g_multi.noteOn(2, 64, 110);
                g_multi.noteOn(3, 43, 110); g_multi.noteOn(4, 72, 110);
                delay(800);
                g_multi.noteOff(1, 60); g_multi.noteOff(2, 64);
                g_multi.noteOff(3, 43); g_multi.noteOff(4, 72);
            }
            else if (c == 'c') {               // 8-note chord within bank 0
                Serial.println("[cmd] 8-note chord on bank0 (ch1), 700ms");
                static const uint8_t chord[] = { 48, 52, 55, 60, 64, 67, 72, 76 };
                for (uint8_t n : chord) g_multi.noteOn(1, n, 100);
                delay(700);
                for (uint8_t n : chord) g_multi.noteOff(1, n);
            }
            else if (c == 'W') { playSpread(); }
            else if (c == 'b') { g_targetBank = (g_targetBank + 1) % kNumBanks;
                                 Serial.printf("[cmd] target bank = %d (ch%d), instrument \"%s\"\n",
                                     g_targetBank, g_targetBank + 1,
                                     g_numInstr ? g_instr[g_bankInstr[g_targetBank]].name : "-"); }
            else if (c == 'V') { loadInstrument(g_targetBank, g_bankInstr[g_targetBank] + 1); }
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
        Serial.printf("alive up=%lus  codec=%s(%s)  instr=%d  voices[b0..3]=%d/%d/%d/%d tot=%d  "
                      "outPeak=%.3f  cpuMax=%.1f%% memMax=%u\n",
                      (unsigned long)(millis() / 1000),
                      g_codecOk ? "OK" : "FAIL", g_codecMsg, g_numInstr,
                      bank0.activeVoices(), bank1.activeVoices(),
                      bank2.activeVoices(), bank3.activeVoices(), g_multi.activeVoices(),
                      po, AudioProcessorUsageMax(), AudioMemoryUsageMax());
        AudioProcessorUsageMaxReset();
    }
}
