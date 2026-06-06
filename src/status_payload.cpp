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
  target["timezone"] = config.timezone;
  target["current_mode"] = modeToString(config.currentMode);
  target["relay_restore_behavior"] = restoreToString(config.relayRestoreBehavior);
  target["monitor_interval_seconds"] = config.monitorIntervalSeconds;
  target["boot_warmup_seconds"] = config.bootWarmupSeconds;
  target["manual_button_enabled"] = config.manualButtonEnabled;
  target["status_led_enabled"] = config.statusLedEnabled;
  target["event_log_max_entries"] = config.eventLogMaxEntries;
  target["notification_cooldown_seconds"] = config.notificationCooldownSeconds;

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
  // include_frequency is omitted: no real mains-frequency value is ever produced.
}

void fillHeartbeatPowerSummary(JsonDocument& doc, const AppConfig& config,
                               const RuntimeStatus* status) {
  // Compact, bounded power summary carried inside the heartbeat envelope.
  // This replaces the standalone /device/power-samples HTTPS upload that
  // crashed low-heap S31 units: no second TLS session, fixed-size payload.
  // Forward-compatible: the hub may ignore these fields until its
  // power-in-heartbeat consumer ships.
  if (!config.power.enabled || !status) return;
  const PowerLiveStatus& power = status->power;

  JsonObject p = doc["power"].to<JsonObject>();
  p["enabled"] = true;
  p["chip_seen"] = power.chipSeen;
  p["uart_contended"] = power.uartContended;
  p["upload_mode"] = "heartbeat_piggyback";
  p["latest_v"] = power.voltageV;
  p["latest_a"] = power.currentMa / 1000.0f;
  p["latest_pf"] = power.powerFactor;
  p["energy_wh"] = power.energyWh;
  p["valid_frames"] = power.validFrameCount;
  p["invalid_frames"] = power.invalidFrameCount;
  if (power.aggregate.hasData && power.aggregate.sampleCount > 0) {
    p["min_w"] = power.aggregate.minW;
    p["max_w"] = power.aggregate.maxW;
    p["avg_w"] = power.aggregate.sumW / power.aggregate.sampleCount;
    p["sample_count"] = power.aggregate.sampleCount;
    p["window_start_uptime_seconds"] = power.aggregate.windowStartUptimeSeconds;
  } else {
    p["sample_count"] = 0;
  }
}

void fillPowerStatus(JsonDocument& doc, const AppConfig& config,
                     const RuntimeStatus* status) {
  doc["power_analytics_enabled"] = config.power.enabled;
  doc["power_chip_type"] = "CSE7766";
  doc["power_sample_rate_hz"] = config.power.sampleRateHz;
  doc["power_batch_seconds"] = config.power.batchSeconds;

  if (!status) return;

  const PowerLiveStatus& power = status->power;
  const uint32_t sampleAgeSeconds =
      (power.lastSampleUptimeSeconds > 0 && status->uptimeSeconds >= power.lastSampleUptimeSeconds)
          ? (status->uptimeSeconds - power.lastSampleUptimeSeconds)
          : 0;

  doc["power_chip_seen"] = power.chipSeen;
  doc["power_source"] = power.realSample ? "steady" : "none";
  doc["power_source_flags"] = power.sourceFlags;
  doc["power_last_sample_uptime_seconds"] = power.lastSampleUptimeSeconds;
  doc["power_last_sample_age_seconds"] = sampleAgeSeconds;
  doc["power_last_sample_unix_ms"] = power.lastSampleUnixMs;
  doc["power_valid_frame_count"] = power.validFrameCount;
  doc["power_invalid_frame_count"] = power.invalidFrameCount;
  doc["power_uart_contended"] = power.uartContended;
  // Power telemetry now rides the heartbeat envelope instead of a separate
  // HTTPS upload that crashed low-heap units; expose the mode so the
  // capability is visible even when memory-constrained.
  doc["power_upload_mode"] = "heartbeat_piggyback";

  if (power.aggregate.hasData && power.aggregate.sampleCount > 0) {
    JsonObject agg = doc["power_aggregate"].to<JsonObject>();
    agg["min_w"] = power.aggregate.minW;
    agg["max_w"] = power.aggregate.maxW;
    agg["avg_w"] = power.aggregate.sumW / power.aggregate.sampleCount;
    agg["sample_count"] = power.aggregate.sampleCount;
    agg["window_start_uptime_seconds"] = power.aggregate.windowStartUptimeSeconds;
  }

  if (!power.realSample) return;

  if (power.voltageValid) {
    doc["power_voltage_v"] = power.voltageV;
  }
  doc["power_current_ma"] = power.currentMa;
  doc["power_current_estimated"] = power.currentEstimated;
  if (power.currentEstimated) {
    doc["power_estimated_current_ma"] = power.estimatedCurrentMa;
  }
  doc["power_power_w"] = power.powerW;
  doc["power_apparent_power_va"] = power.apparentPowerVa;
  doc["power_power_factor"] = power.powerFactor;
  if (power.frequencyValid) {
    doc["power_frequency_hz"] = power.frequencyHz;
  }
  if (power.energyValid) {
    doc["power_energy_wh"] = power.energyWh;
  }
}

