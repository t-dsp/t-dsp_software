// T-DSP ESP32 Bluetooth Receiver + BLE Control
// ---------------------------------------------------------------------------
// Bluetooth Classic A2DP sink on the ESP32-DevKitC of the
// teensy41_digital_audio_board. Decodes A2DP audio and streams it out over
// I2S (ESP32 as I2S MASTER) into the Teensy 4.1's SAI2 slave input.
//
// NEW: a BLE GATT "control" service runs ALONGSIDE the A2DP audio so a phone
// app can command the receiver (enter pairing mode, disconnect, forget device)
// and read its status. A2DP is Bluetooth Classic and BLE is Bluetooth LE; both
// live on the same radio only if the controller comes up in DUAL mode (BTDM).
// The A2DP library defaults to CLASSIC-only and *releases* the BLE controller
// RAM at start(), permanently killing BLE -- so we MUST call
// set_default_bt_mode(ESP_BT_MODE_BTDM) BEFORE start(). Then BLEDevice::init()
// attaches to the already-running Bluedroid stack (it detects the controller is
// up and skips re-init), and A2DP + BLE coexist.
//
// I2S pin map is fixed by the board's #ESP32_I2S1 header:
//   ESP32 GPIO26 (BCK)  -> Teensy pin 4  (BCLK2)
//   ESP32 GPIO16 (WS)   -> Teensy pin 3  (LRCLK2)
//   ESP32 GPIO25 (DOUT) -> Teensy pin 5  (IN2)   <-- audio to the Teensy
//   ESP32 GPIO33 (DIN)  <- Teensy pin 2  (OUT2)  (unused here; future BT TX)
//   ESP32 GPIO0  (MCLK) <> Teensy pin 33 (MCLK2) (unused; Teensy slave needs no MCLK)
//
// A2DP is always 44.1 kHz / 16-bit stereo; the Teensy resamples to the 48 kHz
// F32 graph. Config mirrors the proven JayShoe/esp32_T4_bt_music_receiver.

#include <Arduino.h>

#include "BluetoothA2DPSink.h"

// BLE via the Arduino-ESP32 (Bluedroid-backed) BLE library. Bluedroid-backed
// matters: A2DP also uses Bluedroid, so a single stack serves both. (NimBLE is
// BLE-only and could NOT coexist with the classic A2DP profile.)
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// Classic-BT bond list (enumerate paired phones) + NVS name storage so the app
// can show a switchable list of saved sources with real names.
#include <esp_gap_bt_api.h>
#include <Preferences.h>

// Discoverable Bluetooth name your phone will pair with (classic A2DP) and the
// BLE device/advertising name the control app scans for.
static constexpr char BT_DEVICE_NAME[] = "T-DSP";

// ---- BLE control contract -------------------------------------------------
// Custom 128-bit UUIDs for the T-DSP control service. The app filters scans by
// TDSP_SVC_UUID so it finds this device by capability, not by a fuzzy name.
//   Service  7a9c0001-...  "T-DSP Control"
//   Command  7a9c0002-...  WRITE        (1 byte: an opcode below)
//   Status   7a9c0003-...  READ+NOTIFY  (small JSON string, see buildStatus())
#define TDSP_SVC_UUID  "7a9c0001-4a6e-4b7d-8f1a-2d3c4e5f6a70"
#define TDSP_CMD_UUID  "7a9c0002-4a6e-4b7d-8f1a-2d3c4e5f6a70"
#define TDSP_STAT_UUID "7a9c0003-4a6e-4b7d-8f1a-2d3c4e5f6a70"
// Sources list (READ+NOTIFY): JSON array of paired phones, e.g.
//   [{"a":"aabbccddeeff","n":"Pixel 7","c":1}]  (a=addr hex, n=name, c=connected)
// NOTIFY signals "list changed" -> the app RE-READS (the full list can exceed one
// notification's MTU, but a READ returns the whole value via ATT read-blob).
#define TDSP_SRC_UUID  "7a9c0004-4a6e-4b7d-8f1a-2d3c4e5f6a70"
// Device catalog (READ+NOTIFY): '|'-delimited name lists the Teensy streams over
// UART (@SONGS=/@INSTR=) so the app renders its Dexed pickers dynamically — no
// app rebuild when songs/instruments change. A list longer than one BLE value
// (512 B) is streamed as a burst of framed NOTIFY chunks ("<seq>\x1e<count>\x1e
// <payload>") that the app reassembles — see setCatalog(). A single-chunk list is
// just count=1. The app forces a fresh burst (CMD_REFRESH_CAT) after subscribing.
#define TDSP_SONGS_UUID "7a9c0005-4a6e-4b7d-8f1a-2d3c4e5f6a70"
#define TDSP_INSTR_UUID "7a9c0006-4a6e-4b7d-8f1a-2d3c4e5f6a70"
// Drum grooves catalog (READ+NOTIFY): the '|'-delimited groove names the Teensy
// scans off /drums (@DRUMS=), same chunked-burst contract as songs/instruments.
#define TDSP_DRUMS_UUID "7a9c0007-4a6e-4b7d-8f1a-2d3c4e5f6a70"
// Generic file transfer (READ+NOTIFY): the Teensy streams any SD file as base64
// frames (@FB/@FD/@FE/@FERR) plus the manifest registry (@MANIFESTS). We forward
// each line VERBATIM as one notification (each @FD fits a ~512 MTU) — NO reassembly
// on the ESP32; the app reassembles the file. See CATALOG_TRANSPORT.md. This ONE
// char is the transport for drums (catalog.tsv) and every future catalog type.
#define TDSP_FILE_UUID  "7a9c0008-4a6e-4b7d-8f1a-2d3c4e5f6a70"

