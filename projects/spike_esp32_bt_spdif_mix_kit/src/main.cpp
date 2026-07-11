// spike_esp32_bt_spdif_mix_kit — spike_esp32_bt_spdif_mix refactored onto the reusable
// lib/TDspProgrammingKit. Identical audio; all ESP32 boot/reset/flash control is now the
// drop-in kit instead of inline code.
//
//   (A) phone --A2DP--> ESP32 (I2S master, 44.1k) --> Teensy SAI2 slave (pin 5)
//         --> AsyncAudioInput<AsyncAudioInputI2S2_16bitslave>  (BT, async-resampled)
//   (B) tone --> S/PDIF OUT (pin 14 optical) --[loopback cable]--> S/PDIF IN (pin 15)
//         --> AsyncAudioInputSPDIF3  (S/PDIF, async-resampled)
//   mix (A)+(B) --> AudioOutputTDM (SAI1) --> TAC5212 DAC --> OUT1/OUT2.
//
// ESP32 control (via the kit):  r=reset->app  g=flash mode  @BOOTAPP@=exit flash
//   U=Teensy program mode.  Audio is paused during flashing via onFlashEnter.
// Needs the pin37->ESP32-EN jumper (see lib/TDspProgrammingKit/README.md).

#include <Arduino.h>
#include <Wire.h>
#include "tdsp_hw_config.h"
#include <Audio.h>
#include <TAC5212.h>
#include <TDspProgrammingKit.h>
#include <MIDI.h>
#include <synth_dexed.h>
#include <SD.h>
#include "async_input.h"
#include "input_i2s2_16bit.h"
#include "DexedVoiceBank.h"
#include "william_tell_mid.h"
#include "moonlight_mid.h"
#include "billie_jean_mid.h"
#include "bohemian_mid.h"
#include "sd_midi.h"   // runtime .mid parser: add songs by copying files to SD /songs

constexpr int     TAC5212_EN_PIN      = 35;     // shared SHDNZ, active-low
constexpr uint8_t TAC5212_I2C_ADDRESS = 0x51;

// Physical MIDI IN: schematic MIDI_RX = Teensy pin 0 (Serial1 RX) via the H11L1
// opto. Drives the Dexed source below. (See projects/spike_midi_dexed.)
MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI);

// --- Audio graph (unchanged from spike_esp32_bt_spdif_mix) ------------------
// tdmClk (SAI1 TDM input) is constructed FIRST so it owns update_responsibility.
AudioInputTDM          tdmClk;               // SAI1 TDM clock + update driver
AudioOutputTDM         tdmOut;               // SAI1 TDM -> TAC5212 DAC (ch0=L, ch1=R)

// Resamplers stay in fast DTCM. They fit alongside Dexed because this build caps
// MAX_FILTER_SAMPLES (see platformio.ini) so each filter[] is ~16KB not ~160KB.
AsyncAudioInputSPDIF3  spdifIn(false, false, 100, 20, 80);  // optical IN, pin 15
AudioOutputSPDIF3      spdifOut;                            // optical OUT, pin 14
AudioSynthWaveformSine spdifTone;                          // tone sent out the optical port

AsyncAudioInput<AsyncAudioInputI2S2_16bitslave> btIn(false, false, 100, 20, 80);

AudioSynthWaveformSine testTone;             // local DAC self-test source
AudioSynthDexed        g_dexed(16, AUDIO_SAMPLE_RATE_EXACT);  // 16-voice 6-op FM
AudioMixer4            outL, outR;           // mix: 0=BT, 1=local tone, 2=S/PDIF-in, 3=Dexed
AudioAnalyzePeak       peakBt, peakSpdif, peakOut;

