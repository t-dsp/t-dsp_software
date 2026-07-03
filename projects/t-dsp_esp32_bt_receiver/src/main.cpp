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

// Command opcodes: the first byte of a write to the command characteristic.
enum : uint8_t {
  CMD_PAIRING_MODE = 0x01,  // become discoverable + connectable (accept a new phone)
  CMD_END_PAIRING  = 0x02,  // leave pairing mode (non-discoverable)
  CMD_DISCONNECT   = 0x03,  // drop the current A2DP source
  CMD_FORGET       = 0x04,  // forget the last paired device (clears NVS bond)
};

BluetoothA2DPSink a2dp_sink;

// BLE state shared between the GATT callbacks (BT task context) and the app-
// visible status. Kept tiny and updated from one place (pushStatus()).
static BLECharacteristic *g_statChar = nullptr;
static volatile bool      g_bleClientConnected = false;
static bool               g_discoverable = false;  // we track this; the A2DP lib has no getter

// Build the status JSON into `buf`. Small on purpose so it fits a modest BLE
// MTU; the app can also READ the characteristic (supports long reads) for the
// full value if a notification is truncated on the default 23-byte MTU.
static void buildStatus(char *buf, size_t n) {
  bool connected = a2dp_sink.is_connected();
  const char *peer = connected ? a2dp_sink.get_peer_name() : "";
  if (!peer) peer = "";
  // conn: A2DP source connected?  disc: are we discoverable (pairing mode)?
  snprintf(buf, n, "{\"conn\":%d,\"disc\":%d,\"peer\":\"%s\"}",
           connected ? 1 : 0, g_discoverable ? 1 : 0, peer);
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
  } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
    // dropped or failed -> immediately reopen for (re)pairing
    enterPairingMode("a2dp disconnected");
  }
  pushStatus();
}

// ---- BLE GATT callbacks ---------------------------------------------------
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *) override {
    g_bleClientConnected = true;
    Serial.println("[ble] control app connected");
    pushStatus();
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
      default:
        Serial.printf("[ble] cmd: unknown opcode 0x%02X\n", op);
        break;
    }
    pushStatus();
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

  while (Serial.available()) {
    int c = Serial.read();
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
