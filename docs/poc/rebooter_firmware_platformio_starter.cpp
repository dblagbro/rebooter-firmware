// Rebooter Firmware - PlatformIO Starter Project
// Target: Sonoff S31 / ESP8266
//
// Project layout to create locally:
// .
// ├── platformio.ini
// ├── include/
// │   ├── config.h
// │   ├── pins.h
// │   ├── types.h
// │   ├── app_state.h
// │   ├── config_manager.h
// │   ├── relay_controller.h
// │   ├── led_manager.h
// │   ├── button_handler.h
// │   ├── wifi_manager.h
// │   ├── monitor_engine.h
// │   ├── notification_manager.h
// │   ├── web_server_manager.h
// │   └── event_log.h
// ├── src/
// │   ├── main.cpp
// │   ├── config_manager.cpp
// │   ├── relay_controller.cpp
// │   ├── led_manager.cpp
// │   ├── button_handler.cpp
// │   ├── wifi_manager.cpp
// │   ├── monitor_engine.cpp
// │   ├── notification_manager.cpp
// │   ├── web_server_manager.cpp
// │   └── event_log.cpp
// └── data/
//     ├── index.html
//     ├── app.js
//     └── style.css
//
// ============================== platformio.ini ==============================
/*
[env:sonoff_s31]
platform = platformio/espressif8266
board = esp12e
framework = arduino
monitor_speed = 115200
upload_speed = 460800
board_build.filesystem = littlefs

lib_deps =
  bblanchon/ArduinoJson@^7.0.4
  tzapu/WiFiManager@^2.0.17
  Links2004/WebSockets@^2.6.1

build_flags =
  -D PIO_FRAMEWORK_ARDUINO_LWIP2_LOW_MEMORY
  -D NDEBUG
*/

// NOTE:
// - Using LittleFS is recommended for ESP8266 web assets and persisted JSON. The ESP8266 core docs support LittleFS and OTA update patterns. ([arduino-esp8266.readthedocs.io](https://arduino-esp8266.readthedocs.io/en/latest/filesystem.html?utm_source=chatgpt.com))
// - PlatformIO has current official support for the Espressif 8266 platform and esp12e board target. ([docs.platformio.org](https://docs.platformio.org/en/latest/platforms/espressif8266.html?utm_source=chatgpt.com))
// - I am intentionally NOT using ESPAsyncWebServer here because the widely used repository was archived in January 2025. A synchronous server is the safer starting point. ([github.com](https://github.com/me-no-dev/espasyncwebserver?utm_source=chatgpt.com))

// ============================== include/pins.h ==============================
#pragma once

namespace Pins {
  // TODO: confirm against your exact Sonoff S31 revision before first live test.
  static constexpr uint8_t RELAY = 12;
  static constexpr uint8_t LED = 13;
  static constexpr uint8_t BUTTON = 0;
}

// ============================== include/types.h =============================
#pragma once

#include <Arduino.h>
#include <vector>

enum class DeviceMode : uint8_t {
  SmartPlug = 0,
  InternetWatchdog = 1,
  DeviceWatchdog = 2
};

enum class RelayRestoreBehavior : uint8_t {
  RestorePrevious = 0,
  AlwaysOn = 1,
  AlwaysOff = 2
};

enum class HealthState : uint8_t {
  Unknown = 0,
  Healthy = 1,
  PartialFailure = 2,
  Failed = 3,
  Holdoff = 4,
  Cooldown = 5
};

struct InternetWatchdogConfig {
  std::vector<String> targets;
  uint32_t failureThresholdSeconds = 180;
  uint32_t powerOffSeconds = 5;
  uint32_t postRebootHoldoffSeconds = 180;
  uint32_t maxCyclesPerIncident = 3;
  uint32_t maxCyclesPerHour = 6;
  uint32_t cooldownSeconds = 3600;
  uint32_t dnsRefreshSeconds = 300;
  uint32_t recoveryStabilitySeconds = 15;
};

