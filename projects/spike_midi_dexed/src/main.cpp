// spike_midi_dexed — minimal bring-up of the physical MIDI-IN port driving Dexed.
//
//   MIDI DIN/TRS --opto(H11L1)--> pin 0 (Serial1 RX, 31250)
//     --> Arduino MIDI Library --> AudioSynthDexed (6-op FM)
//     --> AudioOutputTDM (SAI1) --> TAC5212 DAC --> OUT1/OUT2 (HP jack).
//
// Everything BT/S-PDIF from the mix kit is stripped so the only moving parts
// while proving the MIDI port are: the port, Dexed, and the codec. The codec /
// TDM / ESP32-kit setup is copied verbatim from spike_esp32_bt_spdif_mix_kit
// (known-good on this board), so if audio is silent it's the new MIDI/Dexed
// code, not the plumbing.
//
// Bring-up path:
//   1. 'n' plays a local Dexed note (middle C) — proves audio+Dexed with NO MIDI.
//   2. 'R' toggles a raw hex dump of Serial1 — proves the wire sees ANY bytes.
//   3. Play a MIDI keyboard into the DIN — notes should sound + count in heartbeat.
//
// ESP32/kit:  r=reset->app  g=flash mode  @BOOTAPP@=exit flash  U=Teensy prog.

#include <Arduino.h>
#include <Wire.h>
#include "tdsp_hw_config.h"
#include <Audio.h>
#include <TAC5212.h>
#include <TDspProgrammingKit.h>
#include <MIDI.h>
#include <synth_dexed.h>
#include "DexedVoiceBank.h"
#include "william_tell_mid.h"    // generated note stream (scratchpad/midi2c.py)

constexpr int     TAC5212_EN_PIN      = 35;     // shared SHDNZ, active-low
constexpr uint8_t TAC5212_I2C_ADDRESS = 0x51;

// Physical MIDI IN on the schematic's MIDI_RX (Teensy pin 0 = Serial1 RX)
// through the H11L1 optoisolator. The library drives Serial1 at 31250 baud.
MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI);

// --- Audio graph ------------------------------------------------------------
// tdmClk (SAI1 TDM input) is constructed FIRST so it owns update_responsibility
// exactly as in the mix kit; tdmOut is the actual DAC feed. tdmClk is left
// unconnected — it exists only to keep the SAI1 clock/update wiring identical.
AudioInputTDM          tdmClk;               // SAI1 TDM clock + update driver
AudioOutputTDM         tdmOut;               // SAI1 TDM -> TAC5212 DAC (ch0=L, ch1=R)

AudioSynthDexed        g_dexed(16, AUDIO_SAMPLE_RATE_EXACT);  // 16-voice 6-op FM
AudioMixer4            outL, outR;           // slot 0 = Dexed (mono, fanned L+R)
AudioAnalyzePeak       peakOut;

AudioConnection c_dxL   (g_dexed, 0, outL, 0);
AudioConnection c_dxR   (g_dexed, 0, outR, 0);
AudioConnection c_outL  (outL,    0, tdmOut, 0);
AudioConnection c_outR  (outR,    0, tdmOut, 1);
AudioConnection c_pkOut (outL,    0, peakOut, 0);

tac5212::TAC5212 g_codec(Wire);

// ESP32 control/flash — reusable kit (EN=37, IO0=36, Serial7). Non-overlapping
// pins; kept for touch-free Teensy flashing + LED heartbeat + ESP32 boot.
TDspProgrammingKit kit;
elapsedMillis hb;

// --- Dexed / MIDI state -----------------------------------------------------
static int      g_dexedBank  = 0;
static int      g_dexedVoice = 0;
static bool     g_rawMidi    = false;   // 'R': dump Serial1 bytes as hex instead of parsing
static bool     g_autoPing   = false;   // 'O': auto-send a loopback note every 1.5s
static bool     g_blast      = false;   // 'C': stream 0xF8 clocks at full rate (DMM/scope target)
static uint32_t g_clockCount = 0;       // returned 0xF8 clocks (loopback confirm, no notes)
static uint32_t g_noteOnCount = 0;      // lifetime note-on tally (heartbeat)
static uint32_t g_midiByteCount = 0;    // raw-mode byte tally

static void hardResetCodecPower() {
    pinMode(TAC5212_EN_PIN, OUTPUT);
    digitalWrite(TAC5212_EN_PIN, LOW);  delay(5);
    digitalWrite(TAC5212_EN_PIN, HIGH); delay(10);
}

// I2C bus recovery: bit-bang SCL to free a stuck slave before Wire.begin(), so
// setup() can never hang. Wire0: SDA=18, SCL=19. (Verbatim from the mix kit.)
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

