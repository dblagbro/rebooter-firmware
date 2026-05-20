#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "config_manager.h"

static const char* LAST_KNOWN_GOOD_PATH = "/config.lkg.json";
static const char* TEMP_CONFIG_PATH = "/config.tmp";
static const char* RECOVERY_BOOT_FLAG_PATH = "/recovery.flag";
static const char* BOOT_STATE_PATH = "/bootstate.json";
static const char* EVENT_LOG_PATH = "/events.json";
static constexpr uint8_t AUTO_RECOVERY_BOOT_THRESHOLD = 2;

struct StoredBootState {
  uint8_t consecutiveUnhealthyBoots = 0;
  bool bootInProgress = false;
  bool plannedRestart = false;
  String lastFirmwareVersion = "";
  String plannedRestartReason = "";
};

static StoredBootState loadBootStateRecord() {
  StoredBootState state;
  File file = LittleFS.open(BOOT_STATE_PATH, "r");
  if (!file) return state;

  if (file.size() == 0 || file.size() > 256) {
    file.close();
    LittleFS.remove(BOOT_STATE_PATH);
    return state;
  }

  JsonDocument doc;
    if (deserializeJson(doc, file) == DeserializationError::Ok) {
      state.consecutiveUnhealthyBoots = doc["consecutive_unhealthy_boots"] | 0;
      state.bootInProgress = doc["boot_in_progress"] | false;
      state.plannedRestart = doc["planned_restart"] | false;
      state.lastFirmwareVersion = doc["last_firmware_version"] | "";
      state.plannedRestartReason = doc["planned_restart_reason"] | "";
    }

  file.close();
  return state;
}

static bool saveBootStateRecord(const StoredBootState& state) {
  JsonDocument doc;
  doc["consecutive_unhealthy_boots"] = state.consecutiveUnhealthyBoots;
  doc["boot_in_progress"] = state.bootInProgress;
  doc["planned_restart"] = state.plannedRestart;
  doc["last_firmware_version"] = state.lastFirmwareVersion;
  doc["planned_restart_reason"] = state.plannedRestartReason;

  File file = LittleFS.open(BOOT_STATE_PATH, "w");
  if (!file) return false;
  serializeJson(doc, file);
  file.close();
  return true;
}

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

static bool isLegacySecondaryCentralUrl(const String& url) {
  return url == "https://www2.voipguru.org/rebooter";
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
  // Pushover has no transport implemented in notification_manager.cpp, so it is not
  // an accepted type. Restricting it here keeps the schema honest (no advertised-but-fake
  // capability) until a real Pushover transport ships.
  if (config.notifications.type != "webhook") config.notifications.type = "webhook";
  config.notifications.webhookMethod.trim();
  if (config.notifications.webhookMethod != "POST") config.notifications.webhookMethod = "POST";
  config.notifications.webhookUrl.trim();
  if (config.notifications.webhookUrl.length() > 256) config.notifications.webhookUrl = "";
  config.notifications.webhookAuthToken.trim();
  if (config.notifications.webhookAuthToken.length() > 128) config.notifications.webhookAuthToken = "";

  std::vector<WifiNetwork> cleanedNetworks;
  for (auto network : config.wifi.savedNetworks) {
    network.ssid.trim();
    if (network.ssid.isEmpty() || network.ssid.length() > 32) continue;
    if (network.password.length() > 64) network.password = "";
    bool duplicate = false;
    for (const auto& existing : cleanedNetworks) {
      if (existing.ssid == network.ssid) { duplicate = true; break; }
    }
    if (duplicate) continue;
    cleanedNetworks.push_back(network);
    if (cleanedNetworks.size() >= 5) break;
  }
  config.wifi.savedNetworks = cleanedNetworks;
  config.wifi.connectTimeoutMs = clampU32(config.wifi.connectTimeoutMs, 5000, 60000);

  for (auto& url : config.central.baseUrls) {
    url.trim();
  }
  std::vector<String> cleanedBaseUrls;
  for (auto url : config.central.baseUrls) {
    if (url.isEmpty() || url.length() > HubDefaults::MAX_BASE_URL_LENGTH) continue;
    if (url.endsWith("/")) url.remove(url.length() - 1);
    if (url.endsWith("/api/v1")) url.remove(url.length() - 7);
    if (isLegacySecondaryCentralUrl(url)) continue;
    bool duplicate = false;
    for (const auto& existing : cleanedBaseUrls) {
      if (existing == url) { duplicate = true; break; }
    }
    if (duplicate) continue;
    cleanedBaseUrls.push_back(url);
    if (cleanedBaseUrls.size() >= HubDefaults::MAX_BASE_URLS) break;
  }
  if (cleanedBaseUrls.empty()) {
    cleanedBaseUrls = HubDefaults::defaultBaseUrls();
  }
  config.central.baseUrls = cleanedBaseUrls;
  config.central.enrollmentToken.trim();
  if (config.central.enrollmentToken.length() > 128) config.central.enrollmentToken = "";
  config.central.deviceAlias.trim();
  if (config.central.deviceAlias.length() > 64) config.central.deviceAlias = "";
  config.central.siteId.trim();
  if (config.central.siteId.length() > 64) config.central.siteId = "";
  config.central.deviceId.trim();
  if (config.central.deviceId.length() > 96) config.central.deviceId = "";
  config.central.deviceToken.trim();
  if (config.central.deviceToken.length() > 256) config.central.deviceToken = "";
  config.central.pollIntervalSeconds = clampU32(config.central.pollIntervalSeconds, 10, 3600);
  config.central.heartbeatIntervalSeconds = clampU32(config.central.heartbeatIntervalSeconds, 10, 3600);

  config.power.sampleRateHz = static_cast<uint8_t>(clampU32(config.power.sampleRateHz, 1, 2));
  config.power.batchSeconds = static_cast<uint16_t>(clampU32(config.power.batchSeconds, 10, 60));
  // The CSE7766 path never produces a mains-frequency value; keep this off so the field
  // is not advertised as a real capability anywhere downstream.
  config.power.includeFrequency = false;

  // Clamp the UDP discovery port out of the privileged range and away from
  // the device's own services (HTTP 80). Fall back to the design default.
  if (config.discovery.udpPort < 1024 || config.discovery.udpPort == 80) {
    config.discovery.udpPort = 51999;
  }
}