AudioConnection c_txL    (spdifTone, 0, spdifOut, 0);
AudioConnection c_txR    (spdifTone, 0, spdifOut, 1);
AudioConnection c_btL    (btIn,      0, outL, 0);
AudioConnection c_btR    (btIn,      1, outR, 0);
AudioConnection c_toneL  (testTone,  0, outL, 1);
AudioConnection c_toneR  (testTone,  0, outR, 1);
AudioConnection c_spL    (spdifIn,   0, outL, 2);
AudioConnection c_spR    (spdifIn,   1, outR, 2);
AudioConnection c_dxL    (g_dexed,   0, outL, 3);   // Dexed (mono) -> both channels
AudioConnection c_dxR    (g_dexed,   0, outR, 3);
AudioConnection c_outL   (outL,      0, tdmOut, 0);
AudioConnection c_outR   (outR,      0, tdmOut, 1);
AudioConnection c_pkBt   (btIn,      0, peakBt,    0);
AudioConnection c_pkSp   (spdifIn,   0, peakSpdif, 0);
AudioConnection c_pkOut  (outL,      0, peakOut,   0);

tac5212::TAC5212 g_codec(Wire);

// ESP32 control/flash — the reusable kit (EN=37, IO0=36, Serial7). Pins 28/29/36/37
// don't overlap the audio pins, so it coexists with the audio graph.
TDspProgrammingKit kit;
elapsedMillis hb;

static void hardResetCodecPower() {
    pinMode(TAC5212_EN_PIN, OUTPUT);
    digitalWrite(TAC5212_EN_PIN, LOW);  delay(5);
    digitalWrite(TAC5212_EN_PIN, HIGH); delay(10);
}

// I2C bus recovery (see spike_esp32_bt_spdif_mix for the full rationale): bit-bang SCL to
// free a stuck slave before Wire.begin(), so setup() can never hang. Wire0: SDA=18, SCL=19.
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

// Master headphone volume from the phone app: it arrives as an "@VOL=<pct>" line
// on the ESP32 UART (App -> BLE -> ESP32 -> here). 0..100% -> DAC dB: 0 = mute,
// 1..100 maps linearly across -60..0 dB. Controls the TAC5212 OUT1/OUT2 (HP jack).
static void setMasterVolumePct(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    g_dvol = (pct == 0) ? -128.0f : (-60.0f + 0.60f * (float)pct);
    if (g_codecOk) applyVol();
    Serial.printf("[vol] app set %d%% -> %.1f dB\n", pct, g_dvol);
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

// mixer helper: 0=BT, 1=local test tone, 2=S/PDIF-in (slot 3 = Dexed, set once,
// stays on independently of source-mode switches)
static void setMix(float bt, float tone, float spdif) {
    outL.gain(0, bt);    outR.gain(0, bt);
    outL.gain(1, tone);  outR.gain(1, tone);
    outL.gain(2, spdif); outR.gain(2, spdif);
}

// ============================================================================
// Dexed source — 6-op FM synth played by the physical MIDI IN and the app.
// ============================================================================
// Curated instrument list: index (sent by the app as @DXVOICE=<i>) -> a bundled
// DX7 patch (bank, voice) from dexed_banks_data.h. Bank 2 = the rom1a factory
// cartridge. Keep this list in sync with INSTRUMENTS[] in the app (tdspBle.ts).
struct DxInstrument { uint8_t bank, voice; const char *name; };
static const DxInstrument kInstruments[] = {
    // Keys                                        (bank, voice-index)
    {2, 10, "E.Piano"},     {2,  7, "Grand Piano"}, {0,  0, "FM Rhodes"},
    {3,  2, "E.Piano 2"},   {2, 18, "Harpsichord"}, {2, 19, "Clav"},
    {3,  6, "Celeste"},
    // Organs
    {2, 16, "Organ"},       {2, 17, "Pipe Organ"},
    // Strings / ensemble
    {2,  3, "Strings"},     {6,  2, "String Ens"},  {2,  6, "Orchestra"},
    {8, 17, "Pizzicato"},
    // Brass
    {2,  0, "Brass"},       {6,  5, "Trumpet"},     {8, 11, "Synth Brass"},
    // Winds
    {2, 23, "Flute"},       {8,  5, "Pan Flute"},   {4,  2, "Oboe"},
    {4,  3, "Clarinet"},    {4,  4, "Sax"},         {4, 17, "Harmonica"},
    // Guitar / plucked
    {2, 11, "Guitar"},      {6, 14, "Jazz Guitar"}, {3, 21, "Sitar"},
    {3, 28, "Harp"},
    // Bass
    {2, 14, "Bass"},        {6, 11, "E.Bass"},      {6, 17, "Fretless"},
    // Synth / lead
    {2, 13, "Syn Lead"},    {0, 20, "Mini Moog"},   {0, 12, "Jupiter 8"},
    {0,  7, "Synclavier"},
    // Mallets / bells / perc
    {2, 20, "Vibes"},       {2, 21, "Marimba"},     {4, 23, "Xylophone"},
    {2, 25, "Tub Bells"},   {4, 21, "Glockenspiel"},{2, 26, "Steel Drum"},
    {2, 27, "Timpani"},
    // Voice
    {2, 29, "Voice"},       {1, 29, "Choir"},
};
static const int kNumInstruments = sizeof(kInstruments) / sizeof(kInstruments[0]);
static int g_dxInstrument = 0;

// Load a curated instrument into Dexed (runs from loop/handlers, never the ISR).
static void setDexedInstrument(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= kNumInstruments) idx = kNumInstruments - 1;
    const DxInstrument &in = kInstruments[idx];
    g_dexed.panic();
    if (tdsp::dexed::loadVoice(g_dexed, in.bank, in.voice)) {
        g_dxInstrument = idx;
        Serial.printf("[dexed] instrument %d = %s (bank %d voice %d)\n",
                      idx, in.name, in.bank, in.voice);
    }
}