struct DeviceWatchdogConfig {
  String target = "";
  uint32_t failureThresholdSeconds = 60;
  uint32_t powerOffSeconds = 5;
  uint32_t postRebootHoldoffSeconds = 300;
  uint32_t maxCyclesPerIncident = 3;
  uint32_t maxCyclesPerHour = 6;
  uint32_t cooldownSeconds = 3600;
  uint32_t recoveryStabilitySeconds = 30;
};

struct NotificationConfig {
  bool enabled = false;
  String type = "webhook";
  String webhookUrl = "";
  String webhookAuthToken = "";
  bool sendOnTrigger = true;
  bool sendOnRecovery = true;
  bool sendOnMaxCyclesReached = true;
};

struct AppConfig {
  String deviceName = "Rebooter";
  String adminUsername = "admin";
  String adminPasswordHash = "";
  String timezone = "America/New_York";
  DeviceMode currentMode = DeviceMode::SmartPlug;
  RelayRestoreBehavior relayRestoreBehavior = RelayRestoreBehavior::RestorePrevious;
  bool statusLedEnabled = true;
  uint16_t eventLogMaxEntries = 200;
  uint32_t monitorIntervalSeconds = 5;
  bool manualButtonEnabled = true;
  InternetWatchdogConfig internet;
  DeviceWatchdogConfig device;
  NotificationConfig notifications;
};

// ============================== include/app_state.h =========================
#pragma once

#include "types.h"

struct RuntimeStatus {
  bool wifiConnected = false;
  bool relayOn = true;
  bool inCaptivePortal = false;
  bool inHoldoff = false;
  bool inCooldown = false;
  HealthState healthState = HealthState::Unknown;
  String lastEvent = "boot";
  uint32_t uptimeSeconds = 0;
  uint32_t currentIncidentCycles = 0;
  uint32_t currentHourCycles = 0;
  uint32_t holdoffRemainingSeconds = 0;
  uint32_t cooldownRemainingSeconds = 0;
};

// ============================== include/config_manager.h ====================
#pragma once

#include "types.h"

class ConfigManager {
public:
  bool begin();
  bool load(AppConfig& out);
  bool save(const AppConfig& config);
  bool reset();
private:
  const char* configPath_ = "/config.json";
};

// ============================== include/relay_controller.h ==================
#pragma once

class RelayController {
public:
  void begin();
  void set(bool on);
  void toggle();
  bool isOn() const;
private:
  bool relayOn_ = true;
};

// ============================== include/led_manager.h =======================
#pragma once

enum class LedPattern : uint8_t {
  Off,
  Solid,
  SlowBlink,
  FastBlink,
  DoubleBlink
};

class LedManager {
public:
  void begin();
  void setPattern(LedPattern pattern);
  void loop();
private:
  LedPattern pattern_ = LedPattern::Off;
  uint32_t lastTick_ = 0;
  bool state_ = false;
};

// ============================== include/button_handler.h ====================
#pragma once

class ButtonHandler {
public:
  void begin();
  void loop();
  bool shortPressed();
  bool longPressed5s();
  bool longPressed10s();
private:
  bool shortPressed_ = false;
  bool longPressed5s_ = false;
  bool longPressed10s_ = false;
  bool wasDown_ = false;
  uint32_t downStart_ = 0;
};

// ============================== include/wifi_manager.h ======================
#pragma once

#include <functional>

class WifiManagerService {
public:
  bool begin(const String& apName);
  void loop();
  bool isConnected() const;
  String ipAddress() const;
  bool inCaptivePortal() const;
private:
  bool captivePortal_ = false;
};

// ============================== include/monitor_engine.h ====================
#pragma once

#include "types.h"
#include "app_state.h"

class RelayController;
class NotificationManager;
class EventLog;

class MonitorEngine {
public:
  void begin(AppConfig* config, RuntimeStatus* status,
             RelayController* relay, NotificationManager* notifier,
             EventLog* eventLog);
  void loop();
  void resetIncident();
private:
  bool checkInternetTargets();
  bool checkSingleTarget(const String& target);
  void runSmartPlugMode();
  void runInternetWatchdogMode();
  void runDeviceWatchdogMode();
  void triggerPowerCycle(uint32_t powerOffSeconds, uint32_t holdoffSeconds, const String& reason);

