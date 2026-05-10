#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266httpUpdate.h>
#include <WiFiClientSecureBearSSL.h>

#include "pins.h"
#include "bootstrap_config.h"

namespace {
enum class BootstrapState : uint8_t {
  Booting,
  ConnectingWifi,
  WifiConnected,
  Downloading,
  UpdateFailed,
  Idle
};

BootstrapState g_state = BootstrapState::Booting;
uint32_t g_lastBlinkAt = 0;
bool g_ledOn = false;
uint8_t g_updateAttempts = 0;
uint32_t g_nextWifiAttemptAt = 0;
uint32_t g_nextUpdateAttemptAt = 0;

void setLed(bool on) {
  g_ledOn = on;
  digitalWrite(Pins::LED, on ? LOW : HIGH);
}

void blinkLed(uint32_t intervalMs) {
  const uint32_t now = millis();
  if (now - g_lastBlinkAt >= intervalMs) {
    g_lastBlinkAt = now;
    setLed(!g_ledOn);
  }
}

void printBanner() {
  Serial.println();
  Serial.println("Rebooter bootstrap OTA loader");
  Serial.print("Version: ");
  Serial.println(BootstrapConfig::CURRENT_VERSION);
  Serial.println("Target URLs:");
  for (size_t i = 0; i < BootstrapConfig::FIRMWARE_URL_COUNT; ++i) {
    Serial.print("  ");
    Serial.print(i + 1);
    Serial.print(". ");
    Serial.println(BootstrapConfig::FIRMWARE_URLS[i]);
  }
}

void beginWifiConnection() {
  Serial.print("Connecting to SSID: ");
  Serial.println(BootstrapConfig::WIFI_SSID);

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.hostname("rebooter-bootstrap");
  WiFi.begin(BootstrapConfig::WIFI_SSID, BootstrapConfig::WIFI_PASSWORD);

  g_state = BootstrapState::ConnectingWifi;
}

bool waitForWifi() {
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < BootstrapConfig::WIFI_CONNECT_TIMEOUT_MS) {
    blinkLed(250);
    delay(25);
    yield();
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi connected. IP: ");
    Serial.println(WiFi.localIP());
    g_state = BootstrapState::WifiConnected;
    setLed(true);
    return true;
  }

  Serial.println("Wi-Fi connection timed out.");
  WiFi.disconnect();
  g_nextWifiAttemptAt = millis() + BootstrapConfig::WIFI_RETRY_DELAY_MS;
  g_state = BootstrapState::UpdateFailed;
  return false;
}

void configureUpdateCallbacks() {
  ESPhttpUpdate.onStart([]() {
    Serial.println("HTTP update started.");
    g_state = BootstrapState::Downloading;
  });

  ESPhttpUpdate.onEnd([]() {
    Serial.println("HTTP update finished.");
    setLed(true);
  });

  ESPhttpUpdate.onProgress([](int current, int total) {
    static uint32_t lastLogAt = 0;
    const uint32_t now = millis();
    if (now - lastLogAt < 1000 && current != total) return;
    lastLogAt = now;
    Serial.printf("Download progress: %d / %d bytes\n", current, total);
    blinkLed(100);
  });

  ESPhttpUpdate.onError([](int error) {
    Serial.printf("HTTP update error %d: %s\n", error, ESPhttpUpdate.getLastErrorString().c_str());
  });
}

void attemptHttpUpdate() {
  g_updateAttempts++;
  Serial.printf("Starting update attempt %u of %u\n", g_updateAttempts, BootstrapConfig::MAX_UPDATE_ATTEMPTS);
  ESPhttpUpdate.rebootOnUpdate(true);
  ESPhttpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  for (size_t i = 0; i < BootstrapConfig::FIRMWARE_URL_COUNT; ++i) {
    const String url = BootstrapConfig::FIRMWARE_URLS[i];
    std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure());
    client->setInsecure();

    Serial.print("Fetching firmware from: ");
    Serial.println(url);

    const t_httpUpdate_return result = ESPhttpUpdate.update(*client, url, BootstrapConfig::CURRENT_VERSION);

    switch (result) {
      case HTTP_UPDATE_FAILED:
        Serial.printf("Update failed from %s: %s\n", url.c_str(), ESPhttpUpdate.getLastErrorString().c_str());
        break;
      case HTTP_UPDATE_NO_UPDATES:
        Serial.println("Server reported no update. Bootstrap staying online.");
        g_state = BootstrapState::Idle;
        return;
      case HTTP_UPDATE_OK:
        Serial.println("Update installed. Device should reboot now.");
        g_state = BootstrapState::Idle;
        return;
    }
  }

  g_state = BootstrapState::UpdateFailed;
  g_nextUpdateAttemptAt = millis() + BootstrapConfig::UPDATE_RETRY_DELAY_MS;
}

void maybeRetry() {
  const uint32_t now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (now >= g_nextWifiAttemptAt) {
      beginWifiConnection();
      waitForWifi();
    }
    return;
  }

  if (g_updateAttempts >= BootstrapConfig::MAX_UPDATE_ATTEMPTS) {
    g_state = BootstrapState::Idle;
    return;
  }

  if (now >= g_nextUpdateAttemptAt) {
    attemptHttpUpdate();
  }
}
}

void setup() {
  pinMode(Pins::LED, OUTPUT);
  setLed(false);

  pinMode(Pins::RELAY, INPUT);
  pinMode(Pins::BUTTON, INPUT_PULLUP);

  Serial.begin(115200);
  delay(200);

  printBanner();
  configureUpdateCallbacks();
  beginWifiConnection();
  waitForWifi();

  if (WiFi.status() == WL_CONNECTED) {
    attemptHttpUpdate();
  }
}

void loop() {
  switch (g_state) {
    case BootstrapState::ConnectingWifi:
      blinkLed(250);
      break;
    case BootstrapState::WifiConnected:
      setLed(true);
      break;
    case BootstrapState::Downloading:
      blinkLed(100);
      break;
    case BootstrapState::UpdateFailed:
      blinkLed(700);
      break;
    case BootstrapState::Idle:
      blinkLed(1500);
      break;
    case BootstrapState::Booting:
    default:
      blinkLed(80);
      break;
  }

  maybeRetry();
  delay(10);
}
