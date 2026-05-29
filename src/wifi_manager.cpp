#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>

#include "dev_wifi_config.h"
#include "provisioning_config.h"
#include "wifi_manager.h"

static WiFiManager wm;

namespace {
const char* setupApPasswordOrNull() {
  return ProvisioningConfig::SETUP_AP_PASSWORD[0] == '\0'
      ? nullptr
      : ProvisioningConfig::SETUP_AP_PASSWORD;
}

// Interval between non-blocking runtime reconnect attempts.
constexpr uint32_t RECONNECT_ATTEMPT_INTERVAL_MS = 15000;
}

void WifiManagerService::buildCandidateList(const AppConfig* config) {
  candidates_.clear();
  connectTimeoutMs_ = 15000;

  // Tier 1: user-managed saved networks, in priority order.
  if (config != nullptr) {
    connectTimeoutMs_ = config->wifi.connectTimeoutMs;
    for (const auto& network : config->wifi.savedNetworks) {
      if (network.ssid.isEmpty()) continue;
      Candidate candidate;
      candidate.ssid = network.ssid;
      candidate.password = network.password;
      candidate.fromSaved = true;
      candidates_.push_back(candidate);
    }
  }

  // Tier 2: built-in bench/dev networks (compile-time fallback). These are
  // never copied into the saved list; they exist so a wiped config still
  // joins the bench networks.
  if (DevWifiConfig::ENABLED) {
    for (size_t i = 0; i < DevWifiConfig::NETWORK_COUNT; ++i) {
      const auto& network = DevWifiConfig::NETWORKS[i];
      Candidate candidate;
      candidate.ssid = network.ssid;
      candidate.password = network.password;
      candidate.fromSaved = false;
      candidates_.push_back(candidate);
    }
  }
}

bool WifiManagerService::attemptCandidate(const Candidate& candidate) {
  Serial.print("Trying Wi-Fi SSID: ");
  Serial.print(candidate.ssid);
  Serial.println(candidate.fromSaved ? " (saved)" : " (built-in)");

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  if (candidate.password.isEmpty()) {
    WiFi.begin(candidate.ssid.c_str());
  } else {
    WiFi.begin(candidate.ssid.c_str(), candidate.password.c_str());
  }

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < connectTimeoutMs_) {
    delay(100);
    yield();
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected. IP: ");
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.print("Wi-Fi connect timed out for SSID: ");
  Serial.println(candidate.ssid);
  WiFi.disconnect();
  delay(250);
  return false;
}

bool WifiManagerService::walkCandidates() {
  if (candidates_.empty()) return false;

  // One boot-time scan so we only attempt SSIDs actually present, avoiding a
  // full per-network timeout on absent networks. The scan result is freed
  // immediately. If the scan fails we fall back to attempting every candidate.
  bool haveScan = false;
  int scanCount = WiFi.scanNetworks();
  std::vector<String> visibleSsids;
  if (scanCount > 0) {
    haveScan = true;
    for (int i = 0; i < scanCount; ++i) {
      visibleSsids.push_back(WiFi.SSID(i));
    }
  }
  WiFi.scanDelete();

  for (const auto& candidate : candidates_) {
    if (haveScan) {
      bool present = false;
      for (const auto& ssid : visibleSsids) {
        if (ssid == candidate.ssid) { present = true; break; }
      }
      if (!present) {
        Serial.print("Skipping absent SSID: ");
        Serial.println(candidate.ssid);
        continue;
      }
    }
    if (attemptCandidate(candidate)) return true;
  }
  return false;
}

