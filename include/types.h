#pragma once

#include <Arduino.h>
#include <vector>

#include "hub_defaults.h"

static constexpr uint16_t CONFIG_SCHEMA_VERSION = 4;

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
  String webhookMethod = "POST";
  String webhookAuthToken = "";
  bool sendOnTrigger = true;
  bool sendOnRecovery = true;
  bool sendOnMaxCyclesReached = true;
  bool sendTestNotificationEnabled = true;
};

struct CentralConfig {
  bool enabled = false;
  std::vector<String> baseUrls = HubDefaults::defaultBaseUrls();
  String enrollmentToken = "";
  String deviceAlias = "";
  String siteId = "";
  String deviceId = "";
  String deviceToken = "";
  uint32_t pollIntervalSeconds = 30;
  uint32_t heartbeatIntervalSeconds = 60;
};

struct WifiNetwork {
  String ssid;          // <= 32 chars
  String password;      // <= 64 chars ("" = open network)
  // Priority is implicit: the vector index (0 = highest priority).
};

struct WifiConfig {
  std::vector<WifiNetwork> savedNetworks;   // user-managed, max 5
  uint32_t connectTimeoutMs = 15000;        // per-network attempt budget
  bool preferStrongestKnown = false;        // optional scan-first ordering
};

struct PowerAnalyticsConfig {
  bool enabled = false;
  uint8_t sampleRateHz = 1;
  uint16_t batchSeconds = 15;
  bool includeWifiStats = true;
  bool includeFrequency = true;
};

struct AppConfig {
  uint16_t schemaVersion = CONFIG_SCHEMA_VERSION;
  String deviceName = "Rebooter";
  String adminUsername = "admin";
  String adminPasswordHash = "";
  String adminPasswordSalt = "";
  String timezone = "America/New_York";
  DeviceMode currentMode = DeviceMode::SmartPlug;
  RelayRestoreBehavior relayRestoreBehavior = RelayRestoreBehavior::RestorePrevious;
  bool lastRelayOn = true;
  bool statusLedEnabled = true;
  uint16_t eventLogMaxEntries = 200;
  uint32_t monitorIntervalSeconds = 5;
  uint32_t bootWarmupSeconds = 30;
  uint32_t notificationCooldownSeconds = 60;
  bool manualButtonEnabled = true;
  WifiConfig wifi;
  InternetWatchdogConfig internet;
  DeviceWatchdogConfig device;
  NotificationConfig notifications;
  CentralConfig central;
  PowerAnalyticsConfig power;
};
