#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "config_manager.h"

static const char* LAST_KNOWN_GOOD_PATH = "/config.lkg.json";
static const char* TEMP_CONFIG_PATH = "/config.tmp";

static uint32_t clampU32(uint32_t value, uint32_t minValue, uint32_t maxValue) {
  if (value < minValue) return minValue;
  if (value > maxValue) return maxValue;
  return value;
}

static uint16_t clampU16(uint16_t value, uint16_t minValue, uint16_t maxValue) {
  if (value < minValue) return minValue;
  if (value > maxValue) return maxValue;
  return value;
}

static String modeToString(DeviceMode mode) {
  switch (mode) {
    case DeviceMode::InternetWatchdog: return "internet_watchdog";
    case DeviceMode::DeviceWatchdog: return "device_watchdog";
    default: return "smart_plug";
  }
}

static DeviceMode modeFromString(const String& value) {
  if (value == "internet_watchdog") return DeviceMode::InternetWatchdog;
  if (value == "device_watchdog") return DeviceMode::DeviceWatchdog;
  return DeviceMode::SmartPlug;
}

static String restoreToString(RelayRestoreBehavior value) {
  switch (value) {
    case RelayRestoreBehavior::AlwaysOn: return "always_on";
    case RelayRestoreBehavior::AlwaysOff: return "always_off";
    default: return "restore_previous";
  }
}

static RelayRestoreBehavior restoreFromString(const String& value) {
  if (value == "always_on") return RelayRestoreBehavior::AlwaysOn;
  if (value == "always_off") return RelayRestoreBehavior::AlwaysOff;
  return RelayRestoreBehavior::RestorePrevious;
}

static void setDefaultTargets(AppConfig& config) {
  config.internet.targets.clear();
  config.internet.targets.push_back("1.1.1.1");
  config.internet.targets.push_back("8.8.8.8");
  config.internet.targets.push_back("time.nist.gov");
}

static void validateConfig(AppConfig& config) {
  config.schemaVersion = CONFIG_SCHEMA_VERSION;
  config.deviceName.trim();
  if (config.deviceName.isEmpty() || config.deviceName.length() > 32) config.deviceName = "Rebooter";
  config.adminUsername.trim();
  if (config.adminUsername.isEmpty() || config.adminUsername.length() > 32) config.adminUsername = "admin";
  config.adminPasswordHash.trim();
  config.adminPasswordSalt.trim();
  if (config.adminPasswordHash.length() > 80) config.adminPasswordHash = "";
  if (config.adminPasswordSalt.length() > 48) config.adminPasswordSalt = "";
  if (config.adminPasswordHash.isEmpty()) config.adminPasswordSalt = "";
  config.timezone.trim();
  if (config.timezone.isEmpty() || config.timezone.length() > 64) config.timezone = "America/New_York";

  config.eventLogMaxEntries = clampU16(config.eventLogMaxEntries, 25, 1000);
  config.monitorIntervalSeconds = clampU32(config.monitorIntervalSeconds, 2, 3600);
  config.bootWarmupSeconds = clampU32(config.bootWarmupSeconds, 0, 600);
  config.notificationCooldownSeconds = clampU32(config.notificationCooldownSeconds, 0, 3600);

  std::vector<String> cleanedTargets;
  for (auto target : config.internet.targets) {
    target.trim();
    if (!target.isEmpty() && target.length() <= 128) cleanedTargets.push_back(target);
    if (cleanedTargets.size() >= 10) break;
  }
  config.internet.targets = cleanedTargets;
  if (config.internet.targets.empty()) setDefaultTargets(config);

  config.internet.failureThresholdSeconds = clampU32(config.internet.failureThresholdSeconds, 10, 86400);
  config.internet.powerOffSeconds = clampU32(config.internet.powerOffSeconds, 1, 300);
  config.internet.postRebootHoldoffSeconds = clampU32(config.internet.postRebootHoldoffSeconds, 10, 86400);
  config.internet.maxCyclesPerIncident = clampU32(config.internet.maxCyclesPerIncident, 1, 20);
  config.internet.maxCyclesPerHour = clampU32(config.internet.maxCyclesPerHour, 1, 60);
  config.internet.cooldownSeconds = clampU32(config.internet.cooldownSeconds, 60, 86400);
  config.internet.dnsRefreshSeconds = clampU32(config.internet.dnsRefreshSeconds, 60, 86400);
  config.internet.recoveryStabilitySeconds = clampU32(config.internet.recoveryStabilitySeconds, 0, 3600);

  config.device.target.trim();
  if (config.device.target.length() > 128) config.device.target = "";
  config.device.failureThresholdSeconds = clampU32(config.device.failureThresholdSeconds, 10, 86400);
  config.device.powerOffSeconds = clampU32(config.device.powerOffSeconds, 1, 300);
  config.device.postRebootHoldoffSeconds = clampU32(config.device.postRebootHoldoffSeconds, 10, 86400);
  config.device.maxCyclesPerIncident = clampU32(config.device.maxCyclesPerIncident, 1, 20);
  config.device.maxCyclesPerHour = clampU32(config.device.maxCyclesPerHour, 1, 60);
  config.device.cooldownSeconds = clampU32(config.device.cooldownSeconds, 60, 86400);
  config.device.recoveryStabilitySeconds = clampU32(config.device.recoveryStabilitySeconds, 0, 3600);

  config.notifications.type.trim();
  if (config.notifications.type != "webhook" && config.notifications.type != "pushover") config.notifications.type = "webhook";
  config.notifications.webhookMethod.trim();
  if (config.notifications.webhookMethod != "POST") config.notifications.webhookMethod = "POST";
  config.notifications.webhookUrl.trim();
  if (config.notifications.webhookUrl.length() > 256) config.notifications.webhookUrl = "";
  config.notifications.webhookAuthToken.trim();
  if (config.notifications.webhookAuthToken.length() > 128) config.notifications.webhookAuthToken = "";
}

