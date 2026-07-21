// T-DSP ESP32 Bluetooth Receiver + selectable control transport (BLE | WiFi)
// ---------------------------------------------------------------------------
// Bluetooth Classic A2DP sink on the ESP32-DevKitC of the
// teensy41_digital_audio_board. Decodes A2DP audio and streams it out over
// I2S (ESP32 as I2S MASTER) into the Teensy 4.1's SAI2 slave input.
//
// A "control" front-end runs ALONGSIDE the A2DP audio so a phone app can command
// the receiver (enter pairing mode, disconnect, forget device, drive the Teensy's
// @-protocol) and read its status. The control transport is chosen at BUILD TIME
// -- exactly one of:
//
//   -D TDSP_CTRL_BLE   BLE GATT control (Bluedroid). A2DP is Bluetooth Classic and
//                      BLE is Bluetooth LE; both live on the same radio only if the
//                      controller comes up in DUAL mode (BTDM). The A2DP library
//                      defaults to CLASSIC-only and *releases* the BLE controller
//                      RAM at start(), permanently killing BLE -- so we MUST call
//                      set_default_bt_mode(ESP_BT_MODE_BTDM) BEFORE start(). Then
//                      BLEDevice::init() attaches to the already-running Bluedroid
//                      stack (it detects the controller is up and skips re-init),
//                      and A2DP + BLE coexist.
//
//   -D TDSP_CTRL_WIFI  WiFi LAN WebSocket control (NEW). The ESP32 joins your LAN
//                      in station mode and runs a WebSocket server the app connects
//                      to (discoverable as tdsp.local via mDNS). BLE is NOT started
//                      in this build -- the classic-only BT mode frees the BLE
//                      controller RAM for the WiFi/lwIP/WebSocket stacks. A2DP audio
//                      (Bluetooth Classic) and WiFi share the one radio; we bias the
//                      coexistence arbiter toward BT so audio stays glitch-free.
//
// BLE and WiFi are NEVER run at the same time -- each build has exactly one control
// transport, plus A2DP audio always. The transport-agnostic core (A2DP + I2S setup,
// the @-line relay to the Teensy, and the local A2DP verbs pair/forget/disconnect/
// status) is shared; the two front-ends live behind ControlTransport.
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

// ---- Transport selection (compile-time, exactly one) ----------------------
#if defined(TDSP_CTRL_BLE) && defined(TDSP_CTRL_WIFI)
#error "Select exactly one control transport: -D TDSP_CTRL_BLE XOR -D TDSP_CTRL_WIFI (not both)."
#elif !defined(TDSP_CTRL_BLE) && !defined(TDSP_CTRL_WIFI)
#error "No control transport selected: define -D TDSP_CTRL_BLE or -D TDSP_CTRL_WIFI in build_flags."
#endif

// ---- A2DP gate (-D TDSP_A2DP=0 to build WITHOUT Bluetooth audio) -----------
// Default 1 = normal product build. 0 = Bluetooth is never started, so WiFi owns the
// radio: no coexistence arbitration and no mandatory modem sleep. That combination is
// what makes the WiFi link slow+jittery when A2DP is up (measured: ping avg 59 ms /
// max 121 ms with BT on, on a quiet LAN), so this gate exists to A/B it and separate
// "our code is wrong" from "the radio is shared". env:esp32dev_wifi_noa2dp sets it.
#ifndef TDSP_A2DP
#define TDSP_A2DP 1
#endif

// ---- HARD RULE: WiFi control and A2DP must NEVER coexist ------------------
// The classic ESP32 has ONE 2.4 GHz radio. Running WiFi and Bluetooth-Classic
// (A2DP) together forces the mandatory WiFi/BT coexistence modem-sleep, which
// stalls the WebSocket link and SILENTLY DROPS inbound commands (measured on HW:
// ping avg 59 ms / max 121 ms, bulk catalog transfers fail). This is a hardware
// limit of the shared radio, not a fixable bug. A WiFi build MUST therefore be
// built with -D TDSP_A2DP=0 (env:esp32dev_wifi already does). This #error makes
// the forbidden combination impossible to compile. DO NOT REMOVE IT, and never
// add A2DP back to a WiFi env.
#if defined(TDSP_CTRL_WIFI) && TDSP_A2DP
#error "WiFi + A2DP is forbidden on the classic ESP32 (one shared radio). A WiFi build must set -D TDSP_A2DP=0. Never enable A2DP in a TDSP_CTRL_WIFI env."
#endif

#include <Arduino.h>

#include "BluetoothA2DPSink.h"

// Classic-BT bond list (enumerate paired phones) + NVS name storage so the app
// can show a switchable list of saved sources with real names. Transport-agnostic.
#include <esp_gap_bt_api.h>
#include <Preferences.h>

#if defined(TDSP_CTRL_BLE)
// BLE via the Arduino-ESP32 (Bluedroid-backed) BLE library. Bluedroid-backed
// matters: A2DP also uses Bluedroid, so a single stack serves both. (NimBLE is
// BLE-only and could NOT coexist with the classic A2DP profile.)
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#endif

#if defined(TDSP_CTRL_WIFI)
// Legacy Arduino WiFi + mDNS (arduino-esp32 2.0.x / IDF 4.4). WebSocket server is
// links2004/arduinoWebSockets (see platformio.ini lib_deps). No BLE headers here.
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebSocketsServer.h>
// esp_coexist.h (WiFi/BT coexistence arbiter) is present on IDF 4.4 but guard it so
// the build survives if a future platform drops/renames it.
#if __has_include(<esp_coexist.h>)
#include <esp_coexist.h>
#define TDSP_HAVE_COEX 1
#endif

