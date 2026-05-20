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

String firmwareUrl(bool secondary = false) {
  const char* base = secondary ? BootstrapConfig::FIRMWARE_BASE_URL2 : BootstrapConfig::FIRMWARE_BASE_URL;
  return String(base) + BootstrapConfig::FIRMWARE_FILENAME;
}

void printBanner() {
  Serial.println();
  Serial.println("Rebooter bootstrap OTA loader");
  Serial.print("Version: ");
  Serial.println(BootstrapConfig::CURRENT_VERSION);
  Serial.print("Primary URL: ");
  Serial.println(firmwareUrl(false));
  Serial.print("Fallback URL: ");
  Serial.println(firmwareUrl(true));
}

bool tryConnect(const char* ssid, const char* password) {
  Serial.print("Connecting to SSID: ");
  Serial.println(ssid);

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.hostname("rebooter-bootstrap");

  if (password && password[0] != '\0') {
    WiFi.begin(ssid, password);
  } else {
    WiFi.begin(ssid);
  }

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < BootstrapConfig::WIFI_CONNECT_TIMEOUT_MS) {
    blinkLed(250);
    delay(25);
    yield();
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi connected to ");
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

bool tryOpenNetworks() {
  if (!BootstrapConfig::OPEN_WIFI_FALLBACK) return false;

  Serial.println("Scanning for open networks...");
  int n = WiFi.scanNetworks();
  if (n <= 0) {
    Serial.println("No networks found.");
    return false;
  }

  // Collect open networks sorted by signal strength (scanNetworks returns sorted by RSSI)
  for (int i = 0; i < n; i++) {
    if (WiFi.encryptionType(i) == ENC_TYPE_NONE &&
        WiFi.RSSI(i) >= BootstrapConfig::OPEN_WIFI_MIN_RSSI) {
      Serial.print("Trying open network: ");
      Serial.print(WiFi.SSID(i));
      Serial.print(" (RSSI ");
      Serial.print(WiFi.RSSI(i));
      Serial.println(")");

      if (tryConnect(WiFi.SSID(i).c_str(), nullptr)) {
        return true;
      }
    }
  }

  Serial.println("No usable open networks found.");
  return false;
}

void beginWifiConnection() {
  g_state = BootstrapState::ConnectingWifi;

  // Try primary SSID
  if (tryConnect(BootstrapConfig::WIFI_SSID, BootstrapConfig::WIFI_PASSWORD)) {
    g_state = BootstrapState::WifiConnected;
    setLed(true);
    return;
  }

  // Try secondary SSID
  if (tryConnect(BootstrapConfig::WIFI_SSID2, BootstrapConfig::WIFI_PASSWORD2)) {
    g_state = BootstrapState::WifiConnected;
    setLed(true);
    return;
  }

  // Try any open network
  if (tryOpenNetworks()) {
    g_state = BootstrapState::WifiConnected;
    setLed(true);
    return;
  }

  g_nextWifiAttemptAt = millis() + BootstrapConfig::WIFI_RETRY_DELAY_MS;
  g_state = BootstrapState::UpdateFailed;
}

// kept for the retry path in maybeRetry()
bool waitForWifi() {
  beginWifiConnection();
  return (WiFi.status() == WL_CONNECTED);
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

t_httpUpdate_return tryFirmwareUrl(const String& url) {
  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure());
  client->setInsecure();

  ESPhttpUpdate.rebootOnUpdate(true);
  ESPhttpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  Serial.print("Fetching firmware from: ");
  Serial.println(url);

  return ESPhttpUpdate.update(*client, url, BootstrapConfig::CURRENT_VERSION);
}

void attemptHttpUpdate() {
  g_updateAttempts++;
  Serial.printf("Starting update attempt %u of %u\n", g_updateAttempts, BootstrapConfig::MAX_UPDATE_ATTEMPTS);

  t_httpUpdate_return result = tryFirmwareUrl(firmwareUrl(false));

  if (result == HTTP_UPDATE_FAILED) {
    Serial.printf("Primary URL failed: %s\n", ESPhttpUpdate.getLastErrorString().c_str());
    Serial.println("Trying secondary URL...");
    result = tryFirmwareUrl(firmwareUrl(true));
  }

  switch (result) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("Update failed: %s\n", ESPhttpUpdate.getLastErrorString().c_str());
      g_state = BootstrapState::UpdateFailed;
      g_nextUpdateAttemptAt = millis() + BootstrapConfig::UPDATE_RETRY_DELAY_MS;
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("Server reported no update. Bootstrap staying online.");
      g_state = BootstrapState::Idle;
      break;
    case HTTP_UPDATE_OK:
      Serial.println("Update installed. Device should reboot now.");
      g_state = BootstrapState::Idle;
      break;
  }
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

  if (g_state == BootstrapState::WifiConnected) {
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
