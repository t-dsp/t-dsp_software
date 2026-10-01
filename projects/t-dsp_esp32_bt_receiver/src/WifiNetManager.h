// WifiNetManager.h -- runtime Wi-Fi settings + connection policy for the WiFi control build.
//
// The device ALWAYS runs its own access point, so a phone can join it directly with no router
// (festival mode). It ALSO joins a known home/venue network when one is in range. This class owns:
//
//   * Settings, stored in NVS (Preferences namespace "tdspwifi"), editable at runtime from the
//     control UI -- no reflash to add a network or change the AP password:
//       - up to kMaxSaved station networks (ssid + password)
//       - the access point's name + password
//     The compile-time .env values are only a first-boot SEED: TDSP_WIFI_SSID/PASS are imported
//     once (flag "seeded"), and TDSP_AP_SSID/PASS are used until the UI saves its own.
//
//   * The connection policy. The classic ESP32 has ONE radio shared by the AP and the station.
//     A station scan hops channels, which takes the radio away from the AP's channel, and phones
//     on the AP see it as loss + latency. Measured 2026-09-13 on the old policy (WiFi.reconnect()
//     every 5 s PLUS arduino-esp32's AutoReconnect, which re-begin()s instantly on NO_AP_FOUND,
//     i.e. near-continuous scanning when the home network is absent): a client on the AP saw
//     23% ping loss, loss bursts up to 4.2 s, and WS control p95 680 ms / max 1.5 s. So:
//       - AutoReconnect is OFF; this class decides every scan and every connect attempt.
//       - One short async scan, then connect straight to the strongest SAVED network found
//         (channel + BSSID from the scan, so the connect itself does not scan again).
//       - Nothing found -> exponential backoff between scans: 10 s, 20 s, 40 s ... capped at 5 min.
//       - HOLD: while any phone is connected to the AP, never scan and never auto-connect.
//         Joining a LAN on another channel would also drag the AP to that channel and knock the
//         phone off. The UI's "Connect now" / "Save" override the hold explicitly.
//       - A lost LAN link gets one quick retry (3 s) and then the same backoff + hold rules.
//       - PAUSE: after 3 failed rounds in a row (router refusing us, wrong password, network gone)
//         stop trying for 30 min. Every attempt drags the shared radio off the AP channel for up to
//         15 s, and a phone joining T-DSP in that window fails or gets dropped — measured 2026-10-01
//         with the LAN refusing association (WPA3-only router): attempts every 1-5 min made the
//         AP path flaky. "Connect now" / "Save" clear the pause; so does a successful join.
//       - CHANNEL: the AP lives on TDSP_AP_CHANNEL (default 1). A station attempt drags the single
//         radio onto the LAN's channel and the AP stays there after a failure, so after every failed
//         round (and a lost link) the AP config is re-applied and the channel restored.
//       - GRACE: the first LAN round waits 45 s after boot so the phone can join the AP first.
//
// Threading: loop() runs on the Arduino task; the UI actions and JSON readers are called from the
// AsyncTCP task (HTTP handlers). Every public method takes `mu_` (recursive: actions call each other).
// The STA_DISCONNECTED event lambda runs on the WiFi event task and only bumps volatile counters.
#pragma once
#ifndef TDSP_AP_CHANNEL
#define TDSP_AP_CHANNEL 1   // 2.4 GHz channel for the T-DSP access point (1/6/11); see startAp()
#endif

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <utility>
#include <mutex>
#include <esp_wifi.h>   // esp_wifi_scan_stop

class WifiNetManager {
 public:
  static constexpr int kMaxSaved = 5;
  static constexpr int kMaxScan = 24;

  enum class State : uint8_t { Idle, Waiting, Connecting, Connected };