// WiFi credentials come from build flags for now (bench/dev). Set them in the
// esp32dev_wifi env, e.g. -D TDSP_WIFI_SSID='"MyNet"' -D TDSP_WIFI_PASS='"secret"'.
// Runtime provisioning is a later phase. Empty fallbacks + a compile warning make
// an un-configured build obvious rather than silently failing to join.
#ifndef TDSP_WIFI_SSID
#define TDSP_WIFI_SSID ""
#warning "TDSP_WIFI_SSID not defined -- set it in platformio.ini build_flags; WiFi will not join a network."
#endif
#ifndef TDSP_WIFI_PASS
#define TDSP_WIFI_PASS ""
#warning "TDSP_WIFI_PASS not defined -- set it in platformio.ini build_flags (use \"\" for an open network)."
#endif
// WebSocket TCP port. mDNS advertises it under _ws._tcp so the app can discover it.
#ifndef TDSP_WS_PORT
#define TDSP_WS_PORT 81
#endif
// mDNS hostname -> the device is reachable at tdsp.local on the LAN.
#ifndef TDSP_MDNS_HOST
#define TDSP_MDNS_HOST "tdsp"
#endif
#endif  // TDSP_CTRL_WIFI

// Discoverable Bluetooth name your phone will pair with (classic A2DP) and, in the
// BLE build, the BLE device/advertising name the control app scans for.
static constexpr char BT_DEVICE_NAME[] = "T-DSP";

// ===========================================================================
//  Transport-agnostic core: A2DP object, shared state, @-line relays, status.
//  Everything here is identical regardless of the selected control transport.
// ===========================================================================

BluetoothA2DPSink a2dp_sink;

// App-visible state, updated from one place. In the BLE build these track the
// values pushed via opcodes; in the WiFi build the app sends @-lines verbatim, so
// conn/disc/peer are always accurate but vol/hpf/mpe/rg reflect defaults (the app
// owns that state over WiFi). See buildStatus().
static bool               g_discoverable = false;  // we track this; the A2DP lib has no getter
static uint8_t            g_volume     = 50;        // master volume 0..100 (%)
static uint8_t            g_hpf        = 0;         // TAC5212 DAC highpass mode 0..3
static uint8_t            g_midiMode   = 0;         // 0 = normal MIDI, 1 = MPE
static uint8_t            g_replayGain = 1;         // 0 = off, 1 = on (Teensy default on)

static Preferences        g_names;                  // NVS: BD-address(hex) -> friendly name
static esp_bd_addr_t      g_pendingConnect;         // switch target after a willful disconnect
static bool               g_hasPendingConnect = false;

// ---- @-line relays to the Teensy (over UART0/Serial) ----------------------
// The Teensy owns the TAC5212 codec + synth engines; the ESP32 just frames these
// "@KEY=<val>" text lines over UART. Distinct prefixes so they never collide with
// logs. These are used by the BLE opcode dispatch; the WiFi transport instead
// relays whole @-lines verbatim (relayLine), so the app speaks the same protocol.
static void relayVolume(uint8_t v)    { Serial.printf("@VOL=%u\n", v); }
static void relaySong(uint8_t idx)    { Serial.printf("@SONG=%u\n", idx); }
static void relaySongStop()           { Serial.printf("@SONG=stop\n"); }
static void relayDxVoice(uint8_t idx) { Serial.printf("@DXVOICE=%u\n", idx); }
static void relayHpf(uint8_t mode)    { Serial.printf("@HPF=%u\n", mode); }
static void relayMidiMode(uint8_t m)  { Serial.printf("@MIDIMODE=%u\n", m); }
static void relayReplayGain(uint8_t on){ Serial.printf("@RG=%u\n", on ? 1 : 0); }
static void relayLoop(uint8_t on)     { Serial.printf("@LOOP=%u\n", on ? 1 : 0); }
static void relayPressure(uint8_t m)  { Serial.printf("@PRESSURE=%u\n", m); }
static void relayModWheel(uint8_t m)  { Serial.printf("@MODWHEEL=%u\n", m); }
static void relayLfoMode(uint8_t f)   { Serial.printf("@LFOMODE=%u\n", f ? 1 : 0); }
static void relayTimbre(uint8_t m)    { Serial.printf("@TIMBRE=%u\n", m); }
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
static void relayReadFile(const char *path)  { Serial.printf("@READ=%s\n", path); }
static void relayDrumFile(const char *fname) { Serial.printf("@DRUMF=%s\n", fname); }

// Relay an arbitrary control line verbatim (the generic seam). The app sends the
// same @-lines it would over Web Serial; we just add the newline. This is what the
// WiFi transport passes every inbound @-frame through, and what the BLE
// CMD_RELAY_LINE opcode uses.
static void relayLine(const char *line)      { Serial.print(line); Serial.print('\n'); }

// Ask the Teensy to (re)send its song/instrument/drum catalog over UART.
static void requestCatalog() { Serial.print("@GETCAT\n"); }

// ---- Paired-source names (NVS) + status/sources JSON builders -------------
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
// it is the currently connected one. Transport-agnostic (pure string builder).
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

// Build the status JSON into `buf`. Small on purpose so it fits a modest BLE MTU;
// over WiFi it goes out as a whole WS TEXT frame.
//   conn: A2DP source connected?  disc: discoverable (pairing)?  vol: master 0..100
//   hpf: TAC5212 DAC highpass mode 0..3 (0=off)  mpe: 0 normal MIDI / 1 MPE
//   rg: ReplayGain 0 off / 1 on.  peer: connected phone name.
static void buildStatus(char *buf, size_t n) {
  bool connected = a2dp_sink.is_connected();
  const char *peer = connected ? a2dp_sink.get_peer_name() : "";
  if (!peer) peer = "";
  snprintf(buf, n, "{\"conn\":%d,\"disc\":%d,\"vol\":%u,\"hpf\":%u,\"mpe\":%u,\"rg\":%u,\"peer\":\"%s\"}",
           connected ? 1 : 0, g_discoverable ? 1 : 0, g_volume, g_hpf, g_midiMode, g_replayGain, peer);
}

// ---- Local A2DP pairing/connection primitives -----------------------------
// Force the sink into pairing mode: connectable + generally discoverable.
static void enterPairingMode(const char *why) {
  a2dp_sink.set_connectable(true);
  a2dp_sink.set_discoverability(ESP_BT_GENERAL_DISCOVERABLE);
  g_discoverable = true;
  Serial.printf("[a2dp] pairing mode ON (%s) -- discoverable as \"%s\"\n",
                why, BT_DEVICE_NAME);
}