static bool loadFromPath(const char* path, AppConfig& out) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err != DeserializationError::Ok) return false;

  out.schemaVersion = doc["schema_version"] | CONFIG_SCHEMA_VERSION;
  out.deviceName = doc["device_name"] | out.deviceName;
  out.adminUsername = doc["admin_username"] | out.adminUsername;
  out.adminPasswordHash = doc["admin_password_hash"] | out.adminPasswordHash;
  out.adminPasswordSalt = doc["admin_password_salt"] | out.adminPasswordSalt;
  out.timezone = doc["timezone"] | out.timezone;
  out.monitorIntervalSeconds = doc["monitor_interval_seconds"] | out.monitorIntervalSeconds;
  out.bootWarmupSeconds = doc["boot_warmup_seconds"] | out.bootWarmupSeconds;
  out.manualButtonEnabled = doc["manual_button_enabled"] | out.manualButtonEnabled;
  out.statusLedEnabled = doc["status_led_enabled"] | out.statusLedEnabled;
  out.eventLogMaxEntries = doc["event_log_max_entries"] | out.eventLogMaxEntries;
  out.notificationCooldownSeconds = doc["notification_cooldown_seconds"] | out.notificationCooldownSeconds;
  out.lastRelayOn = doc["last_relay_on"] | out.lastRelayOn;

  const String mode = doc["current_mode"] | "smart_plug";
  out.currentMode = modeFromString(mode);
  const String restore = doc["relay_restore_behavior"] | "restore_previous";
  out.relayRestoreBehavior = restoreFromString(restore);

  JsonArray targets = doc["internet"]["targets"].as<JsonArray>();
  out.internet.targets.clear();
  for (JsonVariant v : targets) out.internet.targets.push_back(String((const char*)v));

  out.internet.failureThresholdSeconds = doc["internet"]["failure_threshold_seconds"] | out.internet.failureThresholdSeconds;
  out.internet.powerOffSeconds = doc["internet"]["power_off_seconds"] | out.internet.powerOffSeconds;
  out.internet.postRebootHoldoffSeconds = doc["internet"]["post_reboot_holdoff_seconds"] | out.internet.postRebootHoldoffSeconds;
  out.internet.maxCyclesPerIncident = doc["internet"]["max_cycles_per_incident"] | out.internet.maxCyclesPerIncident;
  out.internet.maxCyclesPerHour = doc["internet"]["max_cycles_per_hour"] | out.internet.maxCyclesPerHour;
  out.internet.cooldownSeconds = doc["internet"]["cooldown_seconds"] | out.internet.cooldownSeconds;
  out.internet.dnsRefreshSeconds = doc["internet"]["dns_refresh_seconds"] | out.internet.dnsRefreshSeconds;
  out.internet.recoveryStabilitySeconds = doc["internet"]["recovery_stability_seconds"] | out.internet.recoveryStabilitySeconds;

  out.device.target = doc["device"]["target"] | out.device.target;
  out.device.failureThresholdSeconds = doc["device"]["failure_threshold_seconds"] | out.device.failureThresholdSeconds;
  out.device.powerOffSeconds = doc["device"]["power_off_seconds"] | out.device.powerOffSeconds;
  out.device.postRebootHoldoffSeconds = doc["device"]["post_reboot_holdoff_seconds"] | out.device.postRebootHoldoffSeconds;
  out.device.maxCyclesPerIncident = doc["device"]["max_cycles_per_incident"] | out.device.maxCyclesPerIncident;
  out.device.maxCyclesPerHour = doc["device"]["max_cycles_per_hour"] | out.device.maxCyclesPerHour;
  out.device.cooldownSeconds = doc["device"]["cooldown_seconds"] | out.device.cooldownSeconds;
  out.device.recoveryStabilitySeconds = doc["device"]["recovery_stability_seconds"] | out.device.recoveryStabilitySeconds;

  out.notifications.enabled = doc["notifications"]["enabled"] | out.notifications.enabled;
  out.notifications.type = doc["notifications"]["type"] | out.notifications.type;
  out.notifications.webhookUrl = doc["notifications"]["webhook_url"] | out.notifications.webhookUrl;
  out.notifications.webhookMethod = doc["notifications"]["webhook_method"] | out.notifications.webhookMethod;
  out.notifications.webhookAuthToken = doc["notifications"]["webhook_auth_token"] | out.notifications.webhookAuthToken;
  out.notifications.sendOnTrigger = doc["notifications"]["send_on_trigger"] | out.notifications.sendOnTrigger;
  out.notifications.sendOnRecovery = doc["notifications"]["send_on_recovery"] | out.notifications.sendOnRecovery;
  out.notifications.sendOnMaxCyclesReached = doc["notifications"]["send_on_max_cycles_reached"] | out.notifications.sendOnMaxCyclesReached;
  out.notifications.sendTestNotificationEnabled = doc["notifications"]["send_test_notification_enabled"] | out.notifications.sendTestNotificationEnabled;

  validateConfig(out);
  return true;
}