bool WifiManagerService::startPortal(const String& apName, bool forcePortal,
                                     AppConfig* config) {
  setupApName_ = ProvisioningConfig::setupApName(ESP.getChipId());
  wm.setTitle("Rebooter Setup");
  wm.setConfigPortalTimeout(ProvisioningConfig::CONFIG_PORTAL_TIMEOUT_SECONDS);
  wm.setDebugOutput(false);
  wm.setHostname(apName.c_str());

  // Option A: keep tzapu for the AP/DHCP/DNS mechanics, but extend its portal
  // page with our own fields so the operator can seed a prioritized saved
  // network and a hub URL. Captured after the portal returns and written into
  // AppConfig, which is the single source of truth for boot ordering.
  WiFiManagerParameter savedSsidParam("rb_ssid", "Extra saved Wi-Fi SSID (optional)", "", 32);
  WiFiManagerParameter savedPassParam("rb_pass", "Extra saved Wi-Fi password", "", 64);
  WiFiManagerParameter hubUrlParam("rb_hub", "Hub URL (optional)", "", 192);
  if (config != nullptr) {
    wm.addParameter(&savedSsidParam);
    wm.addParameter(&savedPassParam);
    wm.addParameter(&hubUrlParam);
  }

  if (forcePortal) {
    WiFi.disconnect(false);
    delay(250);
  }
  Serial.println("Starting setup access point for Wi-Fi provisioning.");
  Serial.print("Setup SSID: ");
  Serial.println(setupApName_);
  if (ProvisioningConfig::SETUP_AP_PASSWORD[0] == '\0') {
    Serial.println("Setup network is open (no password).");
  } else {
    Serial.print("Setup password: ");
    Serial.println(ProvisioningConfig::SETUP_AP_PASSWORD);
  }
  Serial.println("Join the setup network and browse to 192.168.4.1 to configure Wi-Fi.");

  bool ok = forcePortal
      ? wm.startConfigPortal(setupApName_.c_str(), setupApPasswordOrNull())
      : wm.autoConnect(setupApName_.c_str(), setupApPasswordOrNull());
  captivePortal_ = !ok;
  provisionedViaPortal_ = forcePortal && ok;
  if (ok) {
    Serial.print("Provisioned Wi-Fi connected. IP: ");
    Serial.println(WiFi.localIP());
  }

  // Persist any operator-entered fields into AppConfig. The caller is
  // responsible for committing the config to flash.
  if (config != nullptr) {
    String extraSsid = String(savedSsidParam.getValue());
    extraSsid.trim();
    if (!extraSsid.isEmpty()) {
      bool exists = false;
      for (auto& network : config->wifi.savedNetworks) {
        if (network.ssid == extraSsid) {
          network.password = String(savedPassParam.getValue());
          exists = true;
          break;
        }
      }
      if (!exists && config->wifi.savedNetworks.size() < 5) {
        WifiNetwork network;
        network.ssid = extraSsid;
        network.password = String(savedPassParam.getValue());
        config->wifi.savedNetworks.push_back(network);
      }
    }

    String hubUrl = String(hubUrlParam.getValue());
    hubUrl.trim();
    if (!hubUrl.isEmpty()) {
      bool exists = false;
      for (const auto& url : config->central.baseUrls) {
        if (url == hubUrl) { exists = true; break; }
      }
      if (!exists && config->central.baseUrls.size() < HubDefaults::MAX_BASE_URLS) {
        config->central.baseUrls.push_back(hubUrl);
      }
    }
  }

  return ok;
}

bool WifiManagerService::begin(const String& apName, AppConfig* config,
                               bool forcePortal) {
  apName_ = apName;
  provisionedViaPortal_ = false;
  configChangedByPortal_ = false;
  state_ = State::Init;

  buildCandidateList(config);

  // Boot-time state machine: saved networks -> dev networks -> AP portal.
  if (!forcePortal) {
    state_ = State::Connecting;
    if (walkCandidates()) {
      captivePortal_ = false;
      setupApName_ = ProvisioningConfig::setupApName(ESP.getChipId());
      state_ = State::Connected;
      lastLinkOkMs_ = millis();
      return true;
    }
    Serial.println("All saved and built-in Wi-Fi attempts failed; falling back to portal.");
  }

  state_ = State::Portal;
  size_t savedBefore = config != nullptr ? config->wifi.savedNetworks.size() : 0;
  size_t urlsBefore = config != nullptr ? config->central.baseUrls.size() : 0;
  bool ok = startPortal(apName, forcePortal, config);
  if (config != nullptr &&
      (config->wifi.savedNetworks.size() != savedBefore ||
       config->central.baseUrls.size() != urlsBefore)) {
    configChangedByPortal_ = true;
  }
  if (ok) {
    state_ = State::Connected;
    lastLinkOkMs_ = millis();
  }
  return ok;
}

void WifiManagerService::loop() {
  // Runtime reconnect supervisor. SPECS 16.1 says do not reboot or drop to the
  // AP portal on a transient runtime Wi-Fi loss, so this only re-walks the
  // candidate list non-blockingly; it never enters the portal.
  if (state_ == State::Portal) return;

  const uint32_t now = millis();
  if (WiFi.status() == WL_CONNECTED) {
    lastLinkOkMs_ = now;
    if (state_ == State::Reconnecting) {
      Serial.println("Wi-Fi link recovered.");
    }
    state_ = State::Connected;
    return;
  }

  // Link is down. Wait one connect-timeout budget before treating it as a real
  // drop so a brief blip does not trigger reconnection churn.
  if (now - lastLinkOkMs_ < connectTimeoutMs_) return;

  state_ = State::Reconnecting;
  if (now < nextReconnectAttemptMs_) return;
  nextReconnectAttemptMs_ = now + RECONNECT_ATTEMPT_INTERVAL_MS;

  // Attempt one candidate per supervisor tick. attemptCandidate() blocks for at
  // most one connect-timeout budget; that is acceptable here because the device
  // has no Wi-Fi to serve anyway while disconnected.
  for (const auto& candidate : candidates_) {
    if (attemptCandidate(candidate)) {
      lastLinkOkMs_ = millis();
      state_ = State::Connected;
      return;
    }
  }
}