// Go fully idle: NOT connectable, NOT discoverable, no auto-reconnect. Bluetooth audio is
// explicit -- it only (re)connects on an app command: "Connect Bluetooth Audio" (reconnect
// to the last bonded phone) or "Pairing mode" (accept a new phone). Used on boot and after
// any A2DP disconnect, so a phone can't silently re-attach on its own.
static void goIdle(const char *why) {
  a2dp_sink.set_connectable(false);
  a2dp_sink.set_discoverability(ESP_BT_NON_DISCOVERABLE);
  g_discoverable = false;
  Serial.printf("[a2dp] idle (%s) -- Bluetooth audio off until you connect it\n", why);
}

// ===========================================================================
//  ControlTransport: the small internal interface every front-end implements.
//  The core calls into the ACTIVE transport (controlTransport()) for the two
//  directions that differ per build:
//    - sendToApp(): a line arriving FROM the Teensy is pushed out to the client(s)
//    - pushStatus()/pushSources(): mirror A2DP state changes out to the app
// ===========================================================================
class ControlTransport {
 public:
  virtual ~ControlTransport() {}
  virtual void begin() = 0;                          // bring the transport up
  virtual void loop() {}                             // service the transport each loop()
  virtual void sendToApp(const char *line) = 0;      // outbound Teensy @-line -> app
  virtual void pushStatus() {}                       // mirror status JSON to the app
  virtual void pushSources() {}                      // mirror the paired-sources list
};

// The active transport singleton, defined once per build below.
ControlTransport &controlTransport();

// ---- Shared local A2DP verbs (called by the serial p/f/x/s chars AND the WiFi
// !pair/!forget/!disconnect/!status commands). The BLE build reaches the same
// A2DP actions through its opcode dispatch, so it keeps its own richer set. ----
static void ctrlEnterPairing() { enterPairingMode("control command"); }
static void ctrlDisconnect()   { Serial.println("[cmd] DISCONNECT source"); a2dp_sink.disconnect(); }
static void ctrlForget()       { Serial.println("[cmd] FORGET last device, then pairing mode");
                                 a2dp_sink.clean_last_connection(); enterPairingMode("forget"); }
static void ctrlStatus()       { controlTransport().pushStatus(); }
// "Connect Bluetooth Audio": dial the last bonded phone. Mirrors the BLE CMD_RECONNECT
// path -- we boot idle/non-connectable (see goIdle), so the handshake has to be allowed
// first; with no stored bond there is nothing to dial, so fall back to idle rather than
// sit there connectable. Without this the WiFi build could never START Bluetooth audio:
// the explicit-only policy means nothing ever auto-reconnects.
// WiFi-only: the serial p/f/x/s set has no 'r', and the BLE build reaches the same
// actions via its own CMD_RECONNECT opcode -- defining it there too would just be an
// unused static.
#if defined(TDSP_CTRL_WIFI)
static void ctrlReconnect()    { a2dp_sink.set_connectable(true);
                                 bool ok = a2dp_sink.reconnect();
                                 Serial.printf("[cmd] RECONNECT A2DP -> %s\n",
                                               ok ? "attempting" : "no last device (pair first)");
                                 if (!ok) goIdle("reconnect: no bond"); }
#endif

// ---- A2DP callbacks (transport-agnostic) ----------------------------------
// A2DP peer name arrived (AVRCP) -> remember it for the connected address.
static void onPeerName(char *name) {
  const esp_bd_addr_t *a = a2dp_sink.get_current_peer_address();
  if (a) storeName(*a, name);
  controlTransport().pushSources();
}

// A2DP connection state changed (A2DP lib, BT task context). Explicit-only policy:
// nothing auto-reconnects; on any disconnect we go idle until the app connects again.
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
      // Dropped or user-disconnected -> stay OFF. Explicit-only: no auto re-pair / re-attach.
      goIdle("a2dp disconnected");
    }
  }
  controlTransport().pushStatus();
  controlTransport().pushSources();
}

// A complete '@'-framed line arrived from the Teensy over UART -> hand it to the
// active transport, which routes it out to the app (BLE: chunked/framed per char;
// WiFi: broadcast verbatim).
static void handleTeensyLine(const char *line) {
  controlTransport().sendToApp(line);
}

// ===========================================================================
//  BLE control transport (-D TDSP_CTRL_BLE)
// ===========================================================================
#if defined(TDSP_CTRL_BLE)