// Command opcodes: the first byte of a write to the command characteristic.
enum : uint8_t {
  CMD_PAIRING_MODE = 0x01,  // become discoverable + connectable (accept a new phone)
  CMD_END_PAIRING  = 0x02,  // leave pairing mode (non-discoverable)
  CMD_DISCONNECT   = 0x03,  // drop the current A2DP source
  CMD_FORGET       = 0x04,  // forget the last paired device (clears NVS bond)
  CMD_RECONNECT    = 0x05,  // sink reconnects A2DP to the last paired phone
  CMD_SET_VOLUME   = 0x10,  // 2nd byte = master volume 0..100 (%); relayed to the Teensy
  CMD_CONNECT_ADDR = 0x11,  // + 6 bytes BD address: switch A2DP to that paired phone
  CMD_FORGET_ADDR  = 0x12,  // + 6 bytes BD address: remove that specific bond
  CMD_PLAY_SONG    = 0x20,  // play the built-in Dexed demo (William Tell); relayed to Teensy
  CMD_STOP_SONG    = 0x21,  // stop the Dexed demo
  CMD_SET_DX_VOICE = 0x22,  // 2nd byte = Dexed instrument index; relayed to the Teensy
  CMD_REFRESH_CAT  = 0x23,  // re-scan SD + refresh song/instrument catalog (@GETCAT to Teensy)
  CMD_SET_HPF      = 0x24,  // 2nd byte = TAC5212 DAC highpass mode 0..3; relayed to the Teensy
  CMD_SET_MIDI_MODE= 0x25,  // 2nd byte = 0 normal MIDI / 1 MPE; relayed to the Teensy
  CMD_SET_LOOP     = 0x26,  // 2nd byte = 0/1 loop the current song; relayed to the Teensy
  CMD_SET_PRESSURE = 0x27,  // 2nd byte = MPE pressure routing bitmask (1=vol 2=bright 4=vib 8=trem)
  CMD_SET_MODWHEEL = 0x28,  // 2nd byte = mod-wheel routing bitmask (2=bright 4=vib 8=trem)
  CMD_SET_LFOMODE  = 0x29,  // 2nd byte = 0 respect patch LFO / 1 force LFO on any patch
  CMD_SET_TIMBRE   = 0x2A,  // 2nd byte = CC74 timbre (MPE Y) routing bitmask (2=bright 4=vib 8=trem)
  CMD_SET_REPLAYGAIN=0x2B,  // 2nd byte = 0 off / 1 on; ReplayGain loudness normalization, relayed to Teensy
  CMD_PLAY_DRUM    = 0x30,  // + 1 byte: drum groove index; loops until stopped (relayed as @DRUM=<i>)
  CMD_STOP_DRUM    = 0x31,  // stop the drum groove (@DRUM=stop)
  CMD_SET_DRUM_KIT = 0x32,  // + 1 byte: GM drum-kit index ("instrument") (@DRUMKIT=<i>)
  CMD_SET_DRUM_SPEED=0x33,  // RESERVED — drum-speed control removed (drums follow master BPM). Kept so 0x33 isn't reused.
  CMD_SET_DRUM_VOL = 0x34,  // + 1 byte: drum level 0..150 (%) (@DRUMVOL=<pct>)
  CMD_SET_BPM      = 0x35,  // + 1 byte: master tempo 40..240 bpm — song+drum (@BPM=<n>)
  CMD_SET_DRUM_SYNCHRO=0x36,// + 1 byte: 0/1 synchro start (groove begins on first note) (@DRUMSYNCHRO=<0|1>)
  CMD_SET_ARP_ON   = 0x37,  // + 1 byte: 0/1 arpeggiator enable (@ARPON=<0|1>)
  CMD_SET_ARP_PATTERN=0x38, // + 1 byte: pattern index 0..24 (@ARPPAT=<n>)
  CMD_SET_ARP_RATE = 0x39,  // + 1 byte: rate index 0..14 (@ARPRATE=<n>)
  CMD_SET_ARP_OCT  = 0x3a,  // + 1 byte: octave range 1..4 (@ARPOCT=<n>)
  CMD_SET_ARP_LATCH= 0x3b,  // + 1 byte: 0/1 latch held notes (@ARPLATCH=<0|1>)
  CMD_READ_FILE    = 0x40,  // + N bytes: SD path string; Teensy streams it back on the FILE char (@READ=<path>)
  CMD_PLAY_DRUM_FILE=0x41,  // + N bytes: groove filename; plays /drums/<name> (@DRUMF=<filename>)
  CMD_RELAY_LINE   = 0x42,  // + N bytes: a literal control line (e.g. "@DXPICK=<cart>\t<v>", "@REINDEX",
                            //   "@SONG=<i>") relayed VERBATIM to the Teensy. The generic seam the new
                            //   catalog app uses so BLE speaks the same @-protocol as Web Serial.
};

BluetoothA2DPSink a2dp_sink;