// --- Non-blocking song sequencer --------------------------------------------
// Song registry: index (sent by the app as @SONG=<i>) -> a transcoded MIDI
// stream. Keep in sync with DX_SONGS[] in the app (tdspBle.ts). The player is
// non-blocking (ticked every loop()) so BT audio, the ESP32 relay, and app
// control keep running and the app can stop/switch it mid-song.
// Built-in songs baked into flash (always available, even with no SD card).
struct BuiltinSong { const char *name; const SongEv *ev; uint32_t count; };
static const BuiltinSong kBuiltinSongs[] = {
    {"William Tell Overture",      kWilliamTellSong, sizeof(kWilliamTellSong) / sizeof(SongEv)},
    {"Moonlight Sonata (3rd Mvt)", kMoonlightSong,   sizeof(kMoonlightSong)   / sizeof(SongEv)},
    {"Billie Jean",                kBillieJeanSong,  sizeof(kBillieJeanSong)  / sizeof(SongEv)},
    {"Bohemian Rhapsody",          kBohemianSong,    sizeof(kBohemianSong)    / sizeof(SongEv)},
};
static const int kNumBuiltin = sizeof(kBuiltinSongs) / sizeof(kBuiltinSongs[0]);

// Unified runtime song list: built-ins first, then /songs/*.mid off the SD card.
// SD songs are parsed on play into g_sdBuf. Adding a song = drop a .mid on the
// card; it appears in the catalog (and the app) with no firmware rebuild.
struct SongRef { char name[40]; const SongEv *ev; uint32_t count; char path[80]; bool sd; };
static SongRef g_songs[48];
static int     g_numSongs = 0;
static bool    g_sdReady  = false;

static const int MAX_SD_EVENTS = 24000;      // longest parseable SD song
DMAMEM static SongEv g_sdBuf[MAX_SD_EVENTS]; // ~96KB in OCRAM (off the DTCM budget)