static bool loadFromPath(const char* path, AppConfig& out) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;

  const size_t maxSafeBytes = 8192;
  const size_t fileSize = f.size();
  if (fileSize == 0 || fileSize > maxSafeBytes) {
    f.close();
    LittleFS.remove(path);
    return false;
  }

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

  out.wifi.savedNetworks.clear();
  if (doc["wifi"]["saved_networks"].is<JsonArray>()) {
    for (JsonVariant v : doc["wifi"]["saved_networks"].as<JsonArray>()) {
      WifiNetwork network;
      network.ssid = String((const char*)(v["ssid"] | ""));
      network.password = String((const char*)(v["password"] | ""));
      out.wifi.savedNetworks.push_back(network);
    }
  }
  out.wifi.connectTimeoutMs = doc["wifi"]["connect_timeout_ms"] | out.wifi.connectTimeoutMs;
  out.wifi.preferStrongestKnown = doc["wifi"]["prefer_strongest_known"] | out.wifi.preferStrongestKnown;

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

  out.central.enabled = doc["central"]["enabled"] | out.central.enabled;
  out.central.baseUrls.clear();
  if (doc["central"]["base_urls"].is<JsonArray>()) {
    for (JsonVariant v : doc["central"]["base_urls"].as<JsonArray>()) {
      out.central.baseUrls.push_back(String((const char*)v));
    }
  } else {
    const String legacyBaseUrl = doc["central"]["base_url"] | "";
    if (!legacyBaseUrl.isEmpty()) out.central.baseUrls.push_back(legacyBaseUrl);
  }
  out.central.enrollmentToken = doc["central"]["enrollment_token"] | out.central.enrollmentToken;
  out.central.deviceAlias = doc["central"]["device_alias"] | out.central.deviceAlias;
  out.central.siteId = doc["central"]["site_id"] | out.central.siteId;
  out.central.deviceId = doc["central"]["device_id"] | out.central.deviceId;
  out.central.deviceToken = doc["central"]["device_token"] | out.central.deviceToken;
  out.central.pollIntervalSeconds = doc["central"]["poll_interval_seconds"] | out.central.pollIntervalSeconds;
  out.central.heartbeatIntervalSeconds = doc["central"]["heartbeat_interval_seconds"] | out.central.heartbeatIntervalSeconds;

  out.power.enabled = doc["power"]["enabled"] | out.power.enabled;
  out.power.sampleRateHz = doc["power"]["sample_rate_hz"] | out.power.sampleRateHz;
  out.power.batchSeconds = doc["power"]["batch_seconds"] | out.power.batchSeconds;
  out.power.includeWifiStats = doc["power"]["include_wifi_stats"] | out.power.includeWifiStats;
  out.power.includeFrequency = doc["power"]["include_frequency"] | out.power.includeFrequency;

  out.discovery.mdnsEnabled = doc["discovery"]["mdns_enabled"] | out.discovery.mdnsEnabled;
  out.discovery.udpAnnounceEnabled = doc["discovery"]["udp_announce_enabled"] | out.discovery.udpAnnounceEnabled;
  out.discovery.udpPort = doc["discovery"]["udp_port"] | out.discovery.udpPort;

  validateConfig(out);
  return true;
}