// BLE state shared between the GATT callbacks (BT task context) and the app-
// visible status. Kept tiny and updated from one place (pushStatus()).
static BLECharacteristic *g_statChar = nullptr;
static volatile bool      g_bleClientConnected = false;
static bool               g_discoverable = false;  // we track this; the A2DP lib has no getter
static uint8_t            g_volume = 50;            // master volume 0..100 (%), last set from the app
static uint8_t            g_hpf    = 0;             // TAC5212 DAC highpass mode 0..3, last set from the app
static uint8_t            g_midiMode = 0;           // 0 = normal MIDI, 1 = MPE; last set from the app
static uint8_t            g_replayGain = 1;          // 0 = off, 1 = on; ReplayGain normalization (Teensy default on)

// Relay the master volume to the Teensy over UART0 as a framed line "@VOL=<n>".
// The Teensy owns the TAC5212 codec; it parses this line and calls setDvol() on
// the headphone output. Distinct "@VOL=" prefix so it never collides with logs.
static void relayVolume() {
  Serial.printf("@VOL=%u\n", g_volume);
}

// Relay Dexed controls to the Teensy over UART0, same "@VERB=<val>" framing.
// The Teensy plays/stops the built-in song sequencer (by song index) and
// switches the Dexed instrument from a curated list (indices must match the app).
static void relaySong(uint8_t idx)    { Serial.printf("@SONG=%u\n", idx); }
static void relaySongStop()           { Serial.printf("@SONG=stop\n"); }
static void relayDxVoice(uint8_t idx) { Serial.printf("@DXVOICE=%u\n", idx); }

// Relay the TAC5212 DAC highpass mode (0=off, 1=1Hz, 2=12Hz, 3=96Hz) to the
// Teensy, which owns the codec and calls g_codec.setDacHpf().
static void relayHpf(uint8_t mode)    { Serial.printf("@HPF=%u\n", mode); }
static void relayMidiMode(uint8_t m)  { Serial.printf("@MIDIMODE=%u\n", m); }
static void relayReplayGain(uint8_t on){ Serial.printf("@RG=%u\n", on ? 1 : 0); }
static void relayLoop(uint8_t on)     { Serial.printf("@LOOP=%u\n", on ? 1 : 0); }
static void relayPressure(uint8_t m)  { Serial.printf("@PRESSURE=%u\n", m); }
static void relayModWheel(uint8_t m)  { Serial.printf("@MODWHEEL=%u\n", m); }
static void relayLfoMode(uint8_t f)   { Serial.printf("@LFOMODE=%u\n", f ? 1 : 0); }
static void relayTimbre(uint8_t m)    { Serial.printf("@TIMBRE=%u\n", m); }
// Drum-groove controls: play/stop a looping channel-10 groove and set its GM kit,
// speed and level. The Teensy owns playback (dedicated looping player) + the SD
// groove list; these just frame the text commands over UART.
static void relayDrum(uint8_t idx)    { Serial.printf("@DRUM=%u\n", idx); }
static void relayDrumStop()           { Serial.printf("@DRUM=stop\n"); }
static void relayDrumKit(uint8_t idx) { Serial.printf("@DRUMKIT=%u\n", idx); }
static void relayDrumVol(uint8_t p)   { Serial.printf("@DRUMVOL=%u\n", p); }
static void relayBpm(uint8_t b)       { Serial.printf("@BPM=%u\n", b); }        // master tempo (song+drum)
static void relayDrumSynchro(uint8_t s){ Serial.printf("@DRUMSYNCHRO=%u\n", s ? 1 : 0); }
static void relayArpOn(uint8_t s)      { Serial.printf("@ARPON=%u\n", s ? 1 : 0); }
static void relayArpPattern(uint8_t p) { Serial.printf("@ARPPAT=%u\n", p); }
static void relayArpRate(uint8_t r)    { Serial.printf("@ARPRATE=%u\n", r); }
static void relayArpOct(uint8_t o)     { Serial.printf("@ARPOCT=%u\n", o); }
static void relayArpLatch(uint8_t s)   { Serial.printf("@ARPLATCH=%u\n", s ? 1 : 0); }

// ---- Paired-source list (multi-device switch) -----------------------------
static BLECharacteristic *g_srcChar = nullptr;

// ---- Device catalog (Dexed songs + instruments, streamed from the Teensy) --
static BLECharacteristic *g_songsChar = nullptr;
static BLECharacteristic *g_instrChar = nullptr;
static BLECharacteristic *g_drumsChar = nullptr;
static BLECharacteristic *g_fileChar  = nullptr;   // generic file-transfer frames (pass-through)

// Ask the Teensy to stream an SD file / play a groove by name. Variable-length
// string args (path / filename) — the generic catalog transport (CATALOG_TRANSPORT.md).
static void relayReadFile(const char *path)  { Serial.printf("@READ=%s\n", path); }
static void relayDrumFile(const char *fname) { Serial.printf("@DRUMF=%s\n", fname); }
// Relay an arbitrary control line verbatim (the generic new-catalog seam). The app
// sends the same @-lines it would over Web Serial; we just add the newline.
static void relayLine(const char *line)      { Serial.print(line); Serial.print('\n'); }

// Forward ONE Teensy line verbatim to the app as a single notification. Used for the
// file-transfer stream: the Teensy already chunked each @FD to fit one MTU, so there
// is nothing to reassemble here — just relay + pace so the BLE stack doesn't drop it.
static void notifyRaw(BLECharacteristic *ch, const char *line) {
  if (!ch) return;
  ch->setValue((uint8_t *)line, strlen(line));
  if (g_bleClientConnected) { ch->notify(); delay(12); }
}

