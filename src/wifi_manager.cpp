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
}

static bool tryDevWifi() {
  if (!DevWifiConfig::ENABLED) return false;

  for (size_t i = 0; i < DevWifiConfig::NETWORK_COUNT; ++i) {
    const auto& network = DevWifiConfig::NETWORKS[i];

    Serial.print("Trying dev Wi-Fi SSID: ");
    Serial.println(network.ssid);

    WiFi.mode(WIFI_STA);
    WiFi.persistent(true);
    WiFi.begin(network.ssid, network.password);

    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - start < DevWifiConfig::CONNECT_TIMEOUT_MS) {
      delay(100);
      yield();
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("Connected to dev Wi-Fi. IP: ");
      Serial.println(WiFi.localIP());
      return true;
    }

    Serial.print("Dev Wi-Fi connect timed out for SSID: ");
    Serial.println(network.ssid);
    WiFi.disconnect();
    delay(250);
  }

  Serial.println("All dev Wi-Fi attempts timed out, falling back to captive portal.");
  return false;
}

bool WifiManagerService::begin(const String& apName, bool forcePortal) {
  if (!forcePortal && tryDevWifi()) {
    captivePortal_ = false;
    setupApName_ = ProvisioningConfig::setupApName(ESP.getChipId());
    return true;
  }

  setupApName_ = ProvisioningConfig::setupApName(ESP.getChipId());
  wm.setTitle("Rebooter Setup");
  wm.setConfigPortalTimeout(ProvisioningConfig::CONFIG_PORTAL_TIMEOUT_SECONDS);
  wm.setDebugOutput(false);
  wm.setHostname(apName.c_str());
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
  if (ok) {
    Serial.print("Provisioned Wi-Fi connected. IP: ");
    Serial.println(WiFi.localIP());
  }
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

String WifiManagerService::setupApName() const {
  return setupApName_;
}
