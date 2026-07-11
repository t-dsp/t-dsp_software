// spike_midi_player — synth-agnostic MIDI file player bring-up.
//
//   built-in song (baked)  ─┐
//   /songs/*.mid on SD  ────┼─▶ tdsp::MidiFilePlayer ─▶ tdsp::MidiSink ─▶ [ SYNTH ]
//   physical MIDI IN (pin0) ─┘        (non-blocking tick)                    │
//                                                                            ▼
//                            AudioMixer4 ─▶ AudioOutputTDM (SAI1) ─▶ TAC5212 DAC ─▶ HP out
//
// The whole point of this spike: the player and the song catalog talk ONLY to
// the tdsp::MidiSink interface, so the synth engine is a compile-time choice.
//
//   -D TDSP_SYNTH_DEXED   (default) → AudioSynthDexed (6-op FM) via DexedSink
//   -D TDSP_SYNTH_YMFM              → the ymfm port's engine via YmfmSink
//                                     (provided by the separate ymfm spike)
//
// Everything below the "SYNTH BACKEND" section is engine-independent. Swapping
// synths does not touch the parser, the player, the SD catalog, or the codec.
//
// Codec / TDM / ESP32-kit setup is copied from the known-good spike_midi_dexed
// so that if audio is silent it is the player/synth code, not the plumbing.

#include <Arduino.h>
#include <Wire.h>
#include "tdsp_hw_config.h"
#include <Audio.h>
#include <TAC5212.h>
#include <TDspProgrammingKit.h>
#include <MIDI.h>
#include <SD.h>

#include <MidiFilePlayer.h>          // lib/TDspMidiPlayer — synth-agnostic player
#include <MidiSmfFile.h>             // SD .mid loader (pulls in <SD.h>)
#include "song_event.h"             // legacy SongEv (baked built-in song)
#include "william_tell_mid.h"        // one baked demo song

constexpr int     TAC5212_EN_PIN      = 35;     // shared SHDNZ, active-low
constexpr uint8_t TAC5212_I2C_ADDRESS = 0x51;

// Physical MIDI IN: schematic MIDI_RX = Teensy pin 0 (Serial1 RX) via H11L1 opto.
MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI);

// --- Engine-independent audio graph -----------------------------------------
// tdmClk (SAI1 TDM input) is constructed FIRST so it owns update_responsibility
// exactly as in spike_midi_dexed; tdmOut is the actual DAC feed. tdmClk is left
// unconnected — it only keeps the SAI1 clock/update wiring identical.
AudioInputTDM   tdmClk;                          // SAI1 TDM clock + update driver
AudioOutputTDM  tdmOut;                           // SAI1 TDM -> TAC5212 DAC
AudioMixer4     outL, outR;                        // slot 0 = synth (mono, fanned L+R)
AudioAnalyzePeak peakOut;
AudioConnection c_outL (outL, 0, tdmOut, 0);
AudioConnection c_outR (outR, 0, tdmOut, 1);
AudioConnection c_pkOut(outL, 0, peakOut, 0);

// ============================================================================
// SYNTH BACKEND — the only synth-specific code. Selected by the build flag.
// Each branch must provide:
//   * a file-scope engine wired into outL/outR slot 0,
//   * tdsp::MidiSink* g_synthSink   (the player + live MIDI drive this),
//   * void  synthBegin();           (load an initial patch / gain)
//   * void  synthNextInstrument();  (cycle demo instrument; no-op if N/A)
//   * const char* synthName();      (label for the heartbeat)
// ============================================================================
#if defined(TDSP_SYNTH_YMFM)
  // ---- ymfm backend --------------------------------------------------------
  // Wired up when the ymfm port lands: instantiate its AudioStream engine,
  // connect it to outL/outR slot 0, wrap it in a YmfmSink (tdsp::MidiSink),
  // and point g_synthSink at it. See lib/TDspMidiPlayer/README.md.
  #error "TDSP_SYNTH_YMFM selected but no ymfm engine is wired in this spike yet. \
Provide YmfmSink (a tdsp::MidiSink) + the engine node, set g_synthSink, then \
delete this #error. Until then build the default (Dexed) target."