// ---- BLE control contract -------------------------------------------------
// Custom 128-bit UUIDs for the T-DSP control service. The app filters scans by
// TDSP_SVC_UUID so it finds this device by capability, not by a fuzzy name.
//   Service  7a9c0001-...  "T-DSP Control"
//   Command  7a9c0002-...  WRITE        (1 byte opcode + optional args)
//   Status   7a9c0003-...  READ+NOTIFY  (small JSON string, see buildStatus())
#define TDSP_SVC_UUID  "7a9c0001-4a6e-4b7d-8f1a-2d3c4e5f6a70"
#define TDSP_CMD_UUID  "7a9c0002-4a6e-4b7d-8f1a-2d3c4e5f6a70"
#define TDSP_STAT_UUID "7a9c0003-4a6e-4b7d-8f1a-2d3c4e5f6a70"
// Sources list (READ+NOTIFY): JSON array of paired phones, e.g.
//   [{"a":"aabbccddeeff","n":"Pixel 7","c":1}]  (a=addr hex, n=name, c=connected)
#define TDSP_SRC_UUID  "7a9c0004-4a6e-4b7d-8f1a-2d3c4e5f6a70"
// Device catalog (READ+NOTIFY): '|'-delimited name lists the Teensy streams over
// UART (@SONGS=/@INSTR=). A list longer than one BLE value (512 B) is streamed as a
// burst of framed NOTIFY chunks ("<seq>\x1e<count>\x1e<payload>") that the app
// reassembles -- see setCatalog(). A single-chunk list is just count=1.
#define TDSP_SONGS_UUID "7a9c0005-4a6e-4b7d-8f1a-2d3c4e5f6a70"
#define TDSP_INSTR_UUID "7a9c0006-4a6e-4b7d-8f1a-2d3c4e5f6a70"
// Drum grooves catalog (READ+NOTIFY): @DRUMS=, same chunked-burst contract.
#define TDSP_DRUMS_UUID "7a9c0007-4a6e-4b7d-8f1a-2d3c4e5f6a70"
// Generic file transfer (READ+NOTIFY): the Teensy streams any SD file as base64
// frames (@FB/@FD/@FE/@FERR) plus the manifest registry (@MANIFESTS), forwarded
// VERBATIM one line per notification (each fits a ~512 MTU). See CATALOG_TRANSPORT.md.
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
  CMD_SET_DRUM_SPEED=0x33,  // RESERVED -- drum-speed control removed (drums follow master BPM).
  CMD_SET_DRUM_VOL = 0x34,  // + 1 byte: drum level 0..150 (%) (@DRUMVOL=<pct>)
  CMD_SET_BPM      = 0x35,  // + 1 byte: master tempo 40..240 bpm -- song+drum (@BPM=<n>)
  CMD_SET_DRUM_SYNCHRO=0x36,// + 1 byte: 0/1 synchro start (groove begins on first note) (@DRUMSYNCHRO=<0|1>)
  CMD_SET_ARP_ON   = 0x37,  // + 1 byte: 0/1 arpeggiator enable (@ARPON=<0|1>)
  CMD_SET_ARP_PATTERN=0x38, // + 1 byte: pattern index 0..24 (@ARPPAT=<n>)
  CMD_SET_ARP_RATE = 0x39,  // + 1 byte: rate index 0..14 (@ARPRATE=<n>)
  CMD_SET_ARP_OCT  = 0x3a,  // + 1 byte: octave range 1..4 (@ARPOCT=<n>)
  CMD_SET_ARP_LATCH= 0x3b,  // + 1 byte: 0/1 latch held notes (@ARPLATCH=<0|1>)
  CMD_READ_FILE    = 0x40,  // + N bytes: SD path string; Teensy streams it on the FILE char (@READ=<path>)
  CMD_PLAY_DRUM_FILE=0x41,  // + N bytes: groove filename; plays /drums/<name> (@DRUMF=<filename>)
  CMD_RELAY_LINE   = 0x42,  // + N bytes: a literal control line relayed VERBATIM to the Teensy.
};

static BLECharacteristic *g_statChar  = nullptr;
static BLECharacteristic *g_srcChar   = nullptr;
static BLECharacteristic *g_songsChar = nullptr;
static BLECharacteristic *g_instrChar = nullptr;
static BLECharacteristic *g_drumsChar = nullptr;
static BLECharacteristic *g_fileChar  = nullptr;   // generic file-transfer frames (pass-through)
static volatile bool      g_bleClientConnected = false;

// Forward ONE Teensy line verbatim to the app as a single notification. Used for the
// file-transfer stream: the Teensy already chunked each @FD to fit one MTU.
static void notifyRaw(BLECharacteristic *ch, const char *line) {
  if (!ch) return;
  ch->setValue((uint8_t *)line, strlen(line));
  if (g_bleClientConnected) { ch->notify(); delay(12); }
}

// Store a '|'-delimited name list into a catalog characteristic and notify. A BLE
// characteristic value caps at 512 B, so a long list is streamed as a burst of framed
// notifications, each "<seq>\x1e<count>\x1e<payload>" (0x1e = record sep). Notifications
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

// Refresh the status characteristic value and notify any subscribed client.
static void blePushStatus() {
  if (!g_statChar) return;
  char buf[96];
  buildStatus(buf, sizeof(buf));
  g_statChar->setValue((uint8_t *)buf, strlen(buf));
  if (g_bleClientConnected) g_statChar->notify();
  Serial.printf("[ble] status %s\n", buf);
}

// Refresh the sources characteristic and notify (the app re-reads the full value).
static void blePushSources() {
  if (!g_srcChar) return;
  static char buf[640];
  buildSources(buf, sizeof(buf));
  g_srcChar->setValue((uint8_t *)buf, strlen(buf));
  if (g_bleClientConnected) g_srcChar->notify();
  Serial.printf("[ble] sources %s\n", buf);
}

// ---- BLE GATT callbacks ---------------------------------------------------
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *) override {
    g_bleClientConnected = true;
    Serial.println("[ble] control app connected");
    blePushStatus();
    blePushSources();
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
        a2dp_sink.set_connectable(true);  // allow the reconnect handshake (we boot idle/non-connectable)
        bool ok = a2dp_sink.reconnect();  // sink -> last paired phone (needs a stored bond)
        Serial.printf("[ble] cmd: RECONNECT A2DP -> %s\n",
                      ok ? "attempting" : "no last device (pair first)");
        if (!ok) goIdle("reconnect: no bond");   // nothing to connect -> back to idle
        break;
      }
      case CMD_SET_VOLUME:
        if (v.size() >= 2) {
          g_volume = (uint8_t)v[1];
          if (g_volume > 100) g_volume = 100;
          Serial.printf("[ble] cmd: SET VOLUME %u%%\n", g_volume);
          relayVolume(g_volume);  // -> Teensy -> TAC5212 setDvol()
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
          g_hpf = mode;     // remember for the status readback (blePushStatus below)
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
          blePushStatus();                 // mirror new state back to the app
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
    blePushStatus();
    blePushSources();
  }
};