  AppConfig* config_ = nullptr;
  RuntimeStatus* status_ = nullptr;
  RelayController* relay_ = nullptr;
  NotificationManager* notifier_ = nullptr;
  EventLog* eventLog_ = nullptr;

  uint32_t lastMonitorMs_ = 0;
  uint32_t failureStartMs_ = 0;
  uint32_t holdoffStartMs_ = 0;
  uint32_t cooldownStartMs_ = 0;
  uint32_t powerOffStartMs_ = 0;
  bool powerCycleActive_ = false;
  bool relayPowerOffIssued_ = false;
};

// ============================== include/notification_manager.h ==============
#pragma once

#include "types.h"

class NotificationManager {
public:
  void begin(AppConfig* config);
  bool send(const String& eventType, const String& reason, const String& detailsJson);
private:
  AppConfig* config_ = nullptr;
};

// ============================== include/event_log.h =========================
#pragma once

#include <vector>

struct EventEntry {
  uint32_t ts;
  String type;
  String message;
};

class EventLog {
public:
  void begin(uint16_t maxEntries);
  void add(const String& type, const String& message);
  String asJson() const;
private:
  uint16_t maxEntries_ = 200;
  std::vector<EventEntry> items_;
};

// ============================== include/web_server_manager.h ================
#pragma once

#include "types.h"
#include "app_state.h"

class RelayController;
class ConfigManager;
class EventLog;
class MonitorEngine;

class WebServerManager {
public:
  void begin(AppConfig* config, RuntimeStatus* status,
             RelayController* relay, ConfigManager* cfgMgr,
             EventLog* eventLog, MonitorEngine* monitor);
  void loop();
private:
  AppConfig* config_ = nullptr;
  RuntimeStatus* status_ = nullptr;
};

// ============================== src/main.cpp ================================
#include <Arduino.h>
#include <LittleFS.h>

#include "pins.h"
#include "types.h"
#include "app_state.h"
#include "config_manager.h"
#include "relay_controller.h"
#include "led_manager.h"
#include "button_handler.h"
#include "wifi_manager.h"
#include "monitor_engine.h"
#include "notification_manager.h"
#include "web_server_manager.h"
#include "event_log.h"

AppConfig g_config;
RuntimeStatus g_status;
ConfigManager g_cfgMgr;
RelayController g_relay;
LedManager g_led;
ButtonHandler g_button;
WifiManagerService g_wifi;
NotificationManager g_notifier;
EventLog g_eventLog;
MonitorEngine g_monitor;
WebServerManager g_web;

void setup() {
  Serial.begin(115200);
  delay(200);

  LittleFS.begin();
  g_cfgMgr.begin();
  g_cfgMgr.load(g_config);

  g_eventLog.begin(g_config.eventLogMaxEntries);
  g_eventLog.add("boot", "Device booting");

  g_relay.begin();
  g_led.begin();
  g_button.begin();

  g_wifi.begin(g_config.deviceName);
  g_notifier.begin(&g_config);
  g_monitor.begin(&g_config, &g_status, &g_relay, &g_notifier, &g_eventLog);
  g_web.begin(&g_config, &g_status, &g_relay, &g_cfgMgr, &g_eventLog, &g_monitor);

  g_led.setPattern(LedPattern::SlowBlink);
}

void loop() {
  g_wifi.loop();
  g_button.loop();
  g_led.loop();
  g_web.loop();
  g_monitor.loop();

  g_status.uptimeSeconds = millis() / 1000;

  if (g_button.shortPressed() && g_config.currentMode == DeviceMode::SmartPlug && g_config.manualButtonEnabled) {
    g_relay.toggle();
    g_eventLog.add("relay", g_relay.isOn() ? "Relay turned on by button" : "Relay turned off by button");
  }

  if (g_button.longPressed10s()) {
    g_eventLog.add("system", "Factory reset requested by button");
    g_cfgMgr.reset();
    ESP.restart();
  }
}

// ============================== src/config_manager.cpp ======================
#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "config_manager.h"

