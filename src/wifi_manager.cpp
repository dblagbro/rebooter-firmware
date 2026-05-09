#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>

#include "dev_wifi_config.h"
#include "wifi_manager.h"

static WiFiManager wm;

static bool tryDevWifi() {
  if (!DevWifiConfig::ENABLED) return false;

  Serial.print("Trying dev Wi-Fi SSID: ");
  Serial.println(DevWifiConfig::SSID);

  WiFi.mode(WIFI_STA);
  WiFi.persistent(true);
  WiFi.begin(DevWifiConfig::SSID, DevWifiConfig::PASSWORD);

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < DevWifiConfig::CONNECT_TIMEOUT_MS) {
    delay(100);
    yield();
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected to dev Wi-Fi. IP: ");
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.println("Dev Wi-Fi connect timed out, falling back to captive portal.");
  WiFi.disconnect();
  return false;
}

bool WifiManagerService::begin(const String& apName) {
  if (tryDevWifi()) {
    captivePortal_ = false;
    return true;
  }

  wm.setConfigPortalTimeout(180);
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