void fillHeartbeatDocument(JsonDocument& doc, const AppConfig& config,
                           const RuntimeStatus* status,
                           const String& firmwareVersion,
                           bool includeReportedConfig,
                           bool compactMode) {
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
  // 0.2.7: current-connection RSSI (dBm) so the hub can chart WiFi signal
  // quality + alert on degradation. Only meaningful while associated;
  // WiFi.RSSI() returns a sentinel (often 31) when not connected, so emit
  // it only when connected. Tiny (one int) — included even in compact mode
  // because constrained units are exactly where signal trouble bites.
  if (WiFi.isConnected()) {
    doc["wifi_rssi_dbm"] = WiFi.RSSI();
  }
  // 0.2.8 (#154): opt-in periodic nearby-network scan (top-N). Present
  // when the feature is on AND a scan has been attempted — including
  // empty-result scans (n==0 / WIFI_SCAN_FAILED), which write "[]" to
  // signal "I ran a scan and saw nothing". The pre-0.2.17 guard
  // `length() > 2` rejected exactly that "[]" payload, so the hub kept
  // the stale prior summary forever (0.6.18 review sweep S4). The new
  // condition emits any non-empty captured summary; "" still means
  // "feature was never turned on", which we suppress.
  if (status && !status->wifiScanSummary.isEmpty()) {
    JsonDocument scanDoc;
    if (deserializeJson(scanDoc, status->wifiScanSummary) == DeserializationError::Ok) {
      doc["wifi_scan"] = scanDoc;
      doc["wifi_scan_uptime_seconds"] = status->wifiScanUptimeSeconds;
    }
  }
  doc["recovery_mode"] = status ? status->recoveryMode : false;
  doc["auto_recovery_triggered"] = status ? status->autoRecoveryTriggered : false;
  doc["last_known_good_restored"] = status ? status->lastKnownGoodRestored : false;
  doc["consecutive_unhealthy_boots"] = status ? status->consecutiveUnhealthyBoots : 0;
  doc["health_state"] = healthToString(status ? status->healthState : HealthState::Unknown);
  doc["uptime_seconds"] = uptimeSeconds;
  doc["reset_reason"] = status ? status->resetReason : "";
  doc["central_enabled"] = status ? status->centralEnabled : config.central.enabled;
  doc["central_registered"] = status ? status->centralRegistered : (!config.central.deviceId.isEmpty() && !config.central.deviceToken.isEmpty());
  doc["central_state"] = status ? status->centralState : (config.central.enabled ? "idle" : "disabled");
  doc["central_device_id"] = status ? status->centralDeviceId : config.central.deviceId;
  doc["central_last_heartbeat_uptime_seconds"] = lastHeartbeatStamp;
  doc["central_heartbeat_age_seconds"] = heartbeatAgeSeconds;
  if (!compactMode) {
    doc["in_captive_portal"] = status ? status->inCaptivePortal : false;
    doc["previous_boot_different_firmware"] = status ? status->previousBootDifferentFirmware : false;
    doc["last_planned_restart_reason"] = status ? status->lastPlannedRestartReason : "";
    doc["time_synced"] = status ? status->timeSynced : false;
    doc["wall_clock_unix_ms"] = status ? status->wallClockUnixMs : 0;
    doc["incident_cycles"] = status ? status->currentIncidentCycles : 0;
    doc["hour_cycles"] = status ? status->currentHourCycles : 0;
    doc["holdoff_remaining_seconds"] = status ? status->holdoffRemainingSeconds : 0;
    doc["cooldown_remaining_seconds"] = status ? status->cooldownRemainingSeconds : 0;
    doc["last_event_type"] = status ? status->lastEvent : "boot";
    doc["last_event_at"] = "";
    fillPowerStatus(doc, config, status);
  }

  // Compact power summary rides every heartbeat (including compact mode), so
  // power telemetry no longer needs a separate, crash-prone HTTPS upload.
  fillHeartbeatPowerSummary(doc, config, status);

  if (includeReportedConfig && !compactMode) {
    JsonObject reportedConfig = doc["reported_config"].to<JsonObject>();
    fillReportedConfig(reportedConfig, config);
  }
}

}