bool ConfigManager::begin() {
  return true;
}

bool ConfigManager::load(AppConfig& out) {
  if (!LittleFS.exists(configPath_)) {
    out.internet.targets = {"1.1.1.1", "8.8.8.8", "time.nist.gov"};
    return save(out);
  }

  File f = LittleFS.open(configPath_, "r");
  if (!f) return false;

  JsonDocument doc;
  if (deserializeJson(doc, f) != DeserializationError::Ok) {
    f.close();
    return false;
  }
  f.close();

  out.deviceName = doc["device_name"] | out.deviceName;
  out.timezone = doc["timezone"] | out.timezone;
  out.monitorIntervalSeconds = doc["monitor_interval_seconds"] | out.monitorIntervalSeconds;
  out.manualButtonEnabled = doc["manual_button_enabled"] | out.manualButtonEnabled;

  JsonArray targets = doc["internet"]["targets"].as<JsonArray>();
  out.internet.targets.clear();
  for (JsonVariant v : targets) out.internet.targets.push_back(String((const char*)v));
  if (out.internet.targets.empty()) out.internet.targets = {"1.1.1.1", "8.8.8.8", "time.nist.gov"};

  out.internet.failureThresholdSeconds = doc["internet"]["failure_threshold_seconds"] | out.internet.failureThresholdSeconds;
  out.internet.powerOffSeconds = doc["internet"]["power_off_seconds"] | out.internet.powerOffSeconds;
  out.internet.postRebootHoldoffSeconds = doc["internet"]["post_reboot_holdoff_seconds"] | out.internet.postRebootHoldoffSeconds;
  out.device.target = doc["device"]["target"] | out.device.target;
  out.device.failureThresholdSeconds = doc["device"]["failure_threshold_seconds"] | out.device.failureThresholdSeconds;
  out.device.powerOffSeconds = doc["device"]["power_off_seconds"] | out.device.powerOffSeconds;
  out.device.postRebootHoldoffSeconds = doc["device"]["post_reboot_holdoff_seconds"] | out.device.postRebootHoldoffSeconds;
  out.notifications.enabled = doc["notifications"]["enabled"] | out.notifications.enabled;
  out.notifications.type = doc["notifications"]["type"] | out.notifications.type;
  out.notifications.webhookUrl = doc["notifications"]["webhook_url"] | out.notifications.webhookUrl;

  const String mode = doc["current_mode"] | "smart_plug";
  if (mode == "internet_watchdog") out.currentMode = DeviceMode::InternetWatchdog;
  else if (mode == "device_watchdog") out.currentMode = DeviceMode::DeviceWatchdog;
  else out.currentMode = DeviceMode::SmartPlug;

  return true;
}

bool ConfigManager::save(const AppConfig& config) {
  JsonDocument doc;
  doc["device_name"] = config.deviceName;
  doc["timezone"] = config.timezone;
  doc["monitor_interval_seconds"] = config.monitorIntervalSeconds;
  doc["manual_button_enabled"] = config.manualButtonEnabled;
  doc["current_mode"] =
    config.currentMode == DeviceMode::InternetWatchdog ? "internet_watchdog" :
    config.currentMode == DeviceMode::DeviceWatchdog ? "device_watchdog" : "smart_plug";

  JsonArray targets = doc["internet"]["targets"].to<JsonArray>();
  for (const auto& t : config.internet.targets) targets.add(t);
  doc["internet"]["failure_threshold_seconds"] = config.internet.failureThresholdSeconds;
  doc["internet"]["power_off_seconds"] = config.internet.powerOffSeconds;
  doc["internet"]["post_reboot_holdoff_seconds"] = config.internet.postRebootHoldoffSeconds;

  doc["device"]["target"] = config.device.target;
  doc["device"]["failure_threshold_seconds"] = config.device.failureThresholdSeconds;
  doc["device"]["power_off_seconds"] = config.device.powerOffSeconds;
  doc["device"]["post_reboot_holdoff_seconds"] = config.device.postRebootHoldoffSeconds;

  doc["notifications"]["enabled"] = config.notifications.enabled;
  doc["notifications"]["type"] = config.notifications.type;
  doc["notifications"]["webhook_url"] = config.notifications.webhookUrl;

  File f = LittleFS.open(configPath_, "w");
  if (!f) return false;
  serializeJsonPretty(doc, f);
  f.close();
  return true;
}