#else  // ---- Dexed backend (default) -----------------------------------------
  #include <synth_dexed.h>
  #include "DexedSink.h"
  #include "DexedVoiceBank.h"

  AudioSynthDexed g_dexed(16, AUDIO_SAMPLE_RATE_EXACT);   // 16-voice 6-op FM
  AudioConnection c_dxL(g_dexed, 0, outL, 0);
  AudioConnection c_dxR(g_dexed, 0, outR, 0);
  DexedSink       g_dexedSink(&g_dexed);
  tdsp::MidiSink *g_synthSink = &g_dexedSink;

  // Curated demo instruments (bank, voice) from the bundled DX7 banks.
  struct DxInstrument { uint8_t bank, voice; const char *name; };
  static const DxInstrument kInstruments[] = {
      {2, 10, "E.Piano"},  {2, 7, "Grand Piano"}, {0, 0, "FM Rhodes"},
      {2, 16, "Organ"},    {2, 3, "Strings"},     {2, 0, "Brass"},
      {2, 23, "Flute"},    {2, 11, "Guitar"},     {2, 14, "Bass"},
      {2, 13, "Syn Lead"}, {2, 20, "Vibes"},      {2, 29, "Voice"},
  };
  static const int kNumInstruments = sizeof(kInstruments) / sizeof(kInstruments[0]);
  static int g_dxInstrument = 0;

  static void loadInstrument(int idx) {
      if (idx < 0) idx = kNumInstruments - 1;
      if (idx >= kNumInstruments) idx = 0;
      const DxInstrument &in = kInstruments[idx];
      g_dexed.panic();
      if (tdsp::dexed::loadVoice(g_dexed, in.bank, in.voice)) {
          g_dxInstrument = idx;
          Serial.printf("[dexed] instrument %d = %s\n", idx, in.name);
      }
  }

  static void synthBegin() {
      g_dexed.setGain(0.8f);                 // stay under the float->q15 clip rail
      g_dexedSink.setListenChannel(0);       // omni: one patch plays every channel
      loadInstrument(0);
  }
  static void synthNextInstrument() { loadInstrument(g_dxInstrument + 1); }
  static const char *synthName()    { return "Dexed"; }
#endif  // synth backend

// --- Synth-agnostic player + song catalog -----------------------------------
tdsp::MidiFilePlayer g_player;

// Play buffer shared by baked + SD songs (only one song plays at a time).
// ~144 KB in OCRAM (DMAMEM) so it stays off the DTCM budget.
static const int MAX_EVENTS = 24000;
DMAMEM static tdsp::MidiFileEvent g_buf[MAX_EVENTS];

struct SongRef { char name[48]; char path[96]; bool sd; const SongEv *baked; uint32_t bakedCount; };
static SongRef g_songs[48];
static int     g_numSongs = 0;
static int     g_songSel  = 0;
static bool    g_sdReady  = false;

static bool endsWithMid(const char *s) {
    size_t n = strlen(s);
    return n > 4 && strcasecmp(s + n - 4, ".mid") == 0;
}
static bool songNameExists(const char *name) {
    for (int i = 0; i < g_numSongs; ++i)
        if (strcasecmp(g_songs[i].name, name) == 0) return true;
    return false;
}
static void scanSongDir(const char *dir) {
    File d = SD.open(dir);
    if (!d || !d.isDirectory()) { if (d) d.close(); return; }
    const int cap = (int)(sizeof(g_songs) / sizeof(g_songs[0]));
    for (File f = d.openNextFile(); f && g_numSongs < cap; f = d.openNextFile()) {
        const char *nm = f.name();
        if (!f.isDirectory() && nm && endsWithMid(nm)) {
            char disp[48];
            size_t copy = strlen(nm) - 4;
            if (copy > sizeof(disp) - 1) copy = sizeof(disp) - 1;
            memcpy(disp, nm, copy); disp[copy] = 0;
            if (!songNameExists(disp)) {
                SongRef &r = g_songs[g_numSongs++];
                if (strcmp(dir, "/") == 0) snprintf(r.path, sizeof(r.path), "/%s", nm);
                else                       snprintf(r.path, sizeof(r.path), "%s/%s", dir, nm);
                strncpy(r.name, disp, sizeof(r.name) - 1); r.name[sizeof(r.name) - 1] = 0;
                r.sd = true; r.baked = nullptr; r.bakedCount = 0;
            }
        }
        f.close();
    }
    d.close();
}
static void buildSongList() {
    g_numSongs = 0;
    // One baked built-in so the spike demos with no SD card present.
    SongRef &b = g_songs[g_numSongs++];
    strncpy(b.name, "William Tell (baked)", sizeof(b.name) - 1); b.name[sizeof(b.name) - 1] = 0;
    b.sd = false; b.path[0] = 0;
    b.baked = kWilliamTellSong; b.bakedCount = sizeof(kWilliamTellSong) / sizeof(SongEv);
#if TDSP_HAS_SDCARD
    if (g_sdReady) {
        if (!SD.exists("/songs")) SD.mkdir("/songs");
        scanSongDir("/songs");
        scanSongDir("/");
    }
#endif
    Serial.printf("[cat] %d songs (1 baked + %d SD)\n", g_numSongs, g_numSongs - 1);
    for (int i = 0; i < g_numSongs; ++i) Serial.printf("   %2d: %s\n", i, g_songs[i].name);
}