  // publicDefaultPass = the password shipped in the public repo; status reports when it is
  // still in use so the UI can nag.
  void begin(const char *apSsidDefault, const char *apPassDefault, const char *seedSsid, const char *seedPass,
             const char *publicDefaultPass) {
    publicDefaultPass_ = publicDefaultPass;
    apSsidDefault_ = apSsidDefault;
    apPassDefault_ = apPassDefault;
    prefs_.begin("tdspwifi", false);
    apSsid_ = prefs_.getString("apS", apSsidDefault);
    apPass_ = prefs_.getString("apP", apPassDefault);
    if (!validApSsid(apSsid_)) apSsid_ = apSsidDefault;
    if (!validApPass(apPass_)) apPass_ = apPassDefault;
    loadSaved();
    if (!prefs_.getBool("seeded", false)) {
      if (seedSsid && seedSsid[0] && count_ == 0) saveNetwork(String(seedSsid), String(seedPass ? seedPass : ""));
      prefs_.putBool("seeded", true);
    }

    WiFi.persistent(false);         // settings live in OUR namespace; don't let the driver write NVS
    WiFi.setAutoReconnect(false);   // see header: the framework's instant re-begin() scans nonstop
    WiFi.onEvent([this](arduino_event_id_t, arduino_event_info_t info) {
      uint8_t r = info.wifi_sta_disconnected.reason;
      if (r == WIFI_REASON_ASSOC_LEAVE) return;   // our own WiFi.disconnect(): not an attempt failing
      lastReason_ = r;
      discCount_++;
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    WiFi.mode(WIFI_AP_STA);
    startAp();

    // First LAN round only after a boot grace: the owner usually powers the box on and reaches for
    // the app, and a phone joining T-DSP while a station attempt has the radio on the LAN's channel
    // fails or gets dropped. 45 s lets that first join land; once a phone is on, HOLD keeps the LAN
    // rounds off anyway. A LAN-first venue waits 45 s longer for tdsp.local -- acceptable.
    if (count_ > 0) { state_ = State::Waiting; nextAt_ = millis() + kBootGraceMs; }
    else            { state_ = State::Idle; }
  }

  void loop() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    uint32_t now = millis();

    if (apRestartAt_ && (int32_t)(now - apRestartAt_) >= 0) {   // deferred so the HTTP reply flushes first
      apRestartAt_ = 0;
      WiFi.softAPdisconnect(false);
      startAp();
    }

    if (scanning_) serviceScan();

    switch (state_) {
      case State::Idle:
        break;

      case State::Waiting:
        if (scanning_ || (int32_t)(now - nextAt_) < 0) break;
        if (WiFi.softAPgetStationNum() > 0 && !force_) break;   // HOLD: a phone is on the AP
        force_ = false;
        startScan(/*manual=*/false);
        break;

      case State::Connecting:
        if (WiFi.status() == WL_CONNECTED) {
          state_ = State::Connected;
          backoffIdx_ = 0;
          failStreak_ = 0;
          lastError_ = "";
          IPAddress ip = WiFi.localIP();
          Serial.printf("[wifi] joined \"%s\" %u.%u.%u.%u\n", curSsid_.c_str(), ip[0], ip[1], ip[2], ip[3]);
        } else if (discCount_ != discAtAttempt_ || (now - connectStart_) > kConnectTimeoutMs) {
          lastError_ = reasonText(discCount_ != discAtAttempt_ ? lastReason_ : 0);
          Serial.printf("[wifi] \"%s\" failed: %s\n", curSsid_.c_str(), lastError_.c_str());
          WiFi.disconnect(false, false);
          tryNextCandidate();
        }
        break;

      case State::Connected:
        if (WiFi.status() != WL_CONNECTED) {
          Serial.printf("[wifi] lost \"%s\"\n", curSsid_.c_str());
          lastError_ = "link lost";
          curSsid_ = "";
          backoffIdx_ = 0;
          restoreApChannel();
          state_ = count_ ? State::Waiting : State::Idle;
          nextAt_ = now + kLostRetryMs;
        }
        break;
    }
  }

  // ---- UI actions (Arduino loop task, from HTTP handlers) --------------------------------------

  // Add or update a saved network. Empty password = open network. If the station isn't connected,
  // try to join now, overriding the AP-client hold (the user asked for it).
  // Returns "" on success, else a short error for the UI.
  String saveNetwork(const String &ssid, const String &pass) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!validSsid(ssid)) return "network name must be 1-32 bytes";
    if (!(pass.length() == 0 || (pass.length() >= 8 && pass.length() <= 64))) return "password must be empty (open) or 8-63 characters";
    int i = find(ssid);
    if (i < 0) {
      if (count_ >= kMaxSaved) return "already " + String(kMaxSaved) + " saved networks; forget one first";
      i = count_++;
    }
    ssids_[i] = ssid;
    passes_[i] = pass;
    persistSaved();
    if (state_ != State::Connected && state_ != State::Connecting) connectNow();
    return "";
  }

  bool forgetNetwork(const String &ssid) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    int i = find(ssid);
    if (i < 0) return false;
    for (int j = i; j < count_ - 1; ++j) { ssids_[j] = ssids_[j + 1]; passes_[j] = passes_[j + 1]; }
    count_--;
    ssids_[count_] = ""; passes_[count_] = "";
    persistSaved();
    if ((state_ == State::Connected || state_ == State::Connecting) && ssid == curSsid_) {
      WiFi.disconnect(false, false);
      curSsid_ = "";
      state_ = State::Waiting;
      nextAt_ = millis() + backoffMs();
    }
    if (count_ == 0 && state_ != State::Connected) state_ = State::Idle;
    return true;
  }

  // Scan + join the best saved network immediately, even with phones on the AP.
  void connectNow() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (count_ == 0 || state_ == State::Connecting) return;
    if (state_ == State::Connected) return;
    backoffIdx_ = 0;
    failStreak_ = 0;
    force_ = true;
    state_ = State::Waiting;
    nextAt_ = millis();
  }

  // User-requested scan (to pick a network). Results only; never auto-connects, so a phone
  // browsing networks on the AP is not kicked off. Returns false if busy/rate-limited.
  bool requestScan() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    uint32_t now = millis();
    if (scanning_ || state_ == State::Connecting) return false;
    if (lastManualScanAt_ && now - lastManualScanAt_ < kManualScanMinMs) return false;
    lastManualScanAt_ = now;
    return startScan(/*manual=*/true);
  }

  // New AP name/password. Applied ~1.5 s later so the HTTP response reaches the phone first;
  // every phone on the AP then has to rejoin with the new password.
  String setAp(const String &ssid, const String &pass) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!validApSsid(ssid)) return "device network name must be 1-32 bytes";
    if (!validApPass(pass)) return "device password must be 8-63 characters";
    apSsid_ = ssid; apPass_ = pass;
    prefs_.putString("apS", ssid);
    prefs_.putString("apP", pass);
    apRestartAt_ = millis() + 1500;
    return "";
  }

  // ---- read side ------------------------------------------------------------------------------

  bool authorized(const String &secret, const char *token) const {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (secret.length() == 0) return false;
    if (secret == apPass_) return true;
    return token && token[0] && secret == token;
  }

  void statusJson(String &o) const {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    uint32_t now = millis();
    o += "{\"ap\":{\"ssid\":"; jsonStr(o, apSsid_);
    o += ",\"ip\":\"" + WiFi.softAPIP().toString() + "\"";
    o += ",\"clients\":" + String(WiFi.softAPgetStationNum());
    o += ",\"publicDefaultPass\":"; o += (publicDefaultPass_ && apPass_ == publicDefaultPass_) ? "true" : "false";
    o += "},\"sta\":{\"state\":\"";
    o += scanning_ && state_ != State::Connected ? "scanning" : stateName(state_);
    o += "\",\"ssid\":"; jsonStr(o, state_ == State::Connected || state_ == State::Connecting ? curSsid_ : String(""));
    if (state_ == State::Connected) {
      o += ",\"ip\":\"" + WiFi.localIP().toString() + "\",\"rssi\":" + String(WiFi.RSSI());
    }
    bool held = state_ == State::Waiting && !scanning_ && !force_ && WiFi.softAPgetStationNum() > 0;
    o += ",\"held\":"; o += held ? "true" : "false";
    bool paused = state_ == State::Waiting && !scanning_ && failStreak_ >= kPauseAfter;
    o += ",\"paused\":"; o += paused ? "true" : "false";
    long wait = state_ == State::Waiting && !scanning_ ? ((long)nextAt_ - (long)now) / 1000 : 0;
    o += ",\"nextScanS\":" + String(wait < 0 ? 0 : wait);
    o += ",\"scans\":" + String(scans_) + ",\"attempts\":" + String(attempts_);
    o += ",\"lastError\":"; jsonStr(o, lastError_);
    o += "},\"saved\":[";
    for (int i = 0; i < count_; ++i) { if (i) o += ","; jsonStr(o, ssids_[i]); }
    o += "],\"maxSaved\":" + String(kMaxSaved) + "}";
  }

  void scanJson(String &o) const {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    o += "{\"state\":\"";
    o += scanning_ ? "running" : (resultCount_ || lastScanAt_ ? "done" : "idle");
    o += "\",\"ageS\":" + String(lastScanAt_ ? (millis() - lastScanAt_) / 1000 : 0);
    o += ",\"results\":[";
    for (int i = 0; i < resultCount_; ++i) {
      const ScanResult &r = results_[i];
      if (i) o += ",";
      o += "{\"ssid\":"; jsonStr(o, r.ssid);
      o += ",\"rssi\":" + String(r.rssi) + ",\"secure\":" + (r.secure ? "true" : "false");
      o += ",\"saved\":"; o += find(r.ssid) >= 0 ? "true" : "false";
      o += "}";
    }
    o += "]}";
  }

  const String &apSsid() const { return apSsid_; }
  const String &apPass() const { return apPass_; }
  bool apUp() const { return apUp_; }

  static void jsonStr(String &o, const String &s) {
    o += '"';
    for (size_t i = 0; i < s.length(); ++i) {
      uint8_t c = (uint8_t)s[i];
      if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
      else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
      else o += (char)c;
    }
    o += '"';
  }

 private:
  struct ScanResult { String ssid; int32_t rssi; uint8_t channel; uint8_t bssid[6]; bool secure; };
  struct Candidate { int saved; int32_t rssi; uint8_t channel; uint8_t bssid[6]; };

  static constexpr uint32_t kConnectTimeoutMs = 15000;
  static constexpr uint32_t kLostRetryMs = 3000;
  static constexpr uint32_t kManualScanMinMs = 5000;
  static constexpr uint32_t kScanDwellMs = 120;   // active dwell per channel (13 channels)
  // arduino-esp32 2.x gives an async scan only dwell*20 = 2.4 s before scanComplete() reports
  // WIFI_SCAN_FAILED. In AP+STA the driver returns to the AP's channel between scan channels, so a
  // real scan takes longer and was reported as "0 networks" (found on HW 2026-09-13). The framework
  // still finishes the scan and publishes results later, so we wait for them under our own deadline.
  static constexpr uint32_t kScanDeadlineMs = 10000;

  mutable std::recursive_mutex mu_;
  Preferences prefs_;
  const char *publicDefaultPass_ = nullptr;
  const char *apSsidDefault_ = "";
  const char *apPassDefault_ = "";
  String apSsid_, apPass_;
  bool apUp_ = false;
  uint32_t apRestartAt_ = 0;

  String ssids_[kMaxSaved], passes_[kMaxSaved];
  int count_ = 0;

  State state_ = State::Idle;
  uint32_t nextAt_ = 0;
  uint8_t backoffIdx_ = 0;
  uint8_t failStreak_ = 0;                 // failed scan/connect rounds in a row (see PAUSE)
  static constexpr uint8_t  kPauseAfter = 3;
  static constexpr uint32_t kBootGraceMs = 45000;
  static constexpr uint32_t kPauseMs    = 30UL * 60UL * 1000UL;
  bool force_ = false;
  String curSsid_;
  String lastError_;
  uint32_t connectStart_ = 0;
  uint32_t scans_ = 0, attempts_ = 0;

  volatile uint8_t lastReason_ = 0;
  volatile uint32_t discCount_ = 0;
  uint32_t discAtAttempt_ = 0;

  bool scanning_ = false, scanManual_ = false;
  uint32_t lastScanAt_ = 0, lastManualScanAt_ = 0, scanStartedAt_ = 0;
  ScanResult results_[kMaxScan];
  int resultCount_ = 0;

  Candidate cand_[kMaxSaved * 2];
  int candCount_ = 0, candIdx_ = 0;

  static bool validSsid(const String &s) { return s.length() >= 1 && s.length() <= 32; }
  static bool validApSsid(const String &s) { return validSsid(s); }
  static bool validApPass(const String &s) { return s.length() >= 8 && s.length() <= 63; }

  static const char *stateName(State s) {
    switch (s) {
      case State::Idle: return "idle";
      case State::Waiting: return "waiting";
      case State::Connecting: return "connecting";
      case State::Connected: return "connected";
    }
    return "?";
  }

  static String reasonText(uint8_t r) {
    switch (r) {
      case 0: return "timed out";
      case WIFI_REASON_NO_AP_FOUND: return "network not found";
      case WIFI_REASON_AUTH_FAIL:
      case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
      case WIFI_REASON_HANDSHAKE_TIMEOUT: return "wrong password";
      case WIFI_REASON_ASSOC_FAIL: return "association failed";
      default: return "disconnected (reason " + String(r) + ")";
    }
  }

  uint32_t backoffMs() const {
    static constexpr uint32_t steps[] = {10000, 20000, 40000, 80000, 160000, 300000};
    const int n = sizeof(steps) / sizeof(steps[0]);
    return steps[backoffIdx_ < n ? backoffIdx_ : n - 1];
  }

  int find(const String &ssid) const {
    for (int i = 0; i < count_; ++i) if (ssids_[i] == ssid) return i;
    return -1;
  }

  void loadSaved() {
    count_ = prefs_.getUChar("n", 0);
    if (count_ > kMaxSaved) count_ = 0;
    for (int i = 0; i < count_; ++i) {
      ssids_[i] = prefs_.getString(("s" + String(i)).c_str(), "");
      passes_[i] = prefs_.getString(("p" + String(i)).c_str(), "");
    }
  }

  void persistSaved() {
    for (int i = 0; i < kMaxSaved; ++i) {
      String sk = "s" + String(i), pk = "p" + String(i);
      if (i < count_) { prefs_.putString(sk.c_str(), ssids_[i]); prefs_.putString(pk.c_str(), passes_[i]); }
      else { prefs_.remove(sk.c_str()); prefs_.remove(pk.c_str()); }
    }
    prefs_.putUChar("n", (uint8_t)count_);
  }

  void startAp() {
    // Own channel (TDSP_AP_CHANNEL, default 1), not the softAP default: the home router here sits on
    // channel 6 and a neighbour on 11, and the AP had been dragged onto 6 by the LAN join attempts,
    // so every phone<->T-DSP packet contended with the house Wi-Fi. A LAN join still moves the AP to
    // the LAN's channel (one radio) -- that is the price of joining a LAN, and why the AP-first user
    // keeps no saved LAN networks.
    apUp_ = WiFi.softAP(apSsid_.c_str(), apPass_.c_str(), TDSP_AP_CHANNEL);
    if (!apUp_ && (apSsid_ != apSsidDefault_ || apPass_ != apPassDefault_)) {
      // Never leave the device unreachable: fall back to the build defaults.
      apSsid_ = apSsidDefault_; apPass_ = apPassDefault_;
      apUp_ = WiFi.softAP(apSsid_.c_str(), apPass_.c_str(), TDSP_AP_CHANNEL);
    }
    Serial.printf("[wifi] AP \"%s\" %s at %s ch%d\n", apSsid_.c_str(), apUp_ ? "up" : "FAILED", WiFi.softAPIP().toString().c_str(), (int)TDSP_AP_CHANNEL);
  }

  // One radio: a station attempt (and a scan) drags the softAP onto the target's channel, and the
  // AP STAYS there after the attempt fails -- so phones on T-DSP ended up sharing channel 6 with the
  // house router for good. After every failed round (and a lost link) put the AP back on its own
  // channel. Only called when no phone is on the AP (HOLD), except for a manual Connect now.
  void restoreApChannel() {
    // Unconditional: esp_wifi_get_channel() reported the AP's CONFIGURED channel (1) while the radio
    // (and the beacons a phone sees) were still on the station's channel 6 after a failed join --
    // hardware-observed 2026-10-01. Re-apply the AP config with its channel, which the driver honours
    // once the station is disconnected, and set the primary channel as well.
    uint8_t before = 0; wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&before, &sec);
    wifi_config_t cfg;
    esp_err_t e1 = esp_wifi_get_config(WIFI_IF_AP, &cfg);
    if (e1 == ESP_OK) { cfg.ap.channel = TDSP_AP_CHANNEL; e1 = esp_wifi_set_config(WIFI_IF_AP, &cfg); }
    esp_err_t e2 = esp_wifi_set_channel(TDSP_AP_CHANNEL, WIFI_SECOND_CHAN_NONE);
    uint8_t after = 0; esp_wifi_get_channel(&after, &sec);
    Serial.printf("[wifi] AP channel -> ch%d (reported ch%u -> ch%u; cfg %d, set %d)\n",
                  (int)TDSP_AP_CHANNEL, (unsigned)before, (unsigned)after, (int)e1, (int)e2);
  }

  bool startScan(bool manual) {
    int16_t r = WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false, /*passive=*/false, kScanDwellMs);
    if (r == WIFI_SCAN_FAILED) {
      if (!manual) { state_ = State::Waiting; nextAt_ = millis() + backoffMs(); backoffIdx_++; }
      return false;
    }
    scanning_ = true;
    scanManual_ = manual;
    scanStartedAt_ = millis();
    scans_++;
    return true;
  }

  void serviceScan() {
    int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;
    if (n == WIFI_SCAN_FAILED) {
      if (millis() - scanStartedAt_ < kScanDeadlineMs) return;   // framework timeout, not a real failure
      esp_wifi_scan_stop();                                        // truly stuck: clean up
    }
    scanning_ = false;
    lastScanAt_ = millis();
    resultCount_ = 0;
    for (int i = 0; i < n && resultCount_ < kMaxScan; ++i) {
      String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) continue;
      int dup = -1;
      for (int j = 0; j < resultCount_; ++j) if (results_[j].ssid == ssid) { dup = j; break; }
      int32_t rssi = WiFi.RSSI(i);
      if (dup >= 0 && results_[dup].rssi >= rssi) continue;   // keep the strongest BSSID per name
      ScanResult &r = dup >= 0 ? results_[dup] : results_[resultCount_++];
      r.ssid = ssid; r.rssi = rssi; r.channel = WiFi.channel(i);
      memcpy(r.bssid, WiFi.BSSID(i), 6);
      r.secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    }
    WiFi.scanDelete();
    // Sort strongest first for the UI.
    for (int i = 1; i < resultCount_; ++i)
      for (int j = i; j > 0 && results_[j].rssi > results_[j - 1].rssi; --j) std::swap(results_[j], results_[j - 1]);

    if (scanManual_) return;          // browsing only: never connect off a user scan
    if (state_ != State::Waiting) return;
    candCount_ = 0; candIdx_ = 0;
    for (int i = 0; i < resultCount_ && candCount_ < (int)(sizeof(cand_) / sizeof(cand_[0])); ++i) {
      int s = find(results_[i].ssid);
      if (s < 0) continue;
      Candidate &c = cand_[candCount_++];
      c.saved = s; c.rssi = results_[i].rssi; c.channel = results_[i].channel;
      memcpy(c.bssid, results_[i].bssid, 6);
    }
    if (candCount_ == 0) lastError_ = "no saved network in range";
    tryNextCandidate();
  }

  void tryNextCandidate() {
    if (candIdx_ < candCount_) {
      const Candidate &c = cand_[candIdx_++];
      curSsid_ = ssids_[c.saved];
      discAtAttempt_ = discCount_;
      connectStart_ = millis();
      attempts_++;
      const String &pw = passes_[c.saved];
      WiFi.begin(curSsid_.c_str(), pw.length() ? pw.c_str() : nullptr, c.channel, c.bssid, true);
      state_ = State::Connecting;
      Serial.printf("[wifi] joining \"%s\" ch%u %d dBm\n", curSsid_.c_str(), c.channel, (int)c.rssi);
      return;
    }
    curSsid_ = "";
    state_ = count_ ? State::Waiting : State::Idle;
    restoreApChannel();
    if (failStreak_ < 250) failStreak_++;
    if (failStreak_ >= kPauseAfter) {
      // Give the AP a steady radio: stop hammering a LAN that keeps refusing us. Retried in 30 min,
      // or at once from the UI (Connect now / Save).
      nextAt_ = millis() + kPauseMs;
      Serial.printf("[wifi] %u failed rounds in a row (%s): pausing LAN attempts for 30 min so the AP stays steady\n",
                    (unsigned)failStreak_, lastError_.c_str());
    } else {
      nextAt_ = millis() + backoffMs();
      if (backoffIdx_ < 250) backoffIdx_++;
    }
  }
};