bool ConfigManager::reset() {
  if (LittleFS.exists(configPath_)) {
    LittleFS.remove(configPath_);
  }
  return true;
}

// ============================== src/relay_controller.cpp ====================
#include <Arduino.h>
#include "pins.h"
#include "relay_controller.h"

void RelayController::begin() {
  pinMode(Pins::RELAY, OUTPUT);
  set(true);
}

void RelayController::set(bool on) {
  relayOn_ = on;
  digitalWrite(Pins::RELAY, on ? HIGH : LOW);
}

void RelayController::toggle() {
  set(!relayOn_);
}

bool RelayController::isOn() const {
  return relayOn_;
}

// ============================== src/led_manager.cpp =========================
#include <Arduino.h>
#include "pins.h"
#include "led_manager.h"

void LedManager::begin() {
  pinMode(Pins::LED, OUTPUT);
  digitalWrite(Pins::LED, HIGH);
}

void LedManager::setPattern(LedPattern pattern) {
  pattern_ = pattern;
}

void LedManager::loop() {
  const uint32_t now = millis();
  uint32_t interval = 0;

  switch (pattern_) {
    case LedPattern::Off:
      digitalWrite(Pins::LED, HIGH);
      return;
    case LedPattern::Solid:
      digitalWrite(Pins::LED, LOW);
      return;
    case LedPattern::SlowBlink:
      interval = 800; break;
    case LedPattern::FastBlink:
      interval = 150; break;
    case LedPattern::DoubleBlink:
      interval = 100; break;
  }

  if (now - lastTick_ >= interval) {
    lastTick_ = now;
    state_ = !state_;
    digitalWrite(Pins::LED, state_ ? LOW : HIGH);
  }
}

// ============================== src/button_handler.cpp ======================
#include <Arduino.h>
#include "pins.h"
#include "button_handler.h"

void ButtonHandler::begin() {
  pinMode(Pins::BUTTON, INPUT_PULLUP);
}

void ButtonHandler::loop() {
  shortPressed_ = false;
  longPressed5s_ = false;
  longPressed10s_ = false;

  bool down = digitalRead(Pins::BUTTON) == LOW;
  const uint32_t now = millis();

  if (down && !wasDown_) {
    wasDown_ = true;
    downStart_ = now;
  } else if (!down && wasDown_) {
    uint32_t held = now - downStart_;
    wasDown_ = false;
    if (held >= 10000) longPressed10s_ = true;
    else if (held >= 5000) longPressed5s_ = true;
    else if (held >= 50) shortPressed_ = true;
  }
}

bool ButtonHandler::shortPressed() { return shortPressed_; }
bool ButtonHandler::longPressed5s() { return longPressed5s_; }
bool ButtonHandler::longPressed10s() { return longPressed10s_; }

// ============================== src/wifi_manager.cpp ========================
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

// ============================== src/monitor_engine.cpp ======================
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266Ping.h>
#include "monitor_engine.h"
#include "relay_controller.h"
#include "notification_manager.h"
#include "event_log.h"

void MonitorEngine::begin(AppConfig* config, RuntimeStatus* status,
                          RelayController* relay, NotificationManager* notifier,
                          EventLog* eventLog) {
  config_ = config;
  status_ = status;
  relay_ = relay;
  notifier_ = notifier;
  eventLog_ = eventLog;
}