static void setupBle() {
  // Attaches to the Bluedroid stack A2DP already brought up in BTDM mode.
  BLEDevice::init(BT_DEVICE_NAME);

  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  BLEService *svc = server->createService(BLEUUID(TDSP_SVC_UUID), 30);   // >=21 handles: 7 chars + CCCDs (default 15 drops DRUMS/FILE)

  BLECharacteristic *cmd = svc->createCharacteristic(
      TDSP_CMD_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  cmd->setCallbacks(new CommandCallbacks());

  g_statChar = svc->createCharacteristic(
      TDSP_STAT_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  g_statChar->addDescriptor(new BLE2902());  // CCCD so the app can subscribe

  g_srcChar = svc->createCharacteristic(
      TDSP_SRC_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  g_srcChar->addDescriptor(new BLE2902());

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

  blePushStatus();
  Serial.println("[ble] control service up + advertising");
}

// BLE front-end behind the ControlTransport seam.
class BleControlTransport : public ControlTransport {
 public:
  void begin() override { setupBle(); }
  void loop() override {}    // BLE runs in its own FreeRTOS tasks; nothing to poll
  void pushStatus() override { blePushStatus(); }
  void pushSources() override { blePushSources(); }

  // Route a Teensy line out to the app over the appropriate characteristic. This is
  // exactly the old handleTeensyLine() body -- catalog lists get chunk-framed, the
  // file/manifest stream is forwarded verbatim.
  void sendToApp(const char *line) override {
    if      (strncmp(line, "@SONGS=", 7) == 0) { setCatalog(g_songsChar, line + 7); Serial.println("[cat] songs updated"); }
    else if (strncmp(line, "@INSTR=", 7) == 0) { setCatalog(g_instrChar, line + 7); Serial.println("[cat] instruments updated"); }
    else if (strncmp(line, "@DRUMS=", 7) == 0) { setCatalog(g_drumsChar, line + 7); Serial.println("[cat] drums updated"); }
    else if (strncmp(line, "@MANIFESTS=", 11) == 0) { notifyRaw(g_fileChar, line); }
    else if (strncmp(line, "@FB=", 4) == 0 || strncmp(line, "@FD=", 4) == 0 ||
             strncmp(line, "@FE=", 4) == 0 || strncmp(line, "@FERR=", 6) == 0) { notifyRaw(g_fileChar, line); }
    else if (strncmp(line, "@REINDEXED", 10) == 0) { notifyRaw(g_fileChar, line); }
    else if (strncmp(line, "@DXLS=", 6) == 0 || strncmp(line, "@DXVL=", 6) == 0) { setCatalog(g_fileChar, line); }
    // The full-state snapshot (@STATE, ~2 KB now that it carries voice2/arp2/caps) and the
    // opaque app-state blob (@APP) also exceed one MTU, so stream them chunk-framed too. The
    // app reassembles and routes them to its onLine() handler (hydrate). WITHOUT this the phone
    // never receives @STATE, so build-gated cards (Synth 2 / Arp 2, via caps) stay hidden.
    // (The WiFi transport needs none of this: it broadcasts every line whole.)
    else if (strncmp(line, "@STATE=", 7) == 0 || strncmp(line, "@APP=", 5) == 0) { setCatalog(g_fileChar, line); }
  }
};

ControlTransport &controlTransport() { static BleControlTransport t; return t; }

#endif  // TDSP_CTRL_BLE

// ===========================================================================
//  WiFi LAN WebSocket control transport (-D TDSP_CTRL_WIFI)
// ===========================================================================
#if defined(TDSP_CTRL_WIFI)

// WiFi control wire contract (see README):
//   * Discover the device at tdsp.local; WebSocket server on TDSP_WS_PORT (81).
//   * Inbound (app -> ESP32, WS TEXT frame):
//       - a line starting with '@' is relayed VERBATIM to the Teensy (the same
//         @-protocol Web Serial / BLE CMD_RELAY_LINE use), e.g. "@VOL=50", "@SONG=3".
//       - a line starting with '!' is a LOCAL A2DP command handled on the ESP32:
//         "!pair" | "!reconnect" | "!forget" | "!disconnect" | "!status".
//       - anything else is ignored (logged).
//   * Outbound (ESP32 -> app, WS TEXT frame):
//       - every Teensy @-line is broadcast VERBATIM (no BLE 0x1e chunking; WS frames
//         are large and the client tolerates whole lines).
//       - status is a JSON line (buildStatus); the paired-sources JSON is broadcast
//         as "@SOURCES=<json>".

static WebSocketsServer g_ws(TDSP_WS_PORT);

// ---- FlasherX flash-bridge -------------------------------------------------
// A PC can reflash the Teensy over WiFi by making the ESP32 a TRANSPARENT tunnel
// between one WS client and UART0 (Serial <-> Teensy Serial7). On "!fxflash" we
// send "@FXUP\n" to put the Teensy into FlasherX mode (reading Intel-hex from
// Serial7), then loop() pipes raw bytes both ways until "!fxend" or an idle
// timeout. The UART is fixed 115200, and Serial.write() blocks when its TX FIFO is
// full, so the tunnel self-paces to the line rate -- the Teensy never overruns.
// PC-side client: tools/fxflash_wifi.py. See lib/FlasherX + firmware/mix-kit @FXUP.
static bool     g_fxBridge   = false;   // tunnel active
static uint8_t  g_fxClient   = 0;       // the WS client that owns the tunnel
static uint32_t g_fxLastByte = 0;       // last tunnel activity (ms), for idle timeout
static const uint32_t kFxIdleMs = 10000;  // no traffic this long -> drop the tunnel

// Send one logical line to a client (num >= 0) or to all (num < 0), '\n'-terminated and
// split into bounded chunks.
//
// HARDWARE-VERIFIED BUG THIS FIXES: broadcasting a whole line in ONE WS frame wedges the
// socket. A catalog line is big -- @INSTR (Dexed's 320-voice list) is ~7 KB -- which
// overruns the ESP32's lwIP TCP send buffer (~5.7 KB default). WiFiClient::write() then
// fails with errno 11 EAGAIN ("No more processes") and the connection never recovers:
// inbound commands still reach the Teensy, but NOTHING is ever written back. So the
// original "WiFi frames are large, no chunking needed" assumption was wrong in practice.
//
// The chunks are NOT a framing protocol -- there is no 0x1e header and no reassembly
// state. We simply stream bytes and terminate each line with '\n'; the client accumulates
// until it sees one (see app/tdsp-control/src/transport.wifi.ts). That keeps whole-line
// semantics while staying inside the TCP send buffer.
static void wsSendLine(int num, const char *line) {
  // 512 B chunks with a real gap. Sizing/pacing is NOT arbitrary: the lwIP send buffer is
  // ~5.7 KB, and WiFi throughput here is throttled by the modem sleep that BT coexistence
  // makes mandatory -- so bytes leave slowly and a tight loop fills the buffer long before
  // it drains. 2 ms gaps were NOT enough (socket still wedged with EAGAIN); ~1 KB/15 ms
  // keeps us well inside it. g_ws.loop() between chunks services the stack so the TCP
  // window actually reopens instead of us just sleeping.
  static const size_t kWsChunk = 512;
  static const uint32_t kGapMs = 15;
  size_t len = strlen(line);
  for (size_t off = 0; off < len; ) {
    size_t n = (len - off < kWsChunk) ? (len - off) : kWsChunk;
    if (num < 0) g_ws.broadcastTXT((uint8_t *)(line + off), n);
    else         g_ws.sendTXT((uint8_t)num, (uint8_t *)(line + off), n);
    off += n;
    if (off < len) { g_ws.loop(); delay(kGapMs); }   // let lwIP actually drain
  }
  if (num < 0) g_ws.broadcastTXT((uint8_t *)"\n", 1);
  else         g_ws.sendTXT((uint8_t)num, (uint8_t *)"\n", 1);
}

// One inbound WS TEXT message. '!' = local A2DP verb; '@' = verbatim to the Teensy.
static void wsHandleText(uint8_t num, const char *msg) {
  if (msg[0] == '!') {
    if      (!strcmp(msg, "!pair"))       { Serial.println("[ws] cmd: PAIR");       ctrlEnterPairing(); }
    else if (!strcmp(msg, "!reconnect"))  { Serial.println("[ws] cmd: RECONNECT");  ctrlReconnect(); }
    else if (!strcmp(msg, "!forget"))     { Serial.println("[ws] cmd: FORGET");     ctrlForget(); }
    else if (!strcmp(msg, "!disconnect")) { Serial.println("[ws] cmd: DISCONNECT"); ctrlDisconnect(); }
    else if (!strcmp(msg, "!status"))     { char b[96]; buildStatus(b, sizeof(b)); wsSendLine(num, b); }
    else if (!strcmp(msg, "!fxflash")) {
      // Enter the FlasherX tunnel: hand this client raw UART0 access and put the
      // Teensy into @FXUP mode. From here, hex bytes arrive as WS BIN frames.
      // IMPORTANT: once bridging, UART0 belongs to the transfer -- the ONLY thing
      // we may write to Serial is @FXUP and the tunneled hex. Any stray debug
      // print would be read by FlasherX as a bad hex line and abort the flash. So
      // status goes to the WS client, never to Serial.
      // NOTE: do NOT touch A2DP here -- a2dp_sink.disconnect() (and its state
      // callback) log to Serial, which IS the UART to the Teensy, corrupting the
      // very first hex line. The real drop was Serial.write() returning short under
      // load; that's fixed in the WStype_BIN handler (write-all loop).
      g_fxClient = num; g_fxBridge = true; g_fxLastByte = millis();
      Serial.print("@FXUP\n");        // Teensy enters fxRunUpdate() on Serial7
      wsSendLine(num, "!fxbridge=on");  // ack to the PC over WS, not the UART
    }
    else if (!strcmp(msg, "!fxend")) {
      g_fxBridge = false;
      wsSendLine(num, "!fxbridge=off");
    }
    else Serial.printf("[ws] unknown local cmd: %s\n", msg);
    return;
  }
  if (msg[0] == '@') { relayLine(msg); return; }   // verbatim to the Teensy
  Serial.printf("[ws] ignored (not @/!): %s\n", msg);
}

static void onWsEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      Serial.printf("[ws] client %u connected\n", num);
      // Prime the freshly-connected app with current status only.
      //
      // Deliberately NO requestCatalog() here (the BLE build still does it). @SONGS=/
      // @INSTR=/@DRUMS= are the LEGACY BLE catalog -- the WiFi app doesn't read them at
      // all, it fetches /tdsp/*.ndjson via @READ (see app/tdsp-control/src/catalog.ts).
      // Sending them cost ~9.4 KB of unwanted traffic the instant a client connected,
      // which flooded lwIP and wedged the socket (EAGAIN) before the app's first @READ
      // even went out -- so the catalog load timed out. Hardware-verified.
      char buf[96];
      buildStatus(buf, sizeof(buf));
      wsSendLine(num, buf);
      break;
    }
    case WStype_DISCONNECTED:
      Serial.printf("[ws] client %u disconnected\n", num);
      break;
    case WStype_TEXT: {
      // payload is not guaranteed null-terminated -> bounded copy. Sized for the
      // largest inbound control line (paths / relayed lines are short).
      static char msg[1024];
      size_t n = length < sizeof(msg) - 1 ? length : sizeof(msg) - 1;
      memcpy(msg, payload, n);
      msg[n] = 0;
      wsHandleText(num, msg);
      break;
    }
    case WStype_BIN:
      // Raw firmware bytes for the FlasherX tunnel -> straight out UART0 to the
      // Teensy. Serial.write() blocks on a full TX FIFO, pacing us to 115200 so
      // the Teensy's hex parser never overruns. Ignored unless this client owns
      // an active tunnel.
      if (g_fxBridge && num == g_fxClient) {
        // Write EVERY byte. Arduino-ESP32 Serial.write() can return short when the
        // UART TX FIFO is full under WiFi/BT load; ignoring the count silently drops
        // the remainder -> a missing byte -> "bad hex line" on the Teensy (whose RX
        // never overran). Loop until the whole frame is out, yielding if momentarily
        // full so the FIFO can drain at 115200.
        size_t off = 0;
        while (off < length) {
          size_t w = Serial.write(payload + off, length - off);
          off += w;
          if (off < length) delay(1);   // TX FIFO full -> let it drain
        }
        g_fxLastByte = millis();
      }
      break;
    default:
      break;   // PING/PONG/FRAGMENT not used
  }
}