static bool endsWithMid(const char *s) {
    size_t n = strlen(s);
    return n > 4 && strcasecmp(s + n - 4, ".mid") == 0;
}
static bool songNameExists(const char *name) {   // case-insensitive, for de-dup
    for (int i = 0; i < g_numSongs; ++i)
        if (strcasecmp(g_songs[i].name, name) == 0) return true;
    return false;
}
static void buildSongList() {
    g_numSongs = 0;
    for (int i = 0; i < kNumBuiltin && g_numSongs < (int)(sizeof(g_songs)/sizeof(g_songs[0])); ++i) {
        SongRef &r = g_songs[g_numSongs++];
        strncpy(r.name, kBuiltinSongs[i].name, sizeof(r.name) - 1); r.name[sizeof(r.name) - 1] = 0;
        r.ev = kBuiltinSongs[i].ev; r.count = kBuiltinSongs[i].count; r.sd = false; r.path[0] = 0;
    }
    if (!g_sdReady) return;
    File dir = SD.open("/songs");
    if (!dir || !dir.isDirectory()) { if (dir) dir.close(); Serial.println("[sd] no /songs dir"); return; }
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        const char *nm = f.name();
        if (!f.isDirectory() && nm && endsWithMid(nm) &&
            g_numSongs < (int)(sizeof(g_songs)/sizeof(g_songs[0]))) {
            char disp[40];
            strncpy(disp, nm, sizeof(disp) - 1); disp[sizeof(disp) - 1] = 0;
            size_t ln = strlen(disp); if (ln > 4) disp[ln - 4] = 0;       // strip ".mid"
            // Skip if a built-in (or earlier SD file) already provides this name, so
            // the built-in demo set stays unique and a device with no card still
            // shows a clean list.
            if (!songNameExists(disp)) {
                SongRef &r = g_songs[g_numSongs++];
                snprintf(r.path, sizeof(r.path), "/songs/%s", nm);
                strncpy(r.name, disp, sizeof(r.name) - 1); r.name[sizeof(r.name) - 1] = 0;
                r.ev = nullptr; r.count = 0; r.sd = true;
            }
        }
        f.close();
    }
    dir.close();
    Serial.printf("[sd] songs: %d total (%d built-in + %d SD)\n",
                  g_numSongs, kNumBuiltin, g_numSongs - kNumBuiltin);
}

static bool        g_songOn   = false;
static int         g_songSel  = 0;   // selected / currently-playing song index
static uint32_t    g_songIdx  = 0;
static elapsedMillis g_songClock;
static uint32_t    g_songWait = 0;
static const SongEv *g_curEv    = nullptr;   // active song event stream
static uint32_t      g_curCount = 0;

static void songStart(int idx) {
    if (g_numSongs == 0) return;
    if (idx < 0) idx = 0;
    if (idx >= g_numSongs) idx = g_numSongs - 1;
    g_songSel = idx;
    SongRef &r = g_songs[idx];
    if (r.sd) {
        int n = sdmidi::loadFile(r.path, g_sdBuf, MAX_SD_EVENTS);   // parse from SD (main loop)
        if (n <= 0) { Serial.printf("[song] SD load FAILED: %s\n", r.path); return; }
        g_curEv = g_sdBuf; g_curCount = (uint32_t)n;
        Serial.printf("[song] %s (SD, %d events) -> Dexed (start)\n", r.name, n);
    } else {
        g_curEv = r.ev; g_curCount = r.count;
        Serial.printf("[song] %s -> Dexed (start)\n", r.name);
    }
    if (g_curCount == 0) return;
    g_dexed.panic();
    g_songIdx  = 0;
    g_songWait = g_curEv[0].dms;
    g_songClock = 0;
    g_songOn   = true;
}
static void songStop() {
    if (!g_songOn) return;
    g_songOn = false;
    g_dexed.panic();
    Serial.println("[song] stopped");
}
static void songTick() {
    if (!g_songOn) return;
    const SongEv *ev = g_curEv;
    const uint32_t n = g_curCount;
    while (g_songOn && g_songClock >= g_songWait) {
        g_songClock -= g_songWait;
        const SongEv &e = ev[g_songIdx];
        if (e.vel)       g_dexed.keydown(e.note, e.vel);
        else if (e.note) g_dexed.keyup(e.note);
        if (++g_songIdx >= n) {
            g_dexed.panic();
            g_songOn = false;
            Serial.println("[song] done");
            return;
        }
        g_songWait = ev[g_songIdx].dms;
    }
}