void MonitorEngine::loop() {
  if (!config_ || !status_ || !relay_) return;

  const uint32_t now = millis();

  if (powerCycleActive_) {
    if (!relayPowerOffIssued_) {
      relay_->set(false);
      relayPowerOffIssued_ = true;
      powerOffStartMs_ = now;
      status_->relayOn = false;
      eventLog_->add("recovery", "Relay turned off for power cycle");
    } else if (now - powerOffStartMs_ >= (config_->currentMode == DeviceMode::InternetWatchdog ? config_->internet.powerOffSeconds : config_->device.powerOffSeconds) * 1000UL) {
      relay_->set(true);
      status_->relayOn = true;
      status_->inHoldoff = true;
      holdoffStartMs_ = now;
      powerCycleActive_ = false;
      relayPowerOffIssued_ = false;
      eventLog_->add("recovery", "Relay turned back on, entering holdoff");
    }
    return;
  }

  if (status_->inHoldoff) {
    const uint32_t holdoff = config_->currentMode == DeviceMode::InternetWatchdog ?
      config_->internet.postRebootHoldoffSeconds : config_->device.postRebootHoldoffSeconds;
    if (now - holdoffStartMs_ < holdoff * 1000UL) {
      status_->holdoffRemainingSeconds = holdoff - ((now - holdoffStartMs_) / 1000UL);
      return;
    }
    status_->inHoldoff = false;
    status_->holdoffRemainingSeconds = 0;
    failureStartMs_ = 0;
  }

  if (now - lastMonitorMs_ < config_->monitorIntervalSeconds * 1000UL) return;
  lastMonitorMs_ = now;

  switch (config_->currentMode) {
    case DeviceMode::SmartPlug:
      runSmartPlugMode();
      break;
    case DeviceMode::InternetWatchdog:
      runInternetWatchdogMode();
      break;
    case DeviceMode::DeviceWatchdog:
      runDeviceWatchdogMode();
      break;
  }
}

void MonitorEngine::resetIncident() {
  failureStartMs_ = 0;
  status_->currentIncidentCycles = 0;
}

bool MonitorEngine::checkInternetTargets() {
  bool anySuccess = false;
  for (const auto& target : config_->internet.targets) {
    IPAddress ip;
    if (ip.fromString(target)) {
      if (Ping.ping(ip, 1)) anySuccess = true;
    } else {
      IPAddress resolved;
      if (WiFi.hostByName(target.c_str(), resolved) && Ping.ping(resolved, 1)) anySuccess = true;
    }
    if (anySuccess) break;
  }
  return anySuccess;
}

bool MonitorEngine::checkSingleTarget(const String& target) {
  IPAddress ip;
  if (ip.fromString(target)) return Ping.ping(ip, 1);
  IPAddress resolved;
  if (WiFi.hostByName(target.c_str(), resolved)) return Ping.ping(resolved, 1);
  return false;
}

void MonitorEngine::runSmartPlugMode() {
  status_->healthState = HealthState::Healthy;
}

void MonitorEngine::runInternetWatchdogMode() {
  const bool ok = checkInternetTargets();
  if (ok) {
    status_->healthState = HealthState::Healthy;
    failureStartMs_ = 0;
    return;
  }

  status_->healthState = HealthState::Failed;
  if (failureStartMs_ == 0) failureStartMs_ = millis();

  if (millis() - failureStartMs_ >= config_->internet.failureThresholdSeconds * 1000UL) {
    triggerPowerCycle(config_->internet.powerOffSeconds, config_->internet.postRebootHoldoffSeconds, "all_targets_failed");
  }
}

void MonitorEngine::runDeviceWatchdogMode() {
  if (config_->device.target.isEmpty()) return;
  const bool ok = checkSingleTarget(config_->device.target);
  if (ok) {
    status_->healthState = HealthState::Healthy;
    failureStartMs_ = 0;
    return;
  }

  status_->healthState = HealthState::Failed;
  if (failureStartMs_ == 0) failureStartMs_ = millis();

  if (millis() - failureStartMs_ >= config_->device.failureThresholdSeconds * 1000UL) {
    triggerPowerCycle(config_->device.powerOffSeconds, config_->device.postRebootHoldoffSeconds, "device_target_failed");
  }
}

void MonitorEngine::triggerPowerCycle(uint32_t powerOffSeconds, uint32_t holdoffSeconds, const String& reason) {
  (void)powerOffSeconds;
  (void)holdoffSeconds;
  if (powerCycleActive_ || status_->inHoldoff) return;
  powerCycleActive_ = true;
  status_->currentIncidentCycles++;
  eventLog_->add("trigger", reason);
  notifier_->send("watchdog_trigger", reason, "{}");
  failureStartMs_ = 0;
}