// WiFi + WebSocket + mDNS front-end behind the ControlTransport seam. WiFi shares the
// radio with A2DP (Bluetooth Classic); the coex arbiter is biased toward BT so audio
// stays glitch-free. BLE is intentionally NOT started (frees the BLE controller RAM).
class WifiControlTransport : public ControlTransport {
 public:
  void begin() override {
    WiFi.persistent(false);            // don't wear flash writing creds every boot
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(TDSP_MDNS_HOST);  // DHCP hostname (mDNS name set separately)
#if TDSP_A2DP
    // WiFi modem sleep MUST stay ENABLED while Bluetooth is up. A2DP (Bluetooth Classic)
    // and WiFi share the one 2.4 GHz radio, and the coexistence arbiter time-slices them
    // using the modem-sleep windows. With sleep disabled, IDF hard-abort()s the instant
    // WiFi starts:
    //   "E wifi: Should enable WiFi modem sleep when both WiFi and Bluetooth are enabled"
    //   abort() ... Rebooting...
    // -> a boot-loop, verified on hardware. Do NOT "optimize" this away; it is
    // non-negotiable while Bluetooth is up. (true = WIFI_PS_MIN_MODEM, also the
    // arduino-esp32 default; set explicitly so the reason is on record.)
    //
    // The COST is real and measured: the radio parks between beacons, so WS writes stall
    // (ping avg ~59 ms / max ~121 ms on a quiet LAN, vs ~5 ms with BT off) and bulk @READ
    // transfers fall behind. That is the price of WiFi+A2DP on one chip.
    WiFi.setSleep(true);
#else
    // A2DP gated off -> no Bluetooth, no coexistence, so nothing forces modem sleep.
    // WiFi keeps the radio awake: lowest latency, which is the point of this build.
    WiFi.setSleep(false);
#endif
#if TDSP_A2DP && defined(TDSP_HAVE_COEX)
    // Bias the WiFi/BT coexistence arbiter toward Bluetooth so A2DP audio is
    // prioritized over WiFi on the shared radio (best-effort; guarded for portability).
    // Pointless with the A2DP gate off -- there is nothing to coexist with.
    esp_coex_preference_set(ESP_COEX_PREFER_BT);
#endif
    Serial.printf("[wifi] connecting to \"%s\"...\n", TDSP_WIFI_SSID);
    WiFi.begin(TDSP_WIFI_SSID, TDSP_WIFI_PASS);   // non-blocking; loop() reports when up

    g_ws.begin();
    g_ws.onEvent(onWsEvent);
    Serial.printf("[ws] server listening on port %u\n", (unsigned)TDSP_WS_PORT);
  }