// Load the selected bank/voice into Dexed. Panics first for a clean switch.
// Runs from the main loop only (synth_dexed voice load is not audio-ISR safe).
static void applyDexedVoice(int bank, int voice) {
    if (bank < 0)                             bank  = tdsp::dexed::kNumBanks - 1;
    if (bank >= tdsp::dexed::kNumBanks)       bank  = 0;
    if (voice < 0)                            voice = tdsp::dexed::kVoicesPerBank - 1;
    if (voice >= tdsp::dexed::kVoicesPerBank) voice = 0;
    g_dexed.panic();
    if (tdsp::dexed::loadVoice(g_dexed, bank, voice)) {
        g_dexedBank  = bank;
        g_dexedVoice = voice;
        char name[tdsp::dexed::kVoiceNameBufBytes];
        tdsp::dexed::copyVoiceName(bank, voice, name, sizeof(name));
        Serial.printf("[dexed] bank %d (%s)  voice %d  \"%s\"\n",
                      bank, tdsp::dexed::bankName(bank), voice, name);
    }
}

// --- MIDI handlers (Serial1 DIN) --------------------------------------------
// Dexed is not MPE-aware: pitch bend / mod / sustain are global across voices.
// For a bring-up test that's fine.
static void onNoteOn(byte ch, byte note, byte vel) {
    if (vel == 0) { g_dexed.keyup(note); return; }   // running-status note-off
    g_dexed.keydown(note, vel);
    g_noteOnCount++;
    Serial.printf("[midi] noteOn  ch%-2d n%-3d v%-3d\n", ch, note, vel);
}
static void onNoteOff(byte ch, byte note, byte /*vel*/) {
    g_dexed.keyup(note);
    Serial.printf("[midi] noteOff ch%-2d n%-3d\n", ch, note);
}
static void onPitchBend(byte /*ch*/, int bend) {   // bend: -8192..+8191
    g_dexed.setPitchbendRange(2);
    g_dexed.setPitchbend((int16_t)bend);
}
static void onControlChange(byte /*ch*/, byte cc, byte val) {
    if (cc == 1)  g_dexed.setModWheel(val);              // mod wheel
    if (cc == 64) g_dexed.setSustain(val >= 64);         // sustain pedal
    if (cc == 123 && val == 0) g_dexed.panic();          // all notes off
}

// Timing-Clock (0xF8) received. Real-time message, so it fires no note handler
// and makes no sound — it just proves bytes completed the loopback. Used by the
// 'C' blast mode so the DMM/scope target and the software confirm agree.
static void onClock() { g_clockCount++; }

// Transmit a short test note OUT the MIDI port (pin 1 / MIDI OUT jack). With a
// TRS loopback cable to MIDI IN, it should come straight back in on pin 0,
// fire onNoteOn, and sound Dexed. Sent on channel 1.
static uint8_t g_pingNote = 60;
static void sendTestNoteOut() {
    Serial.printf("[cmd] TX MIDI noteOn ch1 n%d v100 out the port...\n", g_pingNote);
    MIDI.sendNoteOn(g_pingNote, 100, 1);
    // Keep servicing RX during the note so the looped-back note-on is processed
    // (Dexed keydown) NOW and sounds for the full hold, instead of getting queued
    // behind the note-off and collapsing to zero duration.
    uint32_t t0 = millis();
    while (millis() - t0 < 250) MIDI.read();
    MIDI.sendNoteOff(g_pingNote, 0, 1);
    if (++g_pingNote > 72) g_pingNote = 60;   // walk C4..C5 so each ping is audibly distinct
}

// --- Built-in demo: the real William Tell Overture MIDI file -----------------
// kWilliamTellSong (william_tell_mid.h) is the note event stream transcoded from
// 'William Tell Overture.mid' (scratchpad/midi2c.py): 16836 events, ~189s, up to
// 17-voice polyphony played straight through Dexed's 16 voices (minor stealing).
// Blocking sequencer, interruptible by any serial key.

// Interruptible delay: returns true (and consumes the byte) if a serial key is
// pressed, so a long/looping demo can be stopped mid-song.
static bool delayOrKey(uint16_t ms) {
    uint32_t t0 = millis();
    while (millis() - t0 < ms) { if (Serial.available()) { Serial.read(); return true; } }
    return false;
}
static void playWilliamTell(bool loop) {
    Serial.printf("[cmd] William Tell Overture (full MIDI, ~189s)%s\n",
                  loop ? "  [looping - press any key to stop]" : "  [press any key to stop]");
    const int n = sizeof(kWilliamTellSong) / sizeof(kWilliamTellSong[0]);
    do {
        for (int i = 0; i < n; ++i) {
            const SongEv &e = kWilliamTellSong[i];
            if (e.dms && delayOrKey(e.dms)) {          // wait until this event's time
                g_dexed.panic(); Serial.println("[cmd] ...stopped"); return;
            }
            if (e.vel)      g_dexed.keydown(e.note, e.vel);   // note-on
            else if (e.note) g_dexed.keyup(e.note);           // note-off (note 0 = padding rest)
        }
        g_dexed.panic();   // flush any hung voices between repeats
    } while (loop);
    Serial.println("[cmd] ...done (W=once  L=loop)");
}