// ============================== src/notification_manager.cpp ================
#include <Arduino.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include "notification_manager.h"

void NotificationManager::begin(AppConfig* config) {
  config_ = config;
}

bool NotificationManager::send(const String& eventType, const String& reason, const String& detailsJson) {
  if (!config_ || !config_->notifications.enabled || config_->notifications.webhookUrl.isEmpty()) return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, config_->notifications.webhookUrl)) return false;
  http.addHeader("Content-Type", "application/json");
  if (!config_->notifications.webhookAuthToken.isEmpty()) {
    http.addHeader("Authorization", "Bearer " + config_->notifications.webhookAuthToken);
  }

  String body = "{\"event_type\":\"" + eventType + "\",\"reason\":\"" + reason + "\",\"details\":" + detailsJson + "}";
  int code = http.POST(body);
  http.end();
  return code >= 200 && code < 300;
}

// ============================== src/event_log.cpp ===========================
#include <Arduino.h>
#include <ArduinoJson.h>
#include "event_log.h"

void EventLog::begin(uint16_t maxEntries) {
  maxEntries_ = maxEntries;
}

void EventLog::add(const String& type, const String& message) {
  if (items_.size() >= maxEntries_) items_.erase(items_.begin());
  items_.push_back({millis() / 1000UL, type, message});
}

String EventLog::asJson() const {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (const auto& item : items_) {
    JsonObject o = arr.add<JsonObject>();
    o["ts"] = item.ts;
    o["type"] = item.type;
    o["message"] = item.message;
  }
  String out;
  serializeJson(doc, out);
  return out;
}

// ============================== src/web_server_manager.cpp ==================
#include <Arduino.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "web_server_manager.h"
#include "relay_controller.h"
#include "config_manager.h"
#include "event_log.h"
#include "monitor_engine.h"

static ESP8266WebServer server(80);
static RelayController* sRelay = nullptr;
static AppConfig* sConfig = nullptr;
static RuntimeStatus* sStatus = nullptr;
static ConfigManager* sCfgMgr = nullptr;
static EventLog* sEventLog = nullptr;
static MonitorEngine* sMonitor = nullptr;

static String modeToString(DeviceMode mode) {
  switch (mode) {
    case DeviceMode::InternetWatchdog: return "internet_watchdog";
    case DeviceMode::DeviceWatchdog: return "device_watchdog";
    default: return "smart_plug";
  }
}