static void songPlay(int idx) {
    if (g_numSongs == 0) return;
    if (idx < 0) idx = 0;
    if (idx >= g_numSongs) idx = g_numSongs - 1;
    g_songSel = idx;
    SongRef &r = g_songs[idx];
    uint32_t n = 0;
    if (r.sd) {
        int got = tdsp::smf::loadSmfFile(r.path, g_buf, MAX_EVENTS);
        if (got <= 0) { Serial.printf("[song] SD load FAILED: %s\n", r.path); return; }
        n = (uint32_t)got;
        Serial.printf("[song] %s (SD, %lu events)\n", r.name, (unsigned long)n);
    } else {
        n = tdsp::expandLegacyNotes(r.baked, r.bakedCount, g_buf, MAX_EVENTS);
        Serial.printf("[song] %s (baked, %lu events)\n", r.name, (unsigned long)n);
    }
    g_player.play(g_buf, n);
}

// --- codec / bring-up (verbatim proven path from spike_midi_dexed) ----------
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

tac5212::TAC5212 g_codec(Wire);
TDspProgrammingKit kit;
elapsedMillis hb;
static bool  g_codecOk = false;
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

// --- physical MIDI IN -> the same synth sink --------------------------------
static void onNoteOn (byte ch, byte note, byte vel) {
    if (vel == 0) g_synthSink->onNoteOff(ch, note, 0);
    else          g_synthSink->onNoteOn(ch, note, vel);
}
static void onNoteOff(byte ch, byte note, byte vel) { g_synthSink->onNoteOff(ch, note, vel); }
static void onPitchBend(byte ch, int bend) {          // bend: -8192..+8191
    g_synthSink->onPitchBend(ch, ((float)bend / 8192.0f) * 2.0f);   // range 2 semis
}
static void onControlChange(byte ch, byte cc, byte val) {
    if (cc == 1)  g_synthSink->onModWheel(ch, val / 127.0f);
    if (cc == 64) g_synthSink->onSustain(ch, val >= 64);
    if (cc == 120 || cc == 123) g_synthSink->onAllNotesOff(ch);
}

void setup() {
    hardResetCodecPower();
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 1500) {}

    Serial.println();
    if (CrashReport) { Serial.println("!!! CRASH REPORT !!!"); Serial.print(CrashReport); }
    Serial.printf("=== spike_midi_player  (synth = %s) ===\n", synthName());
    Serial.println("Synth-agnostic MIDI file player -> tdsp::MidiSink -> engine -> TAC5212.");

    kit.onFlashEnter([] { AudioNoInterrupts(); });
    Serial.println("[setup] kit.begin() -> boot ESP32 (frees I2C)..."); Serial.flush();
    kit.begin();
    delay(300);

    Serial.println("[setup] i2c bus recover..."); Serial.flush();
    i2cBusRecover();
    pinMode(18 /*SDA0*/, INPUT_PULLUP); delayMicroseconds(20);
    if (digitalRead(18) == LOW) {
        g_codecOk = false; g_codecMsg = "i2c wedged - skipped";
        Serial.println("[setup] !! I2C SDA STILL LOW -> SKIP codec init.");
    } else {
        Wire.begin();
        Wire.setClock(100000);
        tdspMuxAutoSelectCodec(TAC5212_I2C_ADDRESS);
        setupCodec();
        Serial.println("[setup] codec init done"); Serial.flush();
    }