void setup() {
    hardResetCodecPower();

    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 1500) {}

    Serial.println();
    Serial.println("=== spike_midi_dexed (MIDI-IN -> Dexed -> TAC5212) ===");
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

    // MIDI: consume all channels (omni). Note-off is handled inside onNoteOn
    // too (vel==0) for controllers that use running-status note-offs.
    MIDI.begin(MIDI_CHANNEL_OMNI);
    // Soft-thru OFF: with a physical OUT->IN loopback cable, the library would
    // otherwise re-echo every received byte back out and feed back forever.
    MIDI.turnThruOff();
    MIDI.setHandleNoteOn(onNoteOn);
    MIDI.setHandleNoteOff(onNoteOff);
    MIDI.setHandlePitchBend(onPitchBend);
    MIDI.setHandleControlChange(onControlChange);
    MIDI.setHandleClock(onClock);

    // Load an initial voice so the first note is obviously a real DX7 patch.
    applyDexedVoice(g_dexedBank, g_dexedVoice);
    g_dexed.setPitchbendRange(2);
    g_dexed.setPitchbend((int16_t)0);
    g_dexed.setModWheel(0);
    g_dexed.setSustain(false);

    Serial.println("running -- cmds: n=test note  W=William Tell  L=loop it  o=TX ping  O=auto-ping");
    Serial.println("                 C=0xF8 blast  R=raw hex  v/V=voice +/-  b=bank+");
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

    // MIDI input: raw hex-dump mode (wire-level debug) OR normal parsing.
    if (g_rawMidi) {
        while (Serial1.available()) {
            uint8_t b = (uint8_t)Serial1.read();
            g_midiByteCount++;
            Serial.printf("%02X ", b);
        }
    } else {
        while (MIDI.read()) { /* handlers fire per message */ }
    }

    if (Serial.available()) {
        int c = Serial.read();
        if (!kit.handleChar(Serial, c)) {     // g / r / U handled by the kit
            if (c == 'n') {                    // local test note — no MIDI needed
                Serial.println("[cmd] test note: middle C, 300ms");
                g_dexed.keydown(60, 100); delay(300); g_dexed.keyup(60);
            }
            else if (c == 'W') { playWilliamTell(false); }  // built-in demo, once
            else if (c == 'L') { playWilliamTell(true);  }  // ...looped (any key stops)
            else if (c == 'o') { sendTestNoteOut(); }   // one loopback ping
            else if (c == 'C') { g_blast = !g_blast;
                                 Serial.printf("[cmd] 0xF8 clock blast %s "
                                     "(measure pin1/OUT_5 with DMM Hz/AC; clocks= confirms RX)\n",
                                     g_blast ? "ON" : "OFF"); }
            else if (c == 'O') { g_autoPing = !g_autoPing;
                                 Serial.printf("[cmd] auto loopback ping %s\n",
                                               g_autoPing ? "ON (every 1.5s)" : "OFF"); }
            else if (c == 'R') { g_rawMidi = !g_rawMidi;
                                 Serial.printf("\n[cmd] raw MIDI hex dump %s "
                                     "(MIDI parsing %s)\n", g_rawMidi ? "ON" : "OFF",
                                     g_rawMidi ? "PAUSED" : "resumed"); }
            else if (c == 'v') { applyDexedVoice(g_dexedBank, g_dexedVoice + 1); }
            else if (c == 'V') { applyDexedVoice(g_dexedBank, g_dexedVoice - 1); }
            else if (c == 'b') { applyDexedVoice(g_dexedBank + 1, 0); }
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

    // Continuous 0xF8 blast: top up the Serial1 TX FIFO every loop (non-blocking,
    // bounded by availableForWrite) to sustain a full-rate 31250-baud stream. A
    // steady signal for a DMM (Hz/AC on pin 1 or MIDI_OUT_5) or scope; if the
    // loopback is good the clocks come back and bump g_clockCount.
    if (g_blast) {
        while (Serial1.availableForWrite() > 0) Serial1.write(0xF8);
    }

    // Auto loopback ping: fire a note out the port every 1.5s so a hands-free
    // OUT->IN loopback watch shows notes arriving without touching a keyboard.
    static elapsedMillis pingTimer;
    if (g_autoPing && pingTimer >= 1500) { pingTimer = 0; sendTestNoteOut(); }

    if (hb >= 1000) {
        hb = 0;
        float po = peakOut.available() ? peakOut.read() : 0.0f;
        Serial.printf("alive up=%lus  codec=%s(%s)  notes=%lu clocks=%lu rawBytes=%lu  "
                      "outPeak=%.3f  cpuMax=%.1f%% memMax=%u\n",
                      (unsigned long)(millis() / 1000),
                      g_codecOk ? "OK" : "FAIL", g_codecMsg,
                      (unsigned long)g_noteOnCount, (unsigned long)g_clockCount,
                      (unsigned long)g_midiByteCount,
                      po, AudioProcessorUsageMax(), AudioMemoryUsageMax());
    }
}
