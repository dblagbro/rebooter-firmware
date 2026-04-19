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

