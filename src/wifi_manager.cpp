#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include "wifi_manager.h"

static WiFiManager wm;

bool WifiManagerService::begin(const String& apName) {
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