  void loop() override {
    g_ws.loop();
    maintainWifi();
    // Flush broadcasts requested from the BT task (see pushStatus/pushSources).
    if (pendingStatus)  { pendingStatus  = false; broadcastStatus(); }
    if (pendingSources) { pendingSources = false; broadcastSources(); }
  }

  // Called from loop() (Arduino task) via handleTeensyLine -> safe to send directly.
  // Whole line, chunked + '\n'-terminated (see wsSendLine: a 7 KB @INSTR in one frame
  // wedges the socket). No BLE-style 0x1e framing -- the client just splits on '\n'.
  void sendToApp(const char *line) override { wsSendLine(-1, line); }

  // NOTE: these are called from the A2DP callback, which runs in the *BT task*.
  // arduinoWebSockets is NOT thread-safe and the server is serviced on the Arduino
  // loop task, so touching g_ws from here could corrupt its client state. Just raise
  // a flag; loop() does the actual broadcast on the right task.
  void pushStatus() override  { pendingStatus  = true; }
  void pushSources() override { pendingSources = true; }

 private:
  volatile bool pendingStatus  = false;
  volatile bool pendingSources = false;
  bool     mdnsUp   = false;
  bool     wasConn  = false;
  uint32_t lastTry  = 0;

  void broadcastStatus() {
    char buf[96];
    buildStatus(buf, sizeof(buf));
    wsSendLine(-1, buf);
  }

  void broadcastSources() {
    char src[600];
    buildSources(src, sizeof(src));
    char buf[640];
    snprintf(buf, sizeof(buf), "@SOURCES=%s", src);   // framed so the app can tell it apart
    wsSendLine(-1, buf);
  }