// Stream the device catalog (song + instrument names, '|'-delimited) to the ESP32
// over the UART link. The ESP32 serves it on BLE so the app renders its pickers
// from whatever the device reports — adding a song/instrument is then a firmware
// change only, no app update. Sent when the ESP32 asks (@GETCAT, on BLE connect).
static void sendCatalog() {
    kit.uart().print("@SONGS=");
    for (int i = 0; i < g_numSongs; ++i) { if (i) kit.uart().print('|'); kit.uart().print(g_songs[i].name); }
    kit.uart().print('\n');
    kit.uart().print("@INSTR=");
    for (int i = 0; i < kNumInstruments; ++i) { if (i) kit.uart().print('|'); kit.uart().print(kInstruments[i].name); }
    kit.uart().print('\n');
    Serial.println("[cat] catalog sent to ESP32");
}

// --- MIDI IN (Serial1 DIN) -> Dexed -----------------------------------------
static void onNoteOn(byte, byte note, byte vel) {
    if (vel == 0) { g_dexed.keyup(note); return; }
    g_dexed.keydown(note, vel);
}
static void onNoteOff(byte, byte note, byte) { g_dexed.keyup(note); }
static void onPitchBend(byte, int bend) { g_dexed.setPitchbendRange(2); g_dexed.setPitchbend((int16_t)bend); }
static void onControlChange(byte, byte cc, byte val) {
    if (cc == 1)  g_dexed.setModWheel(val);
    if (cc == 64) g_dexed.setSustain(val >= 64);
    if (cc == 123 && val == 0) g_dexed.panic();
}

void setup() {
    hardResetCodecPower();

    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 1500) {}

    Serial.println();
    if (CrashReport) { Serial.println("!!! CRASH REPORT (previous run) !!!"); Serial.print(CrashReport); }
    Serial.println("=== spike_esp32_bt_spdif_mix_kit (TDspProgrammingKit) ===");
    Serial.println("MIX: (A) ESP32 A2DP  +  (B) S/PDIF optical loopback tone  -> TAC5212.");
    Serial.println("Connect a TOSLINK cable pin14(OUT)->pin15(IN). Pair 'T-DSP' and play.");

    // Pause the audio graph while flashing the ESP32 so the USB<->ESP32 passthrough
    // isn't CPU-starved into dropping bytes.
    kit.onFlashEnter([] { AudioNoInterrupts(); });

    // Boot the ESP32 into its A2DP app FIRST (frees the shared I2C bus), holding EN+IO0
    // high. kit.begin() also sets up the LED (heartbeat).
    Serial.println("[setup] kit.begin() -> boot ESP32 into app (EN+IO0 held)..."); Serial.flush();
    kit.begin();
    delay(300);

    Serial.println("[setup] i2c bus recover..."); Serial.flush();
    i2cBusRecover();
    pinMode(18 /*SDA0*/, INPUT_PULLUP); delayMicroseconds(20);
    if (digitalRead(18) == LOW) {
        g_codecOk = false; g_codecMsg = "i2c wedged - skipped";
        Serial.println("[setup] !! I2C SDA STILL LOW -> SKIP codec init. "
                       "Use 'i' after the bus frees.");
        Serial.flush();
    } else {
        Serial.println("[setup] Wire.begin..."); Serial.flush();
        Wire.begin();
        Wire.setClock(100000);
        tdspMuxAutoSelectCodec(TAC5212_I2C_ADDRESS);
        setupCodec();
        Serial.println("[setup] codec init done"); Serial.flush();
    }

    // SD card (Teensy 4.1 built-in slot): scan /songs/*.mid so songs can be added
    // by copying files to the card. Falls back to the built-in songs if no card.