// Store a '|'-delimited name list into a catalog characteristic and notify.
// A BLE characteristic value caps at 512 B, so a long list (e.g. Dexed's full
// 320-voice set, ~7 KB) can't be served in one value. We stream it as a burst of
// framed notifications, each "<seq>\x1e<count>\x1e<payload>" (0x1e = record sep,
// never appears in names). The app reassembles the chunks in order. Notifications
// are unacknowledged, so we pace them so the BLE stack doesn't drop chunks.
static const size_t kCatChunk = 400;   // payload bytes/chunk; safe under a 512 MTU

static void setCatalog(BLECharacteristic *ch, const char *list) {
  if (!ch) return;
  size_t len   = strlen(list);
  size_t count = len ? (len + kCatChunk - 1) / kCatChunk : 1;   // >=1 (empty = one empty chunk)
  for (size_t seq = 0; seq < count; ++seq) {
    size_t off = seq * kCatChunk;
    size_t n   = (len - off < kCatChunk) ? (len - off) : kCatChunk;
    char   frame[24 + kCatChunk];
    int    h = snprintf(frame, sizeof(frame), "%u\x1e%u\x1e", (unsigned)seq, (unsigned)count);
    memcpy(frame + h, list + off, n);
    ch->setValue((uint8_t *)frame, h + n);
    if (g_bleClientConnected) { ch->notify(); delay(25); }   // pace so chunks aren't dropped
  }
}

// A complete '@'-framed line arrived from the Teensy over UART. The Teensy sends
// the catalog on our @GETCAT request; store each list into its characteristic.
static void handleTeensyLine(const char *line) {
  if      (strncmp(line, "@SONGS=", 7) == 0) { setCatalog(g_songsChar, line + 7); Serial.println("[cat] songs updated"); }
  else if (strncmp(line, "@INSTR=", 7) == 0) { setCatalog(g_instrChar, line + 7); Serial.println("[cat] instruments updated"); }
  else if (strncmp(line, "@DRUMS=", 7) == 0) { setCatalog(g_drumsChar, line + 7); Serial.println("[cat] drums updated"); }
  // Generic file transport: the manifest registry + file-read frames go straight to
  // the FILE characteristic, one line per notification (each already fits one MTU).
  else if (strncmp(line, "@MANIFESTS=", 11) == 0) { notifyRaw(g_fileChar, line); }
  else if (strncmp(line, "@FB=", 4) == 0 || strncmp(line, "@FD=", 4) == 0 ||
           strncmp(line, "@FE=", 4) == 0 || strncmp(line, "@FERR=", 6) == 0) { notifyRaw(g_fileChar, line); }
  // @REINDEXED marks the end of a catalog rebuild — relay it so the app's reindex()
  // (which sent @REINDEX via CMD_RELAY_LINE) knows the /tdsp DB is ready to re-read.
  else if (strncmp(line, "@REINDEXED", 10) == 0) { notifyRaw(g_fileChar, line); }
}

// Ask the Teensy to (re)send its catalog over UART.
static void requestCatalog() { Serial.print("@GETCAT\n"); }
static Preferences        g_names;                 // NVS: BD-address(hex) -> friendly name
static esp_bd_addr_t      g_pendingConnect;        // switch target after a willful disconnect
static bool               g_hasPendingConnect = false;