bool ConfigManager::begin() {
  return true;
}

bool ConfigManager::load(AppConfig& out) {
  if (!LittleFS.exists(configPath_)) {
    setDefaultTargets(out);
    validateConfig(out);
    return save(out);
  }

  if (loadFromPath(configPath_, out)) return true;
  if (LittleFS.exists(LAST_KNOWN_GOOD_PATH) && loadFromPath(LAST_KNOWN_GOOD_PATH, out)) {
    save(out);
    return true;
  }

  out = AppConfig();
  setDefaultTargets(out);
  validateConfig(out);
  return save(out);
}

bool ConfigManager::save(const AppConfig& config) {
  AppConfig clean = config;
  validateConfig(clean);

  JsonDocument doc;
  doc["schema_version"] = clean.schemaVersion;
  doc["device_name"] = clean.deviceName;
  doc["admin_username"] = clean.adminUsername;
  doc["admin_password_hash"] = clean.adminPasswordHash;
  doc["admin_password_salt"] = clean.adminPasswordSalt;
  doc["timezone"] = clean.timezone;
  doc["monitor_interval_seconds"] = clean.monitorIntervalSeconds;
  doc["boot_warmup_seconds"] = clean.bootWarmupSeconds;
  doc["manual_button_enabled"] = clean.manualButtonEnabled;
  doc["status_led_enabled"] = clean.statusLedEnabled;
  doc["event_log_max_entries"] = clean.eventLogMaxEntries;
  doc["notification_cooldown_seconds"] = clean.notificationCooldownSeconds;
  doc["last_relay_on"] = clean.lastRelayOn;
  doc["current_mode"] = modeToString(clean.currentMode);
  doc["relay_restore_behavior"] = restoreToString(clean.relayRestoreBehavior);

  JsonArray targets = doc["internet"]["targets"].to<JsonArray>();
  for (const auto& t : clean.internet.targets) targets.add(t);
  doc["internet"]["failure_threshold_seconds"] = clean.internet.failureThresholdSeconds;
  doc["internet"]["power_off_seconds"] = clean.internet.powerOffSeconds;
  doc["internet"]["post_reboot_holdoff_seconds"] = clean.internet.postRebootHoldoffSeconds;
  doc["internet"]["max_cycles_per_incident"] = clean.internet.maxCyclesPerIncident;
  doc["internet"]["max_cycles_per_hour"] = clean.internet.maxCyclesPerHour;
  doc["internet"]["cooldown_seconds"] = clean.internet.cooldownSeconds;
  doc["internet"]["dns_refresh_seconds"] = clean.internet.dnsRefreshSeconds;
  doc["internet"]["recovery_stability_seconds"] = clean.internet.recoveryStabilitySeconds;

  doc["device"]["target"] = clean.device.target;
  doc["device"]["failure_threshold_seconds"] = clean.device.failureThresholdSeconds;
  doc["device"]["power_off_seconds"] = clean.device.powerOffSeconds;
  doc["device"]["post_reboot_holdoff_seconds"] = clean.device.postRebootHoldoffSeconds;
  doc["device"]["max_cycles_per_incident"] = clean.device.maxCyclesPerIncident;
  doc["device"]["max_cycles_per_hour"] = clean.device.maxCyclesPerHour;
  doc["device"]["cooldown_seconds"] = clean.device.cooldownSeconds;
  doc["device"]["recovery_stability_seconds"] = clean.device.recoveryStabilitySeconds;

  doc["notifications"]["enabled"] = clean.notifications.enabled;
  doc["notifications"]["type"] = clean.notifications.type;
  doc["notifications"]["webhook_url"] = clean.notifications.webhookUrl;
  doc["notifications"]["webhook_method"] = clean.notifications.webhookMethod;
  doc["notifications"]["webhook_auth_token"] = clean.notifications.webhookAuthToken;
  doc["notifications"]["send_on_trigger"] = clean.notifications.sendOnTrigger;
  doc["notifications"]["send_on_recovery"] = clean.notifications.sendOnRecovery;
  doc["notifications"]["send_on_max_cycles_reached"] = clean.notifications.sendOnMaxCyclesReached;
  doc["notifications"]["send_test_notification_enabled"] = clean.notifications.sendTestNotificationEnabled;

  File f = LittleFS.open(TEMP_CONFIG_PATH, "w");
  if (!f) return false;
  serializeJsonPretty(doc, f);
  f.close();

  if (LittleFS.exists(LAST_KNOWN_GOOD_PATH)) LittleFS.remove(LAST_KNOWN_GOOD_PATH);
  if (LittleFS.exists(configPath_)) LittleFS.rename(configPath_, LAST_KNOWN_GOOD_PATH);
  if (LittleFS.exists(configPath_)) LittleFS.remove(configPath_);
  return LittleFS.rename(TEMP_CONFIG_PATH, configPath_);
}

bool ConfigManager::reset() {
  if (LittleFS.exists(configPath_)) LittleFS.remove(configPath_);
  if (LittleFS.exists(LAST_KNOWN_GOOD_PATH)) LittleFS.remove(LAST_KNOWN_GOOD_PATH);
  if (LittleFS.exists(TEMP_CONFIG_PATH)) LittleFS.remove(TEMP_CONFIG_PATH);
  return true;
}