static void writeConfigDocument(JsonDocument& doc, const AppConfig& clean) {
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

  // The config layer stores Wi-Fi passwords in plaintext (it is the source of
  // truth); the web layer is responsible for redacting them on read.
  JsonArray savedNetworks = doc["wifi"]["saved_networks"].to<JsonArray>();
  for (const auto& network : clean.wifi.savedNetworks) {
    JsonObject entry = savedNetworks.add<JsonObject>();
    entry["ssid"] = network.ssid;
    entry["password"] = network.password;
  }
  doc["wifi"]["connect_timeout_ms"] = clean.wifi.connectTimeoutMs;
  doc["wifi"]["prefer_strongest_known"] = clean.wifi.preferStrongestKnown;

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

  doc["central"]["enabled"] = clean.central.enabled;
  JsonArray centralBaseUrls = doc["central"]["base_urls"].to<JsonArray>();
  for (const auto& url : clean.central.baseUrls) centralBaseUrls.add(url);
  doc["central"]["enrollment_token"] = clean.central.enrollmentToken;
  doc["central"]["device_alias"] = clean.central.deviceAlias;
  doc["central"]["site_id"] = clean.central.siteId;
  doc["central"]["device_id"] = clean.central.deviceId;
  doc["central"]["device_token"] = clean.central.deviceToken;
  doc["central"]["poll_interval_seconds"] = clean.central.pollIntervalSeconds;
  doc["central"]["heartbeat_interval_seconds"] = clean.central.heartbeatIntervalSeconds;

  doc["power"]["enabled"] = clean.power.enabled;
  doc["power"]["sample_rate_hz"] = clean.power.sampleRateHz;
  doc["power"]["batch_seconds"] = clean.power.batchSeconds;
  doc["power"]["include_wifi_stats"] = clean.power.includeWifiStats;
  doc["power"]["include_frequency"] = clean.power.includeFrequency;

  doc["discovery"]["mdns_enabled"] = clean.discovery.mdnsEnabled;
  doc["discovery"]["udp_announce_enabled"] = clean.discovery.udpAnnounceEnabled;
  doc["discovery"]["udp_port"] = clean.discovery.udpPort;
}

static bool writeConfigToPath(const char* path, const AppConfig& config) {
  AppConfig clean = config;
  validateConfig(clean);

  JsonDocument doc;
  writeConfigDocument(doc, clean);

  File f = LittleFS.open(path, "w");
  if (!f) return false;
  serializeJsonPretty(doc, f);
  f.close();
  return true;
}

static void overlayRecoveryPreservedState(AppConfig& restored, const AppConfig& current) {
  restored.lastRelayOn = current.lastRelayOn;

  // Preserve the saved Wi-Fi list across an auto-recovery rollback so a
  // recovery does not strand the device on a network it can no longer reach.
  restored.wifi = current.wifi;

  restored.central.enabled = current.central.enabled;
  restored.central.baseUrls = current.central.baseUrls;
  restored.central.enrollmentToken = current.central.enrollmentToken;
  restored.central.deviceAlias = current.central.deviceAlias;
  restored.central.siteId = current.central.siteId;
  restored.central.deviceId = current.central.deviceId;
  restored.central.deviceToken = current.central.deviceToken;
  restored.central.pollIntervalSeconds = current.central.pollIntervalSeconds;
  restored.central.heartbeatIntervalSeconds = current.central.heartbeatIntervalSeconds;
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
  if (!writeConfigToPath(TEMP_CONFIG_PATH, clean)) return false;

  if (LittleFS.exists(LAST_KNOWN_GOOD_PATH)) LittleFS.remove(LAST_KNOWN_GOOD_PATH);
  if (LittleFS.exists(configPath_)) LittleFS.rename(configPath_, LAST_KNOWN_GOOD_PATH);
  if (LittleFS.exists(configPath_)) LittleFS.remove(configPath_);
  return LittleFS.rename(TEMP_CONFIG_PATH, configPath_);
}