// 12-char lowercase hex of a BD address; doubles as the NVS key for its name.
static void addrHex(const uint8_t *bda, char *out /*>=13*/) {
  snprintf(out, 13, "%02x%02x%02x%02x%02x%02x",
           bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

// Remember a phone's friendly name (captured on connect) keyed by its address.
static void storeName(const uint8_t *bda, const char *name) {
  if (!name || !*name) return;
  char key[13];
  addrHex(bda, key);
  g_names.begin("btnames", false);
  g_names.putString(key, name);
  g_names.end();
}

// Build the JSON sources array: every bonded phone, its stored name, and whether
// it is the currently connected one.
static void buildSources(char *buf, size_t n) {
  int num = esp_bt_gap_get_bond_device_num();
  if (num < 0) num = 0;
  if (num > 8) num = 8;                     // cap for the fixed buffer
  esp_bd_addr_t list[8];
  int got = 0;
  if (num > 0) {
    int cnt = num;
    if (esp_bt_gap_get_bond_device_list(&cnt, list) == ESP_OK) got = cnt;
  }
  const esp_bd_addr_t *cur =
      a2dp_sink.is_connected() ? a2dp_sink.get_current_peer_address() : nullptr;
  g_names.begin("btnames", true);
  size_t o = 0;
  o += snprintf(buf + o, n - o, "[");
  for (int i = 0; i < got && o < n - 80; ++i) {
    char key[13];
    addrHex(list[i], key);
    String nm = g_names.getString(key, "");
    bool isCur = cur && memcmp(cur, list[i], sizeof(esp_bd_addr_t)) == 0;
    o += snprintf(buf + o, n - o, "%s{\"a\":\"%s\",\"n\":\"%s\",\"c\":%d}",
                  i ? "," : "", key, nm.length() ? nm.c_str() : key, isCur ? 1 : 0);
  }
  snprintf(buf + o, n - o, "]");
  g_names.end();
}

// Refresh the sources characteristic and notify (the app re-reads the full value).
static void pushSources() {
  if (!g_srcChar) return;
  static char buf[640];
  buildSources(buf, sizeof(buf));
  g_srcChar->setValue((uint8_t *)buf, strlen(buf));
  if (g_bleClientConnected) g_srcChar->notify();
  Serial.printf("[ble] sources %s\n", buf);
}

// A2DP peer name arrived (AVRCP) -> remember it for the connected address.
static void onPeerName(char *name) {
  const esp_bd_addr_t *a = a2dp_sink.get_current_peer_address();
  if (a) storeName(*a, name);
  pushSources();
}

// Build the status JSON into `buf`. Small on purpose so it fits a modest BLE
// MTU; the app can also READ the characteristic (supports long reads) for the
// full value if a notification is truncated on the default 23-byte MTU.
static void buildStatus(char *buf, size_t n) {
  bool connected = a2dp_sink.is_connected();
  const char *peer = connected ? a2dp_sink.get_peer_name() : "";
  if (!peer) peer = "";
  // conn: A2DP source connected?  disc: discoverable (pairing)?  vol: master 0..100
  // hpf: TAC5212 DAC highpass mode 0..3 (0=off)  mpe: 0 normal MIDI / 1 MPE
  // rg: ReplayGain 0 off / 1 on.
  snprintf(buf, n, "{\"conn\":%d,\"disc\":%d,\"vol\":%u,\"hpf\":%u,\"mpe\":%u,\"rg\":%u,\"peer\":\"%s\"}",
           connected ? 1 : 0, g_discoverable ? 1 : 0, g_volume, g_hpf, g_midiMode, g_replayGain, peer);
}

// Refresh the status characteristic value and notify any subscribed client.
static void pushStatus() {
  if (!g_statChar) return;
  char buf[96];
  buildStatus(buf, sizeof(buf));
  g_statChar->setValue((uint8_t *)buf, strlen(buf));
  if (g_bleClientConnected) g_statChar->notify();
  Serial.printf("[ble] status %s\n", buf);
}

// Force the sink back into pairing mode: connectable + generally discoverable.
// Called at boot and on every A2DP disconnect so a phone can ALWAYS (re)connect
// after a drop/failure without needing the BLE app or a power cycle.
static void enterPairingMode(const char *why) {
  a2dp_sink.set_connectable(true);
  a2dp_sink.set_discoverability(ESP_BT_GENERAL_DISCOVERABLE);
  g_discoverable = true;
  Serial.printf("[a2dp] pairing mode ON (%s) -- discoverable as \"%s\"\n",
                why, BT_DEVICE_NAME);
}

// A2DP connection state changed (called by the A2DP lib, BT task context).
// Persistent-pairing policy: whenever NOTHING is connected we stay discoverable
// so the phone can reconnect after any failure; while a source is connected we
// go non-discoverable (a sink serves one source, no reason to advertise). Also
// mirror the state out to the BLE status characteristic for the control app.
static void onA2dpConnState(esp_a2d_connection_state_t state, void *) {
  Serial.printf("[a2dp] conn state -> %s\n", a2dp_sink.to_str(state));
  if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
    a2dp_sink.set_discoverability(ESP_BT_NON_DISCOVERABLE);
    g_discoverable = false;
    const esp_bd_addr_t *a = a2dp_sink.get_current_peer_address();
    if (a) storeName(*a, a2dp_sink.get_peer_name());  // remember this phone by name
  } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
    if (g_hasPendingConnect) {
      // Deliberate source switch: the old link just closed -> open the new one.
      g_hasPendingConnect = false;
      a2dp_sink.connect_to(g_pendingConnect);
    } else {
      // dropped or failed -> immediately reopen for (re)pairing
      enterPairingMode("a2dp disconnected");
    }
  }
  pushStatus();
  pushSources();
}

// ---- BLE GATT callbacks ---------------------------------------------------
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *) override {
    g_bleClientConnected = true;
    Serial.println("[ble] control app connected");
    pushStatus();
    pushSources();
    requestCatalog();   // pull fresh song/instrument lists from the Teensy
  }
  void onDisconnect(BLEServer *) override {
    g_bleClientConnected = false;
    Serial.println("[ble] control app disconnected -> re-advertising");
    BLEDevice::startAdvertising();  // allow the app to reconnect
  }
};

class CommandCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *ch) override {
    std::string v = ch->getValue();
    if (v.empty()) return;
    uint8_t op = (uint8_t)v[0];
    switch (op) {
      case CMD_PAIRING_MODE:
        Serial.println("[ble] cmd: ENTER pairing mode");
        enterPairingMode("ble command");
        break;
      case CMD_END_PAIRING:
        Serial.println("[ble] cmd: END pairing mode");
        a2dp_sink.set_discoverability(ESP_BT_NON_DISCOVERABLE);
        g_discoverable = false;
        break;
      case CMD_DISCONNECT:
        Serial.println("[ble] cmd: DISCONNECT source");
        a2dp_sink.disconnect();
        break;
      case CMD_FORGET:
        Serial.println("[ble] cmd: FORGET last device");
        a2dp_sink.clean_last_connection();
        break;
      case CMD_RECONNECT: {
        bool ok = a2dp_sink.reconnect();  // sink -> last paired phone (needs a stored bond)
        Serial.printf("[ble] cmd: RECONNECT A2DP -> %s\n",
                      ok ? "attempting" : "no last device (pair first)");
        break;
      }
      case CMD_SET_VOLUME:
        if (v.size() >= 2) {
          g_volume = (uint8_t)v[1];
          if (g_volume > 100) g_volume = 100;
          Serial.printf("[ble] cmd: SET VOLUME %u%%\n", g_volume);
          relayVolume();  // -> Teensy -> TAC5212 setDvol()
        }
        break;
      case CMD_CONNECT_ADDR:
        if (v.size() >= 7) {
          esp_bd_addr_t addr;
          for (int i = 0; i < 6; ++i) addr[i] = (uint8_t)v[1 + i];
          Serial.printf("[ble] cmd: CONNECT %02x%02x%02x%02x%02x%02x\n",
                        addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
          if (a2dp_sink.is_connected()) {
            // Switch source: willful disconnect (suppresses auto-reconnect), then
            // connect the target once the old link closes (see onA2dpConnState).
            memcpy(g_pendingConnect, addr, sizeof(esp_bd_addr_t));
            g_hasPendingConnect = true;
            a2dp_sink.disconnect();
          } else {
            a2dp_sink.connect_to(addr);
          }
        }
        break;
      case CMD_FORGET_ADDR:
        if (v.size() >= 7) {
          esp_bd_addr_t addr;
          for (int i = 0; i < 6; ++i) addr[i] = (uint8_t)v[1 + i];
          char key[13];
          addrHex(addr, key);
          esp_bt_gap_remove_bond_device(addr);
          g_names.begin("btnames", false);
          g_names.remove(key);
          g_names.end();
          Serial.printf("[ble] cmd: FORGET %s\n", key);
        }
        break;
      case CMD_PLAY_SONG: {
        uint8_t song = (v.size() >= 2) ? (uint8_t)v[1] : 0;   // opcode-only = song 0
        Serial.printf("[ble] cmd: PLAY SONG %u\n", song);
        relaySong(song);   // -> Teensy: @SONG=<index>
        break;
      }
      case CMD_STOP_SONG:
        Serial.println("[ble] cmd: STOP SONG");
        relaySongStop();   // -> Teensy: @SONG=stop
        break;
      case CMD_SET_DX_VOICE:
        if (v.size() >= 2) {
          Serial.printf("[ble] cmd: SET DEXED VOICE %u\n", (uint8_t)v[1]);
          relayDxVoice((uint8_t)v[1]);  // -> Teensy: @DXVOICE=<n>
        }
        break;
      case CMD_REFRESH_CAT:
        Serial.println("[ble] cmd: REFRESH CATALOG");
        requestCatalog();   // -> Teensy re-scans SD + re-sends @SONGS/@INSTR
        break;
      case CMD_SET_HPF:
        if (v.size() >= 2) {
          uint8_t mode = (uint8_t)v[1];
          if (mode > 3) mode = 0;
          g_hpf = mode;     // remember for the status readback (pushStatus below)
          Serial.printf("[ble] cmd: SET HPF %u\n", mode);
          relayHpf(mode);   // -> Teensy: @HPF=<mode>
        }
        break;
      case CMD_SET_MIDI_MODE:
        if (v.size() >= 2) {
          g_midiMode = v[1] ? 1 : 0;
          Serial.printf("[ble] cmd: SET MIDI MODE %s\n", g_midiMode ? "MPE" : "MIDI");
          relayMidiMode(g_midiMode);   // -> Teensy: @MIDIMODE=<0|1>
        }
        break;
      case CMD_SET_REPLAYGAIN:
        if (v.size() >= 2) {
          g_replayGain = v[1] ? 1 : 0;
          Serial.printf("[ble] cmd: SET REPLAYGAIN %s\n", g_replayGain ? "on" : "off");
          relayReplayGain(g_replayGain);   // -> Teensy: @RG=<0|1>
          pushStatus();                    // mirror new state back to the app
        }
        break;
      case CMD_SET_LOOP:
        if (v.size() >= 2) {
          Serial.printf("[ble] cmd: SET LOOP %s\n", v[1] ? "ON" : "off");
          relayLoop(v[1]);   // -> Teensy: @LOOP=<0|1>
        }
        break;
      case CMD_SET_PRESSURE:
        if (v.size() >= 2) {
          Serial.printf("[ble] cmd: SET PRESSURE mask=%u\n", v[1]);
          relayPressure(v[1]);   // -> Teensy: @PRESSURE=<mask>
        }
        break;
      case CMD_SET_MODWHEEL:
        if (v.size() >= 2) {
          Serial.printf("[ble] cmd: SET MODWHEEL mask=%u\n", v[1]);
          relayModWheel(v[1]);   // -> Teensy: @MODWHEEL=<mask>
        }
        break;
      case CMD_SET_LFOMODE:
        if (v.size() >= 2) {
          Serial.printf("[ble] cmd: SET LFOMODE %s\n", v[1] ? "force" : "respect");
          relayLfoMode(v[1]);    // -> Teensy: @LFOMODE=<0|1>
        }
        break;
      case CMD_SET_TIMBRE:
        if (v.size() >= 2) {
          Serial.printf("[ble] cmd: SET TIMBRE mask=%u\n", v[1]);
          relayTimbre(v[1]);     // -> Teensy: @TIMBRE=<mask>
        }
        break;
      case CMD_PLAY_DRUM: {
        uint8_t groove = (v.size() >= 2) ? (uint8_t)v[1] : 0;
        Serial.printf("[ble] cmd: PLAY DRUM %u\n", groove);
        relayDrum(groove);       // -> Teensy: @DRUM=<index>
        break;
      }
      case CMD_STOP_DRUM:
        Serial.println("[ble] cmd: STOP DRUM");
        relayDrumStop();         // -> Teensy: @DRUM=stop
        break;
      case CMD_READ_FILE: {
        std::string path(v.begin() + 1, v.end());   // opcode byte + path string
        Serial.printf("[ble] cmd: READ FILE %s\n", path.c_str());
        relayReadFile(path.c_str());   // -> Teensy: @READ=<path>; frames come back on FILE char
        break;
      }
      case CMD_PLAY_DRUM_FILE: {
        std::string fname(v.begin() + 1, v.end());
        Serial.printf("[ble] cmd: PLAY DRUM FILE %s\n", fname.c_str());
        relayDrumFile(fname.c_str());  // -> Teensy: @DRUMF=<filename>
        break;
      }
      case CMD_RELAY_LINE: {
        std::string ln(v.begin() + 1, v.end());   // opcode byte + literal control line
        Serial.printf("[ble] cmd: RELAY %s\n", ln.c_str());
        relayLine(ln.c_str());         // -> Teensy: <line>\n  (verbatim @-protocol)
        break;
      }
      case CMD_SET_DRUM_KIT:
        if (v.size() >= 2) {
          Serial.printf("[ble] cmd: SET DRUM KIT %u\n", (uint8_t)v[1]);
          relayDrumKit((uint8_t)v[1]);   // -> Teensy: @DRUMKIT=<index>
        }
        break;
      case CMD_SET_DRUM_VOL:
        if (v.size() >= 2) {
          Serial.printf("[ble] cmd: SET DRUM VOL %u%%\n", (uint8_t)v[1]);
          relayDrumVol((uint8_t)v[1]);   // -> Teensy: @DRUMVOL=<pct>
        }
        break;
      case CMD_SET_BPM:
        if (v.size() >= 2) {
          Serial.printf("[ble] cmd: SET BPM %u\n", (uint8_t)v[1]);
          relayBpm((uint8_t)v[1]);        // -> Teensy: @BPM=<n>
        }
        break;
      case CMD_SET_DRUM_SYNCHRO:
        if (v.size() >= 2) {
          Serial.printf("[ble] cmd: SET DRUM SYNCHRO %u\n", (uint8_t)v[1]);
          relayDrumSynchro((uint8_t)v[1]); // -> Teensy: @DRUMSYNCHRO=<0|1>
        }
        break;
      case CMD_SET_ARP_ON:
        if (v.size() >= 2) { Serial.printf("[ble] cmd: SET ARP ON %u\n", (uint8_t)v[1]); relayArpOn((uint8_t)v[1]); }
        break;
      case CMD_SET_ARP_PATTERN:
        if (v.size() >= 2) { Serial.printf("[ble] cmd: SET ARP PATTERN %u\n", (uint8_t)v[1]); relayArpPattern((uint8_t)v[1]); }
        break;
      case CMD_SET_ARP_RATE:
        if (v.size() >= 2) { Serial.printf("[ble] cmd: SET ARP RATE %u\n", (uint8_t)v[1]); relayArpRate((uint8_t)v[1]); }
        break;
      case CMD_SET_ARP_OCT:
        if (v.size() >= 2) { Serial.printf("[ble] cmd: SET ARP OCT %u\n", (uint8_t)v[1]); relayArpOct((uint8_t)v[1]); }
        break;
      case CMD_SET_ARP_LATCH:
        if (v.size() >= 2) { Serial.printf("[ble] cmd: SET ARP LATCH %u\n", (uint8_t)v[1]); relayArpLatch((uint8_t)v[1]); }
        break;
      default:
        Serial.printf("[ble] cmd: unknown opcode 0x%02X\n", op);
        break;
    }
    pushStatus();
    pushSources();
  }
};