void WebServerManager::begin(AppConfig* config, RuntimeStatus* status,
                             RelayController* relay, ConfigManager* cfgMgr,
                             EventLog* eventLog, MonitorEngine* monitor) {
  config_ = config;
  status_ = status;
  sConfig = config;
  sStatus = status;
  sRelay = relay;
  sCfgMgr = cfgMgr;
  sEventLog = eventLog;
  sMonitor = monitor;

  server.on("/api/status", HTTP_GET, []() {
    JsonDocument doc;
    doc["device_name"] = sConfig->deviceName;
    doc["mode"] = modeToString(sConfig->currentMode);
    doc["relay_on"] = sRelay->isOn();
    doc["wifi_connected"] = sStatus->wifiConnected;
    doc["health_state"] = static_cast<int>(sStatus->healthState);
    doc["uptime_seconds"] = sStatus->uptimeSeconds;
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  server.on("/api/events", HTTP_GET, []() {
    server.send(200, "application/json", sEventLog->asJson());
  });

  server.on("/api/relay/on", HTTP_POST, []() {
    sRelay->set(true);
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/relay/off", HTTP_POST, []() {
    sRelay->set(false);
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/config/save", HTTP_POST, []() {
    if (!server.hasArg("plain")) {
      server.send(400, "application/json", "{\"error\":\"missing body\"}");
      return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
      server.send(400, "application/json", "{\"error\":\"invalid json\"}");
      return;
    }

    const String mode = doc["current_mode"] | modeToString(sConfig->currentMode);
    if (mode == "internet_watchdog") sConfig->currentMode = DeviceMode::InternetWatchdog;
    else if (mode == "device_watchdog") sConfig->currentMode = DeviceMode::DeviceWatchdog;
    else sConfig->currentMode = DeviceMode::SmartPlug;

    sConfig->deviceName = doc["device_name"] | sConfig->deviceName;
    sConfig->internet.failureThresholdSeconds = doc["internet"]["failure_threshold_seconds"] | sConfig->internet.failureThresholdSeconds;
    sConfig->internet.powerOffSeconds = doc["internet"]["power_off_seconds"] | sConfig->internet.powerOffSeconds;
    sConfig->internet.postRebootHoldoffSeconds = doc["internet"]["post_reboot_holdoff_seconds"] | sConfig->internet.postRebootHoldoffSeconds;
    sConfig->device.target = doc["device"]["target"] | sConfig->device.target;
    sConfig->device.failureThresholdSeconds = doc["device"]["failure_threshold_seconds"] | sConfig->device.failureThresholdSeconds;
    sConfig->device.powerOffSeconds = doc["device"]["power_off_seconds"] | sConfig->device.powerOffSeconds;
    sConfig->device.postRebootHoldoffSeconds = doc["device"]["post_reboot_holdoff_seconds"] | sConfig->device.postRebootHoldoffSeconds;
    sConfig->notifications.enabled = doc["notifications"]["enabled"] | sConfig->notifications.enabled;
    sConfig->notifications.webhookUrl = doc["notifications"]["webhook_url"] | sConfig->notifications.webhookUrl;

    sCfgMgr->save(*sConfig);
    sMonitor->resetIncident();
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/", HTTP_GET, []() {
    if (LittleFS.exists("/index.html")) {
      File f = LittleFS.open("/index.html", "r");
      server.streamFile(f, "text/html");
      f.close();
    } else {
      server.send(200, "text/plain", "Rebooter firmware is running. Upload UI assets to LittleFS.");
    }
  });

  server.begin();
}

void WebServerManager::loop() {
  server.handleClient();
}

// ============================== data/index.html =============================
/*
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Rebooter</title>
  <link rel="stylesheet" href="/style.css">
</head>
<body>
  <main>
    <h1>Rebooter</h1>
    <section id="status"></section>
    <button onclick="relayOn()">Relay On</button>
    <button onclick="relayOff()">Relay Off</button>
  </main>
  <script src="/app.js"></script>
</body>
</html>
*/

// ============================== data/app.js ================================
/*
async function refreshStatus() {
  const res = await fetch('/api/status');
  const data = await res.json();
  document.getElementById('status').innerText = JSON.stringify(data, null, 2);
}
async function relayOn() { await fetch('/api/relay/on', { method: 'POST' }); refreshStatus(); }
async function relayOff() { await fetch('/api/relay/off', { method: 'POST' }); refreshStatus(); }
refreshStatus();
setInterval(refreshStatus, 5000);
*/

// ============================== data/style.css ==============================
/*
body { font-family: Arial, sans-serif; margin: 2rem; }
main { max-width: 800px; margin: 0 auto; }
button { margin-right: 0.5rem; }
#status { white-space: pre-wrap; background: #f5f5f5; padding: 1rem; border-radius: 8px; margin: 1rem 0; }
*/

// ============================== build / flash notes =========================
// Build:
//   pio run -e sonoff_s31
// Upload firmware over serial:
//   pio run -e sonoff_s31 -t upload
// Upload LittleFS assets:
//   pio run -e sonoff_s31 -t uploadfs
// Serial monitor:
//   pio device monitor -b 115200
//
// This is a real starter scaffold, not a finished production firmware.
// Missing or intentionally simplified items before release:
// - robust auth/session management
// - proper Sonoff S31 pin verification on your board revision
// - rate limiting / lockout logic
// - hourly cycle accounting and cooldown enforcement
// - config validation
// - richer UI and captive portal customization
// - OTA upload path in UI
// - test coverage and burn-in validation