bool ConfigManager::reset() {
  if (LittleFS.exists(configPath_)) LittleFS.remove(configPath_);
  if (LittleFS.exists(LAST_KNOWN_GOOD_PATH)) LittleFS.remove(LAST_KNOWN_GOOD_PATH);
  if (LittleFS.exists(TEMP_CONFIG_PATH)) LittleFS.remove(TEMP_CONFIG_PATH);
  if (LittleFS.exists(RECOVERY_BOOT_FLAG_PATH)) LittleFS.remove(RECOVERY_BOOT_FLAG_PATH);
  if (LittleFS.exists(BOOT_STATE_PATH)) LittleFS.remove(BOOT_STATE_PATH);
  if (LittleFS.exists(EVENT_LOG_PATH)) LittleFS.remove(EVENT_LOG_PATH);
  return true;
}

bool ConfigManager::restoreLastKnownGood(AppConfig& out) {
  if (!LittleFS.exists(LAST_KNOWN_GOOD_PATH)) return false;

  AppConfig current = out;
  AppConfig restored = AppConfig();
  if (!loadFromPath(LAST_KNOWN_GOOD_PATH, restored)) return false;

  // Recovery should roll back behavioral settings, but keep the freshest
  // low-risk operational identity/state from the currently running config.
  overlayRecoveryPreservedState(restored, current);
  validateConfig(restored);

  if (!writeConfigToPath(TEMP_CONFIG_PATH, restored)) return false;
  if (LittleFS.exists(configPath_)) LittleFS.remove(configPath_);
  if (!LittleFS.rename(TEMP_CONFIG_PATH, configPath_)) return false;

  out = restored;
  return true;
}

BootHealthSnapshot ConfigManager::beginBootSession(const String& currentFirmwareVersion) {
  StoredBootState state = loadBootStateRecord();
  BootHealthSnapshot snapshot;

  if (state.bootInProgress) {
    snapshot.previousBootIncomplete = true;
    if (state.plannedRestart) {
      snapshot.previousBootPlannedRestart = true;
      snapshot.previousPlannedRestartReason = state.plannedRestartReason;
      state.consecutiveUnhealthyBoots = 0;
    } else if (!state.lastFirmwareVersion.isEmpty() &&
        !currentFirmwareVersion.isEmpty() &&
        state.lastFirmwareVersion != currentFirmwareVersion) {
      snapshot.previousBootDifferentFirmware = true;
      state.consecutiveUnhealthyBoots = 0;
    } else if (state.consecutiveUnhealthyBoots < UINT8_MAX) {
      state.consecutiveUnhealthyBoots++;
    }
  } else {
    state.consecutiveUnhealthyBoots = 0;
  }

  snapshot.consecutiveUnhealthyBoots = state.consecutiveUnhealthyBoots;
  if (!snapshot.previousBootDifferentFirmware &&
      state.consecutiveUnhealthyBoots >= AUTO_RECOVERY_BOOT_THRESHOLD) {
    snapshot.autoRecoveryTriggered = true;
    state.consecutiveUnhealthyBoots = 0;
  }

  state.bootInProgress = true;
  state.plannedRestart = false;
  state.lastFirmwareVersion = currentFirmwareVersion;
  state.plannedRestartReason = "";
  saveBootStateRecord(state);
  return snapshot;
}

bool ConfigManager::markBootHealthy() {
  StoredBootState state = loadBootStateRecord();
  state.consecutiveUnhealthyBoots = 0;
  state.bootInProgress = false;
  state.plannedRestart = false;
  state.plannedRestartReason = "";
  return saveBootStateRecord(state);
}

bool ConfigManager::prepareForPlannedRestart(const String& reason) {
  StoredBootState state = loadBootStateRecord();
  state.consecutiveUnhealthyBoots = 0;
  state.bootInProgress = false;
  state.plannedRestart = true;
  state.plannedRestartReason = reason;
  return saveBootStateRecord(state);
}

bool ConfigManager::requestRecoveryBoot() {
  File f = LittleFS.open(RECOVERY_BOOT_FLAG_PATH, "w");
  if (!f) return false;
  f.print("recovery");
  f.close();
  return true;
}

bool ConfigManager::consumeRecoveryBootRequest() {
  if (!LittleFS.exists(RECOVERY_BOOT_FLAG_PATH)) return false;
  LittleFS.remove(RECOVERY_BOOT_FLAG_PATH);
  return true;
}