static void setupBle() {
  // Attaches to the Bluedroid stack A2DP already brought up in BTDM mode.
  BLEDevice::init(BT_DEVICE_NAME);

  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  BLEService *svc = server->createService(TDSP_SVC_UUID);

  BLECharacteristic *cmd = svc->createCharacteristic(
      TDSP_CMD_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  cmd->setCallbacks(new CommandCallbacks());

  g_statChar = svc->createCharacteristic(
      TDSP_STAT_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  g_statChar->addDescriptor(new BLE2902());  // CCCD so the app can subscribe

  // Sources list: READ returns the full JSON (ATT read-blob), NOTIFY signals the
  // app to re-read when the paired-device set or connection changes.
  g_srcChar = svc->createCharacteristic(
      TDSP_SRC_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  g_srcChar->addDescriptor(new BLE2902());

  // Catalog: song + instrument name lists (streamed from the Teensy). READ +
  // NOTIFY, same "re-read on change" contract as sources.
  g_songsChar = svc->createCharacteristic(
      TDSP_SONGS_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  g_songsChar->addDescriptor(new BLE2902());
  g_instrChar = svc->createCharacteristic(
      TDSP_INSTR_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  g_instrChar->addDescriptor(new BLE2902());
  g_drumsChar = svc->createCharacteristic(
      TDSP_DRUMS_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  g_drumsChar->addDescriptor(new BLE2902());
  // Generic file-transfer stream (@FB/@FD/@FE/@FERR + @MANIFESTS), forwarded verbatim.
  g_fileChar = svc->createCharacteristic(
      TDSP_FILE_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  g_fileChar->addDescriptor(new BLE2902());

  svc->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(TDSP_SVC_UUID);   // app scans/filters by this
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);           // iOS-friendly advertising intervals
  adv->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  pushStatus();
  Serial.println("[ble] control service up + advertising");
}

// Onboard LED (GPIO2 on the ESP32-DevKitC). Blinked as a heartbeat in loop() so
// you can SEE the app is actually running -- independent of the serial mirror
// (which is unreliable when the ESP32's own USB/CP210x is unpowered) and of BT.
static constexpr int LED_PIN = 2;

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  Serial.println("T-DSP ESP32: A2DP sink + BLE control, starting...");

  // CRITICAL: dual mode so BLE survives A2DP start() (see file header).
  a2dp_sink.set_default_bt_mode(ESP_BT_MODE_BTDM);

  // On boot, actively re-establish the last A2DP connection so a reboot
  // reconnects with NO manual step. (The stale-bond "connecting... stop" is
  // avoided because WE initiate the reconnect instead of waiting for the phone.)
  // The bond can still be wiped on demand -- the 'f' serial command and the BLE
  // FORGET opcode both call clean_last_connection(), after which there is no
  // device to auto-reconnect to and the sink is freshly pairable.
  a2dp_sink.set_auto_reconnect(true);

  // Mirror A2DP connect/disconnect out to the BLE status characteristic.
  a2dp_sink.set_on_connection_state_changed(onA2dpConnState);
  // Capture each phone's friendly name on connect, for the paired-sources list.
  a2dp_sink.set_peer_name_callback(onPeerName);

  // I2S peripheral: ESP32 is master and transmits the decoded audio.
  // APLL on for accurate 44.1 kHz; 8x128-byte DMA buffers.
  static const i2s_config_t i2s_config = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
      .sample_rate = 44100,
      .bits_per_sample = (i2s_bits_per_sample_t)16,
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
      .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_STAND_I2S),
      .intr_alloc_flags = 0,
      .dma_buf_count = 8,
      .dma_buf_len = 128,
      .use_apll = true,
      .tx_desc_auto_clear = true,
      .fixed_mclk = 0,
  };
  a2dp_sink.set_i2s_config(i2s_config);

  // Board I2S pin map (see header above). MCLK left unconnected: the Teensy
  // runs as an I2S slave and does not need a master clock.
  static const i2s_pin_config_t pin_config = {
      .mck_io_num = I2S_PIN_NO_CHANGE,
      .bck_io_num = 26,   // BCK  -> Teensy BCLK2 (pin 4)
      .ws_io_num = 16,    // WS   -> Teensy LRCLK2 (pin 3)
      .data_out_num = 25, // DOUT -> Teensy IN2 (pin 5)
      .data_in_num = I2S_PIN_NO_CHANGE,
  };
  a2dp_sink.set_pin_config(pin_config);

  a2dp_sink.start(BT_DEVICE_NAME);
  // Boot straight into pairing mode so a phone can connect immediately, and stay
  // there any time nothing is connected (see onA2dpConnState).
  enterPairingMode("boot");
  Serial.printf("A2DP sink up. Pair with \"%s\".\n", BT_DEVICE_NAME);

  // BLE control comes up AFTER A2DP so it attaches to the running stack.
  setupBle();
  requestCatalog();   // cache the Teensy's song/instrument lists early (also re-fetched on BLE connect)
  Serial.println("Ready: streaming audio + BLE control both live.");
}

void loop() {
  // Host command interface over UART0. In the bridge-only setup the ESP32's own
  // USB is unplugged, so the Teensy forwards a byte over Serial7 (mix firmware:
  // 'P'->'p', 'F'->'f') letting the host drive pairing ON COMMAND without the BLE
  // app. Also works from the ESP32 USB directly when connected.
  //   p = enter pairing mode (discoverable + connectable)
  //   f = forget last device (clear NVS bond) THEN enter pairing mode  <- clean re-pair
  //   x = disconnect the current A2DP source
  //   s = print status
  // Heartbeat: blink the onboard LED ~2 Hz so it's visually obvious the app is
  // running. If this LED is NOT blinking, the ESP32 is stuck (download mode / reset),
  // not running the app -- the definitive "is it alive" indicator.
  static uint32_t lastBlink = 0;
  if (millis() - lastBlink >= 250) {
    lastBlink = millis();
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  }

  // Two framings share this UART: bare single-char commands (p/f/x/s from the
  // Teensy's P/F relay + pairing) and '@'-framed lines (e.g. @SONGS=/@INSTR=
  // catalog). A '@' starts line mode until '\n'; other bytes are single commands.
  // Big enough for the whole catalog line: Dexed's full 320-voice @INSTR list is
  // ~7 KB. The value is then chunked out over BLE by setCatalog().
  static char line[8192];
  static size_t ln = 0;
  static bool inLine = false;
  while (Serial.available()) {
    int c = Serial.read();
    if (c == '@') { inLine = true; ln = 0; line[ln++] = '@'; continue; }
    if (inLine) {
      if (c == '\n' || c == '\r' || ln >= sizeof(line) - 1) {
        line[ln] = 0; inLine = false;
        if (ln > 1) handleTeensyLine(line);
      } else {
        line[ln++] = (char)c;
      }
      continue;
    }
    if (c == 'p') {
      enterPairingMode("serial cmd");
    } else if (c == 'f') {
      Serial.println("[cmd] FORGET last device, then pairing mode");
      a2dp_sink.clean_last_connection();
      enterPairingMode("forget+serial");
    } else if (c == 'x') {
      Serial.println("[cmd] DISCONNECT source");
      a2dp_sink.disconnect();
    } else if (c == 's') {
      pushStatus();
    }
  }
  // A2DP + BLE run in their own FreeRTOS tasks; nothing else to poll.
  delay(20);
}