#if TDSP_HAS_SDCARD
    g_sdReady = SD.begin(BUILTIN_SDCARD);
    Serial.printf("[sd] card %s\n", g_sdReady ? "ready" : "not present");
#endif
    buildSongList();

    AudioMemory(60);
    setMix(1.0f, 0.0f, 1.0f);
    outL.gain(3, 0.5f);  outR.gain(3, 0.5f);   // Dexed source (0.5 = headroom for
                                                 // dense 16-voice passages, avoid clip)
    testTone.frequency(440.0f);  testTone.amplitude(0.0f);
    spdifTone.frequency(1000.0f); spdifTone.amplitude(0.25f);
    if (g_codecOk) applyVol();

    // Physical MIDI IN on Serial1 (pin 0) -> Dexed, omni. Soft-thru off.
    MIDI.begin(MIDI_CHANNEL_OMNI);
    MIDI.turnThruOff();
    MIDI.setHandleNoteOn(onNoteOn);
    MIDI.setHandleNoteOff(onNoteOff);
    MIDI.setHandlePitchBend(onPitchBend);
    MIDI.setHandleControlChange(onControlChange);
    g_dexed.setPitchbendRange(2);
    g_dexed.setPitchbend((int16_t)0);
    g_dexed.setModWheel(0);
    g_dexed.setSustain(false);
    setDexedInstrument(g_dxInstrument);        // default: E.Piano

    Serial.println("running -- cmds: t=DACtone a=BT+SPDIF mix  s=SPDIF-only  m=BT-only");
    Serial.println("                 x=toggle SPDIF tone  +/-=vol  d=dump  i=re-init codec");
    Serial.println("                 W=play/stop song  S=next song  V=next instrument   MIDI-IN pin0");
    Serial.println("      ESP32/kit:  r=reset  g=flash mode  @BOOTAPP@=exit flash  U=Teensy prog");
    Serial.println("                 P=ESP32 pairing mode  F=ESP32 forget bond + pair");

    // LATE, SETTLED reset — the automatic "press BOOT for you" once everything's configured.
    Serial.println("[setup] settle 2.5s, then late kit.bootApp()..."); Serial.flush();
    delay(2500);
    kit.bootApp();
}