  void maintainWifi() {
    if (WiFi.status() == WL_CONNECTED) {
      if (!wasConn) {
        wasConn = true;
        IPAddress ip = WiFi.localIP();
        Serial.printf("[wifi] connected: %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);
        if (!mdnsUp) startMdns();
      }
    } else {
      if (wasConn) { wasConn = false; Serial.println("[wifi] link lost -> reconnecting"); }
      uint32_t now = millis();
      if (now - lastTry >= 5000) { lastTry = now; WiFi.reconnect(); }   // non-blocking retry
    }
  }

  void startMdns() {
    if (MDNS.begin(TDSP_MDNS_HOST)) {
      // Advertise TWO service types on the same port:
      //  * _tdsp._tcp -- OUR type. The app browses this so it finds T-DSP units and ONLY
      //    T-DSP units (several may be on one LAN, and _ws._tcp is generic enough that any
      //    random WebSocket gadget would show up in the picker).
      //  * _ws._tcp   -- generic, kept so plain WS tooling/browsers can still see it.
      MDNS.addService("tdsp", "tcp", TDSP_WS_PORT);
      MDNS.addService("ws", "tcp", TDSP_WS_PORT);
      // TXT records: let a client label the device in a picker WITHOUT connecting to it,
      // and skip units it can't talk to. `name` is the friendly label; `proto` guards
      // against a future wire-format change; `a2dp` says whether this build has Bluetooth
      // audio at all (the TDSP_A2DP gate).
      MDNS.addServiceTxt("tdsp", "tcp", "name", BT_DEVICE_NAME);
      MDNS.addServiceTxt("tdsp", "tcp", "proto", "at-line/1");
      MDNS.addServiceTxt("tdsp", "tcp", "a2dp", TDSP_A2DP ? "1" : "0");
      mdnsUp = true;
      Serial.printf("[mdns] advertising %s.local (_tdsp._tcp + _ws._tcp :%u)\n",
                    TDSP_MDNS_HOST, (unsigned)TDSP_WS_PORT);
    } else {
      Serial.println("[mdns] FAILED to start");
    }
  }
};

ControlTransport &controlTransport() { static WifiControlTransport t; return t; }

#endif  // TDSP_CTRL_WIFI

// ===========================================================================
//  Arduino setup()/loop() -- A2DP + I2S (always) + the active control transport.
// ===========================================================================

// Onboard LED (GPIO2 on the ESP32-DevKitC). Blinked as a heartbeat in loop() so
// you can SEE the app is actually running -- independent of the serial mirror
// (which is unreliable when the ESP32's own USB/CP210x is unpowered) and of BT.
static constexpr int LED_PIN = 2;

void setup() {
  Serial.setRxBufferSize(16384);   // hold a full @INSTR/@DXLS catalog burst from the Teensy while the loop is busy (default 256 B overflows)
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
#if defined(TDSP_CTRL_BLE)
  Serial.println("T-DSP ESP32: A2DP sink + BLE control, starting...");
#else
  Serial.println("T-DSP ESP32: A2DP sink + WiFi WebSocket control, starting...");
#endif

#if !TDSP_A2DP
  // A2DP gated OFF at build time: Bluetooth is never started, so WiFi owns the radio
  // outright (no coexistence, no mandatory modem sleep -- see WifiControlTransport::begin).
  // Diagnostic/experimental build: proves how much of the WiFi latency is BT coexistence.
  // There is NO Bluetooth audio in this build.
  Serial.println("[a2dp] DISABLED at build time (-D TDSP_A2DP=0) -- WiFi has the radio to itself");
#else
  // Bluetooth mode BEFORE start(): BTDM keeps BLE alive alongside A2DP (BLE build);
  // CLASSIC_BT frees the BLE controller RAM for the WiFi/lwIP/WebSocket stacks (WiFi build).
#if defined(TDSP_CTRL_BLE)
  a2dp_sink.set_default_bt_mode(ESP_BT_MODE_BTDM);
#else
  a2dp_sink.set_default_bt_mode(ESP_BT_MODE_CLASSIC_BT);
#endif

  // Explicit-only: never auto-reconnect to the last phone; the app decides.
  a2dp_sink.set_auto_reconnect(false);

  // Mirror A2DP connect/disconnect out to the active transport's status.
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
  // Boot IDLE, not pairing: Bluetooth audio is explicit. Tap "Connect Bluetooth Audio"
  // in the app to reconnect the last phone, or "Pairing mode" to add a new one.
  goIdle("boot");
  Serial.printf("A2DP sink up (idle). Connect from the app to start Bluetooth audio.\n");
#endif  // TDSP_A2DP

  // Control transport comes up AFTER A2DP: BLE attaches to the running Bluedroid
  // stack; WiFi just joins the LAN + starts the WS server. Exactly one is compiled in.
  controlTransport().begin();
  requestCatalog();   // cache the Teensy's song/instrument lists early
  Serial.println("Ready: streaming audio + control transport both live.");
}

void loop() {
  // Host command interface over UART0. In the bridge-only setup the ESP32's own
  // USB is unplugged, so the Teensy forwards a byte over Serial7 (mix firmware:
  // 'P'->'p', 'F'->'f') letting the host drive pairing ON COMMAND without the app.
  //   p = enter pairing mode (discoverable + connectable)
  //   f = forget last device (clear NVS bond) THEN enter pairing mode  <- clean re-pair
  //   x = disconnect the current A2DP source
  //   s = print/emit status
  // Heartbeat: blink the onboard LED ~2 Hz so it's visually obvious the app is
  // running. If this LED is NOT blinking, the ESP32 is stuck (download mode / reset).
  static uint32_t lastBlink = 0;
  if (millis() - lastBlink >= 250) {
    lastBlink = millis();
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  }

  // Service the active control transport (WiFi needs webSocket.loop() + reconnect;
  // BLE is a no-op -- it runs in its own tasks).
  controlTransport().loop();

#if defined(TDSP_CTRL_WIFI)
  // FlasherX tunnel: while active, UART0 is dedicated to the flash transfer. Pipe
  // everything the Teensy emits (FlasherX status + the "enter N to flash" prompt)
  // back to the owning WS client as BIN, and DON'T let the normal @-line parser
  // below eat those bytes. PC->Teensy hex arrives via WStype_BIN (see onWsEvent).
  if (g_fxBridge) {
    uint8_t buf[512];
    int n = 0;
    while (Serial.available() && n < (int)sizeof(buf)) buf[n++] = (uint8_t)Serial.read();
    if (n > 0) { g_ws.sendBIN(g_fxClient, buf, n); g_fxLastByte = millis(); }
    // The Teensy reboots (or aborts) at the end of a flash -> UART0 goes quiet.
    // Drop the tunnel on idle so normal control resumes without needing !fxend.
    if (millis() - g_fxLastByte > kFxIdleMs) {
      // Teensy rebooted (or aborted) -> UART0 quiet. Resume normal control. No
      // Serial print here: if the Teensy is mid-reboot a stray byte is harmless,
      // but keeping UART0 clean during the handoff is the safe habit.
      g_fxBridge = false;
    }
    delay(2);
    return;   // skip the normal single-char / @-line UART parser while tunneling
  }
#endif

  // Two framings share this UART: bare single-char commands (p/f/x/s from the
  // Teensy's P/F relay + pairing) and '@'-framed lines (e.g. @SONGS=/@INSTR=
  // catalog). A '@' starts line mode until '\n'; other bytes are single commands.
  // Big enough for the whole catalog line: Dexed's full 320-voice @INSTR list is
  // ~7 KB. The value is then routed out by the active transport (chunked over BLE,
  // verbatim over WiFi).
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
      ctrlEnterPairing();
    } else if (c == 'f') {
      ctrlForget();
    } else if (c == 'x') {
      ctrlDisconnect();
    } else if (c == 's') {
      ctrlStatus();
    }
  }
  // A2DP + (BLE, if built) run in their own FreeRTOS tasks; the WiFi transport is
  // serviced above via controlTransport().loop(). Small delay to yield.
  delay(20);
}