#if TDSP_HAS_SDCARD
    g_sdReady = SD.begin(BUILTIN_SDCARD);
    Serial.printf("[sd] card %s\n", g_sdReady ? "ready" : "not present");
#endif

    AudioMemory(40);
    outL.gain(0, 0.62f); outR.gain(0, 0.62f);   // synth make-up (matches mix-kit level)
    if (g_codecOk) applyVol();

    // Player -> synth sink. Default channel mask skips drums (channel 10) since
    // the default backend is a single melodic engine. A drum-capable backend
    // would call g_player.setChannelMask(kMaskAll).
    g_player.setSink(g_synthSink);

    synthBegin();
    buildSongList();

    // Physical MIDI IN, omni.
    MIDI.begin(MIDI_CHANNEL_OMNI);
    MIDI.turnThruOff();
    MIDI.setHandleNoteOn(onNoteOn);
    MIDI.setHandleNoteOff(onNoteOff);
    MIDI.setHandlePitchBend(onPitchBend);
    MIDI.setHandleControlChange(onControlChange);

    Serial.println("running -- cmds: W=play/stop  S=next song  V=next instrument  n=test note");
    Serial.println("                 D=toggle drum channel  +/-=vol  i=re-init codec");
    Serial.println("      ESP32/kit:  r=reset  g=flash mode  @BOOTAPP@=exit flash  U=Teensy prog");

    Serial.println("[setup] settle 2.5s, late kit.bootApp()..."); Serial.flush();
    delay(2500);
    kit.bootApp();
}

void loop() {
    if (kit.service(Serial)) return;

    while (MIDI.read()) { /* live MIDI -> synth sink via handlers */ }
    g_player.tick();

    if (Serial.available()) {
        int c = Serial.read();
        if (!kit.handleChar(Serial, c)) {
            if (c == 'W') { if (g_player.isPlaying()) { g_player.stop(); Serial.println("[song] stopped"); }
                            else songPlay(g_songSel); }
            else if (c == 'S') { if (g_numSongs) g_songSel = (g_songSel + 1) % g_numSongs;
                                 Serial.printf("[song] selected %d: %s\n", g_songSel, g_songs[g_songSel].name); }
            else if (c == 'V') { synthNextInstrument(); }
            else if (c == 'n') { Serial.println("[cmd] test note C4 300ms");
                                 g_synthSink->onNoteOn(1, 60, 100); delay(300); g_synthSink->onNoteOff(1, 60, 0); }
            else if (c == 'D') { bool drums = g_player.channelMask() == tdsp::MidiFilePlayer::kMaskAll;
                                 g_player.setChannelMask(drums ? tdsp::MidiFilePlayer::kMaskNoDrums
                                                               : tdsp::MidiFilePlayer::kMaskAll);
                                 Serial.printf("[cmd] drum channel (10) %s\n", drums ? "OFF" : "ON"); }
            else if (c == '+') { g_dvol += 3.0f; if (g_dvol > 0) g_dvol = 0; applyVol();
                                 Serial.printf("[cmd] vol %.0f dB\n", g_dvol); }
            else if (c == '-') { g_dvol -= 3.0f; if (g_dvol < -60) g_dvol = -60; applyVol();
                                 Serial.printf("[cmd] vol %.0f dB\n", g_dvol); }
            else if (c == 'i') { Serial.println("[cmd] re-init codec"); setupCodec(); applyVol(); }
        }
    }

    if (hb >= 1000) {
        hb = 0;
        float po = peakOut.available() ? peakOut.read() : 0.0f;
        Serial.printf("alive up=%lus  synth=%s codec=%s(%s)  song=%s %lu/%lu  outPeak=%.3f  "
                      "cpuMax=%.1f%% memMax=%u\n",
                      (unsigned long)(millis() / 1000), synthName(),
                      g_codecOk ? "OK" : "FAIL", g_codecMsg,
                      g_player.isPlaying() ? "play" : "idle",
                      (unsigned long)g_player.eventIndex(), (unsigned long)g_player.eventCount(),
                      po, AudioProcessorUsageMax(), AudioMemoryUsageMax());
        AudioProcessorUsageMaxReset();
    }
}