void loop() {
    // Flash-mode passthrough owns the loop (also handles @BOOTAPP@); in run mode this
    // ticks the slow LED heartbeat and returns false.
    if (kit.service(Serial)) return;

    // Dexed source: drain physical MIDI IN and advance the (non-blocking) song.
    while (MIDI.read()) { /* handlers fire per message */ }
    songTick();

    if (Serial.available()) {
        int c = Serial.read();
        if (!kit.handleChar(Serial, c)) {     // g / r / U handled by the kit
            if (c == 'P') { kit.uart().write('p'); Serial.println("[cmd] -> ESP32: ENTER pairing mode"); }
            else if (c == 'F') { kit.uart().write('f'); Serial.println("[cmd] -> ESP32: FORGET bond + pairing mode"); }
            else if (c == 't') { testTone.amplitude(0.4f); setMix(0.0f, 1.0f, 0.0f);
                                 Serial.println("[cmd] local DAC tone 440Hz -> BOTH"); }
            else if (c == 'a') { testTone.amplitude(0.0f); setMix(1.0f, 0.0f, 1.0f);
                                 Serial.println("[cmd] MIX: BT + S/PDIF"); }
            else if (c == 's') { testTone.amplitude(0.0f); setMix(0.0f, 0.0f, 1.0f);
                                 Serial.println("[cmd] S/PDIF-in only"); }
            else if (c == 'm') { testTone.amplitude(0.0f); setMix(1.0f, 0.0f, 0.0f);
                                 Serial.println("[cmd] Bluetooth only"); }
            else if (c == 'x') { static bool on = true; on = !on;
                                 spdifTone.amplitude(on ? 0.25f : 0.0f);
                                 Serial.printf("[cmd] S/PDIF out tone %s\n", on ? "ON" : "OFF"); }
            else if (c == '+') { g_dvol += 3.0f; if (g_dvol > 0) g_dvol = 0;
                                 applyVol(); Serial.printf("[cmd] vol %.0f dB\n", g_dvol); }
            else if (c == '-') { g_dvol -= 3.0f; if (g_dvol < -60) g_dvol = -60;
                                 applyVol(); Serial.printf("[cmd] vol %.0f dB\n", g_dvol); }
            else if (c == 'd') { Serial.printf("[reg] RX_OFF(26)=%02X RX_CH1(28)=%02X RX_CH2(29)=%02X "
                                 "CH_EN(76)=%02X PWR(78)=%02X\n",
                                 g_codec.readRegister(0, 0x26), g_codec.readRegister(0, 0x28),
                                 g_codec.readRegister(0, 0x29), g_codec.readRegister(0, 0x76),
                                 g_codec.readRegister(0, 0x78)); }
            else if (c == 'i') { Serial.println("[cmd] re-init codec"); setupCodec(); applyVol();
                                 Serial.printf("[cmd] codec=%s (%s), vol %.0f dB\n",
                                               g_codecOk ? "OK" : "FAIL", g_codecMsg, g_dvol); }
            else if (c == 'W') { if (g_songOn) songStop(); else songStart(g_songSel); }  // play/stop
            else if (c == 'S') { if (g_numSongs) g_songSel = (g_songSel + 1) % g_numSongs;  // pick song
                                 Serial.printf("[song] selected: %s\n", g_songs[g_songSel].name); }
            else if (c == 'V') { setDexedInstrument((g_dxInstrument + 1) % kNumInstruments); }
        }
    }

    // Mirror the ESP32's UART log to USB, line-buffered with an [esp] prefix.
    static char line[160];
    static size_t n = 0;
    while (kit.uart().available()) {
        char c = (char)kit.uart().read();
        if (c == '\n' || n >= sizeof(line) - 1) {
            line[n] = 0;
            if (n) {
                // Control lines from the ESP32 (relayed from the BLE app) are acted
                // on here; everything else is just mirrored to USB with an [esp] tag.
                if (strncmp(line, "@VOL=", 5) == 0) setMasterVolumePct(atoi(line + 5));
                else if (strncmp(line, "@DXVOICE=", 9) == 0) setDexedInstrument(atoi(line + 9));
                else if (strncmp(line, "@SONG=", 6) == 0) {
                    if (strcmp(line + 6, "stop") == 0) songStop();
                    else songStart(atoi(line + 6));   // @SONG=<song index>
                }
                else if (strcmp(line, "@GETCAT") == 0) sendCatalog();  // ESP32 wants the catalog
                else Serial.printf("[esp] %s\n", line);
            }
            n = 0;
        } else if (c != '\r') {
            line[n++] = c;
        }
    }

    // Status heartbeat print (the LED itself is driven by kit.service()).
    if (hb >= 1000) {
        hb = 0;
        float pbt = peakBt.available()    ? peakBt.read()    : 0.0f;
        float psp = peakSpdif.available() ? peakSpdif.read() : 0.0f;
        float po  = peakOut.available()   ? peakOut.read()   : 0.0f;
        Serial.printf("alive up=%lus  codec=%s(%s)  spdif=%s inFreq=%.0f  "
                      "btPeak=%.3f spdifPeak=%.3f outPeak=%.3f  cpuMax=%.1f%% memMax=%u\n",
                      (unsigned long)(millis() / 1000),
                      g_codecOk ? "OK" : "FAIL", g_codecMsg,
                      AsyncAudioInputSPDIF3::isLocked() ? "LOCKED" : "no-signal",
                      spdifIn.getInputFrequency(), pbt, psp, po,
                      AudioProcessorUsageMax(), AudioMemoryUsageMax());
        AudioProcessorUsageMaxReset();   // make cpuMax a per-second rolling peak
        AudioMemoryUsageMaxReset();
    }
}
