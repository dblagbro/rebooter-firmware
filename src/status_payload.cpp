#include <ESP8266WiFi.h>

#include "status_payload.h"

namespace StatusPayload {

String modeToString(DeviceMode mode) {
  switch (mode) {
    case DeviceMode::InternetWatchdog: return "internet_watchdog";
    case DeviceMode::DeviceWatchdog: return "device_watchdog";
    default: return "smart_plug";
  }
}

String healthToString(HealthState state) {
  switch (state) {
    case HealthState::Healthy: return "healthy";
    case HealthState::PartialFailure: return "partial_failure";
    case HealthState::Failed: return "failed";
    case HealthState::Holdoff: return "holdoff";
    case HealthState::Cooldown: return "cooldown";
    default: return "unknown";
  }
}

String restoreToString(RelayRestoreBehavior value) {
  switch (value) {
    case RelayRestoreBehavior::AlwaysOn: return "always_on";
    case RelayRestoreBehavior::AlwaysOff: return "always_off";
    default: return "restore_previous";
  }
}

void fillReportedConfig(JsonObject target, const AppConfig& config) {
  target["device_name"] = config.deviceName;
  target["current_mode"] = modeToString(config.currentMode);
  target["relay_restore_behavior"] = restoreToString(config.relayRestoreBehavior);
  target["monitor_interval_seconds"] = config.monitorIntervalSeconds;
  target["boot_warmup_seconds"] = config.bootWarmupSeconds;
  target["manual_button_enabled"] = config.manualButtonEnabled;

  JsonArray internetTargets = target["internet"]["targets"].to<JsonArray>();
  for (const auto& value : config.internet.targets) internetTargets.add(value);
  target["internet"]["failure_threshold_seconds"] = config.internet.failureThresholdSeconds;
  target["internet"]["power_off_seconds"] = config.internet.powerOffSeconds;
  target["internet"]["post_reboot_holdoff_seconds"] = config.internet.postRebootHoldoffSeconds;
  target["internet"]["max_cycles_per_incident"] = config.internet.maxCyclesPerIncident;
  target["internet"]["max_cycles_per_hour"] = config.internet.maxCyclesPerHour;
  target["internet"]["cooldown_seconds"] = config.internet.cooldownSeconds;
  target["internet"]["dns_refresh_seconds"] = config.internet.dnsRefreshSeconds;
  target["internet"]["recovery_stability_seconds"] = config.internet.recoveryStabilitySeconds;

  target["device"]["target"] = config.device.target;
  target["device"]["failure_threshold_seconds"] = config.device.failureThresholdSeconds;
  target["device"]["power_off_seconds"] = config.device.powerOffSeconds;
  target["device"]["post_reboot_holdoff_seconds"] = config.device.postRebootHoldoffSeconds;
  target["device"]["max_cycles_per_incident"] = config.device.maxCyclesPerIncident;
  target["device"]["max_cycles_per_hour"] = config.device.maxCyclesPerHour;
  target["device"]["cooldown_seconds"] = config.device.cooldownSeconds;
  target["device"]["recovery_stability_seconds"] = config.device.recoveryStabilitySeconds;

  target["notifications"]["enabled"] = config.notifications.enabled;
  target["notifications"]["type"] = config.notifications.type;
  target["notifications"]["webhook_method"] = config.notifications.webhookMethod;
  target["notifications"]["send_on_trigger"] = config.notifications.sendOnTrigger;
  target["notifications"]["send_on_recovery"] = config.notifications.sendOnRecovery;
  target["notifications"]["send_on_max_cycles_reached"] = config.notifications.sendOnMaxCyclesReached;
  target["notifications"]["send_test_notification_enabled"] = config.notifications.sendTestNotificationEnabled;

  target["central"]["enabled"] = config.central.enabled;
  JsonArray centralBaseUrls = target["central"]["base_urls"].to<JsonArray>();
  for (const auto& url : config.central.baseUrls) centralBaseUrls.add(url);
  target["central"]["device_alias"] = config.central.deviceAlias;
  target["central"]["poll_interval_seconds"] = config.central.pollIntervalSeconds;
  target["central"]["heartbeat_interval_seconds"] = config.central.heartbeatIntervalSeconds;

  target["power"]["enabled"] = config.power.enabled;
  target["power"]["sample_rate_hz"] = config.power.sampleRateHz;
  target["power"]["batch_seconds"] = config.power.batchSeconds;
  target["power"]["include_wifi_stats"] = config.power.includeWifiStats;
  target["power"]["include_frequency"] = config.power.includeFrequency;
}

void fillHeartbeatDocument(JsonDocument& doc, const AppConfig& config,
                           const RuntimeStatus* status,
                           const String& firmwareVersion) {
  const uint32_t lastHeartbeatStamp = status ? status->centralLastHeartbeatSeconds : 0;
  const uint32_t uptimeSeconds = status ? status->uptimeSeconds : 0;
  const uint32_t heartbeatAgeSeconds =
      (lastHeartbeatStamp > 0 && uptimeSeconds >= lastHeartbeatStamp)
          ? (uptimeSeconds - lastHeartbeatStamp)
          : 0;

  doc["device_id"] = config.central.deviceId;
  doc["firmware_version"] = firmwareVersion;
  doc["local_ip"] = WiFi.localIP().toString();
  doc["mode"] = modeToString(config.currentMode);
  doc["relay_on"] = status ? status->relayOn : true;
  doc["wifi_connected"] = status ? status->wifiConnected : true;
  doc["in_captive_portal"] = status ? status->inCaptivePortal : false;
  doc["recovery_mode"] = status ? status->recoveryMode : false;
  doc["auto_recovery_triggered"] = status ? status->autoRecoveryTriggered : false;
  doc["last_known_good_restored"] = status ? status->lastKnownGoodRestored : false;
  doc["consecutive_unhealthy_boots"] = status ? status->consecutiveUnhealthyBoots : 0;
  doc["health_state"] = healthToString(status ? status->healthState : HealthState::Unknown);
  doc["uptime_seconds"] = uptimeSeconds;
  doc["incident_cycles"] = status ? status->currentIncidentCycles : 0;
  doc["hour_cycles"] = status ? status->currentHourCycles : 0;
  doc["holdoff_remaining_seconds"] = status ? status->holdoffRemainingSeconds : 0;
  doc["cooldown_remaining_seconds"] = status ? status->cooldownRemainingSeconds : 0;
  doc["last_event_type"] = status ? status->lastEvent : "boot";
  doc["last_event_at"] = "";
  doc["central_enabled"] = status ? status->centralEnabled : config.central.enabled;
  doc["central_registered"] = status ? status->centralRegistered : (!config.central.deviceId.isEmpty() && !config.central.deviceToken.isEmpty());
  doc["central_state"] = status ? status->centralState : (config.central.enabled ? "idle" : "disabled");
  doc["central_device_id"] = status ? status->centralDeviceId : config.central.deviceId;
  doc["central_last_heartbeat_uptime_seconds"] = lastHeartbeatStamp;
  doc["central_heartbeat_age_seconds"] = heartbeatAgeSeconds;
  doc["power_analytics_enabled"] = config.power.enabled;
  doc["power_chip_type"] = "CSE7766";
  doc["power_sample_rate_hz"] = config.power.sampleRateHz;
  doc["power_batch_seconds"] = config.power.batchSeconds;

  JsonObject reportedConfig = doc["reported_config"].to<JsonObject>();
  fillReportedConfig(reportedConfig, config);
}

}