bool WifiManagerService::isConnected() const {
  return WiFi.status() == WL_CONNECTED;
}

String WifiManagerService::ipAddress() const {
  return WiFi.localIP().toString();
}

bool WifiManagerService::inCaptivePortal() const {
  return captivePortal_;
}

String WifiManagerService::setupApName() const {
  return setupApName_;
}

bool WifiManagerService::provisionedViaPortal() const {
  return provisionedViaPortal_;
}

bool WifiManagerService::configChangedByPortal() const {
  return configChangedByPortal_;
}

void WifiManagerService::clearProvisionedCredentials() {
  wm.resetSettings();
  WiFi.persistent(true);
  WiFi.disconnect(true);
  delay(250);
  captivePortal_ = false;
  provisionedViaPortal_ = false;
}

namespace {
// 0.2.8 (#154) periodic-scan tuning.
constexpr uint32_t PERIODIC_SCAN_HEAP_FLOOR = 18000;   // don't scan when tight
constexpr uint32_t PERIODIC_SCAN_TIMEOUT_MS = 12000;   // abort a stuck async scan
constexpr int PERIODIC_SCAN_TOP_N = 5;                 // strongest N reported
constexpr uint8_t PERIODIC_SCAN_SSID_MAX = 24;         // truncate long SSIDs
}

void WifiManagerService::loopPeriodicScan(const AppConfig* config, uint32_t freeHeap) {
  if (!config || !config->wifi.periodicScanEnabled) return;
  if (state_ != State::Connected) return;  // only scan while associated + stable
  const uint32_t now = millis();

  if (periodicScanInFlight_) {
    const int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
      if (now - periodicScanStartedMs_ > PERIODIC_SCAN_TIMEOUT_MS) {
        WiFi.scanDelete();
        periodicScanInFlight_ = false;
        periodicScanLastRunMs_ = now;  // back off a full interval after a dud
      }
      return;
    }
    // Scan finished (n >= 0) or failed (WIFI_SCAN_FAILED). Build a compact
    // top-N-by-RSSI summary; WiFi.scanNetworks already returns descending RSSI.
    if (n > 0) {
      String out = "[";
      const int limit = n < PERIODIC_SCAN_TOP_N ? n : PERIODIC_SCAN_TOP_N;
      for (int i = 0; i < limit; ++i) {
        if (i > 0) out += ",";
        String ssid = WiFi.SSID(i);
        if (ssid.length() > PERIODIC_SCAN_SSID_MAX) ssid = ssid.substring(0, PERIODIC_SCAN_SSID_MAX);
        ssid.replace("\\", "\\\\");
        ssid.replace("\"", "\\\"");
        out += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
      }
      out += "]";
      periodicScanSummary_ = out;
      periodicScanUptimeSeconds_ = now / 1000UL;
    }
    WiFi.scanDelete();
    periodicScanInFlight_ = false;
    periodicScanLastRunMs_ = now;
    return;
  }

  // Idle: time for the next scan? Respect the interval + a heap floor.
  const uint32_t intervalMs = config->wifi.periodicScanIntervalSeconds * 1000UL;
  const bool due = (periodicScanLastRunMs_ == 0) || (now - periodicScanLastRunMs_ >= intervalMs);
  if (!due) return;
  if (freeHeap < PERIODIC_SCAN_HEAP_FLOOR) {
    periodicScanLastRunMs_ = now;  // skip this slot, retry next interval
    return;
  }
  // Kick off the non-blocking scan (async=true, show_hidden=false).
  if (WiFi.scanNetworks(true, false) == WIFI_SCAN_RUNNING) {
    periodicScanInFlight_ = true;
    periodicScanStartedMs_ = now;
  } else {
    periodicScanLastRunMs_ = now;  // couldn't start; back off
  }
}

String WifiManagerService::scanNetworksJson() {
  String out = "[";
  int count = WiFi.scanNetworks();
  for (int i = 0; i < count; ++i) {
    if (i > 0) out += ",";
    String ssid = WiFi.SSID(i);
    ssid.replace("\\", "\\\\");
    ssid.replace("\"", "\\\"");
    out += "{\"ssid\":\"";
    out += ssid;
    out += "\",\"rssi\":";
    out += String(WiFi.RSSI(i));
    out += ",\"secure\":";
    out += (WiFi.encryptionType(i) == ENC_TYPE_NONE) ? "false" : "true";
    out += "}";
  }
  out += "]";
  WiFi.scanDelete();
  return out;
}
