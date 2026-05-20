#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>

#include "dev_wifi_config.h"
#include "wifi_manager.h"

static WiFiManager wm;

static bool tryConnect(const char* ssid, const char* password, uint32_t timeoutMs) {
  Serial.print("Trying Wi-Fi SSID: ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.persistent(true);
  WiFi.begin(ssid, password);

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(100);
    yield();
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected to ");
    Serial.print(ssid);
    Serial.print(". IP: ");
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.print("Failed to connect to ");
  Serial.println(ssid);
  WiFi.disconnect();
  return false;
}

static bool tryOpenNetworks() {
  if (!DevWifiConfig::OPEN_WIFI_FALLBACK) return false;

  Serial.println("Scanning for open networks...");
  int n = WiFi.scanNetworks();
  if (n <= 0) {
    Serial.println("No networks found.");
    return false;
  }

  for (int i = 0; i < n; i++) {
    if (WiFi.encryptionType(i) == ENC_TYPE_NONE &&
        WiFi.RSSI(i) >= DevWifiConfig::OPEN_WIFI_MIN_RSSI) {
      Serial.print("Trying open network: ");
      Serial.print(WiFi.SSID(i));
      Serial.print(" (RSSI ");
      Serial.print(WiFi.RSSI(i));
      Serial.println(")");

      if (tryConnect(WiFi.SSID(i).c_str(), nullptr, DevWifiConfig::CONNECT_TIMEOUT_MS)) {
        return true;
      }
    }
  }

  Serial.println("No usable open networks found.");
  return false;
}

static bool tryKnownNetworks() {
  if (!DevWifiConfig::ENABLED) return false;

  // Try WiFiManager's saved credentials first (from previous captive portal config)
  WiFi.mode(WIFI_STA);
  WiFi.persistent(true);
  WiFi.begin();

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < DevWifiConfig::CONNECT_TIMEOUT_MS) {
    delay(100);
    yield();
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected to saved Wi-Fi. IP: ");
    Serial.println(WiFi.localIP());
    return true;
  }
  WiFi.disconnect();

  // Try primary hardcoded SSID
  if (tryConnect(DevWifiConfig::SSID, DevWifiConfig::PASSWORD, DevWifiConfig::CONNECT_TIMEOUT_MS)) {
    return true;
  }

  // Try secondary hardcoded SSID
  if (tryConnect(DevWifiConfig::SSID2, DevWifiConfig::PASSWORD2, DevWifiConfig::CONNECT_TIMEOUT_MS)) {
    return true;
  }

  // Try any open network
  if (tryOpenNetworks()) {
    return true;
  }

  return false;
}

bool WifiManagerService::begin(const String& apName) {
  if (tryKnownNetworks()) {
    captivePortal_ = false;
    return true;
  }

  // All known networks failed — wait up to AP_FALLBACK_TIMEOUT_MS
  // retrying the known networks before opening the captive portal
  Serial.println("All known networks failed. Retrying before AP mode...");
  const uint32_t deadline = millis() + DevWifiConfig::AP_FALLBACK_TIMEOUT_MS;
  while (millis() < deadline) {
    if (tryKnownNetworks()) {
      captivePortal_ = false;
      return true;
    }
    delay(5000);
  }

  // Fall back to AP mode captive portal
  Serial.println("Entering AP mode for configuration.");
  wm.setConfigPortalTimeout(0);  // stay open until configured
  bool ok = wm.autoConnect(apName.c_str());
  captivePortal_ = !ok;
  return ok;
}

void WifiManagerService::loop() {
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
