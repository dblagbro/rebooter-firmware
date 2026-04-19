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

static String healthToString(HealthState state) {
  switch (state) {
    case HealthState::Healthy: return "healthy";
    case HealthState::PartialFailure: return "partial_failure";
    case HealthState::Failed: return "failed";
    case HealthState::Holdoff: return "holdoff";
    case HealthState::Cooldown: return "cooldown";
    default: return "unknown";
  }
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

static void persistRelayState() {
  sConfig->lastRelayOn = sRelay->isOn();
  sCfgMgr->save(*sConfig);
}

static void sendConfigJson() {
  JsonDocument doc;
  doc["schema_version"] = sConfig->schemaVersion;
  doc["device_name"] = sConfig->deviceName;
  doc["current_mode"] = modeToString(sConfig->currentMode);
  doc["relay_restore_behavior"] = restoreToString(sConfig->relayRestoreBehavior);
  doc["last_relay_on"] = sConfig->lastRelayOn;
  doc["monitor_interval_seconds"] = sConfig->monitorIntervalSeconds;
  doc["boot_warmup_seconds"] = sConfig->bootWarmupSeconds;
  doc["manual_button_enabled"] = sConfig->manualButtonEnabled;

  JsonArray targets = doc["internet"]["targets"].to<JsonArray>();
  for (const auto& target : sConfig->internet.targets) targets.add(target);
  doc["internet"]["failure_threshold_seconds"] = sConfig->internet.failureThresholdSeconds;
  doc["internet"]["power_off_seconds"] = sConfig->internet.powerOffSeconds;
  doc["internet"]["post_reboot_holdoff_seconds"] = sConfig->internet.postRebootHoldoffSeconds;
  doc["internet"]["max_cycles_per_incident"] = sConfig->internet.maxCyclesPerIncident;
  doc["internet"]["max_cycles_per_hour"] = sConfig->internet.maxCyclesPerHour;
  doc["internet"]["cooldown_seconds"] = sConfig->internet.cooldownSeconds;

  doc["device"]["target"] = sConfig->device.target;
  doc["device"]["failure_threshold_seconds"] = sConfig->device.failureThresholdSeconds;
  doc["device"]["power_off_seconds"] = sConfig->device.powerOffSeconds;
  doc["device"]["post_reboot_holdoff_seconds"] = sConfig->device.postRebootHoldoffSeconds;
  doc["device"]["max_cycles_per_incident"] = sConfig->device.maxCyclesPerIncident;
  doc["device"]["max_cycles_per_hour"] = sConfig->device.maxCyclesPerHour;
  doc["device"]["cooldown_seconds"] = sConfig->device.cooldownSeconds;

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
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
    doc["health_state"] = healthToString(sStatus->healthState);
    doc["uptime_seconds"] = sStatus->uptimeSeconds;
    doc["incident_cycles"] = sStatus->currentIncidentCycles;
    doc["hour_cycles"] = sStatus->currentHourCycles;
    doc["holdoff_remaining_seconds"] = sStatus->holdoffRemainingSeconds;
    doc["cooldown_remaining_seconds"] = sStatus->cooldownRemainingSeconds;
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  server.on("/api/config", HTTP_GET, []() {
    sendConfigJson();
  });

  server.on("/api/events", HTTP_GET, []() {
    server.send(200, "application/json", sEventLog->asJson());
  });

  server.on("/api/relay/on", HTTP_POST, []() {
    sRelay->set(true);
    persistRelayState();
    sEventLog->add("relay", "Relay turned on by API");
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/relay/off", HTTP_POST, []() {
    sRelay->set(false);
    persistRelayState();
    sEventLog->add("relay", "Relay turned off by API");
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/relay/toggle", HTTP_POST, []() {
    sRelay->toggle();
    persistRelayState();
    sEventLog->add("relay", sRelay->isOn() ? "Relay toggled on by API" : "Relay toggled off by API");
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

    const String restore = doc["relay_restore_behavior"] | restoreToString(sConfig->relayRestoreBehavior);
    sConfig->relayRestoreBehavior = restoreFromString(restore);
    sConfig->deviceName = doc["device_name"] | sConfig->deviceName;
    sConfig->monitorIntervalSeconds = doc["monitor_interval_seconds"] | sConfig->monitorIntervalSeconds;
    sConfig->bootWarmupSeconds = doc["boot_warmup_seconds"] | sConfig->bootWarmupSeconds;
    sConfig->manualButtonEnabled = doc["manual_button_enabled"] | sConfig->manualButtonEnabled;

    if (doc["internet"]["targets"].is<JsonArray>()) {
      sConfig->internet.targets.clear();
      for (JsonVariant v : doc["internet"]["targets"].as<JsonArray>()) sConfig->internet.targets.push_back(String((const char*)v));
    }
    sConfig->internet.failureThresholdSeconds = doc["internet"]["failure_threshold_seconds"] | sConfig->internet.failureThresholdSeconds;
    sConfig->internet.powerOffSeconds = doc["internet"]["power_off_seconds"] | sConfig->internet.powerOffSeconds;
    sConfig->internet.postRebootHoldoffSeconds = doc["internet"]["post_reboot_holdoff_seconds"] | sConfig->internet.postRebootHoldoffSeconds;
    sConfig->internet.maxCyclesPerIncident = doc["internet"]["max_cycles_per_incident"] | sConfig->internet.maxCyclesPerIncident;
    sConfig->internet.maxCyclesPerHour = doc["internet"]["max_cycles_per_hour"] | sConfig->internet.maxCyclesPerHour;
    sConfig->internet.cooldownSeconds = doc["internet"]["cooldown_seconds"] | sConfig->internet.cooldownSeconds;
    sConfig->internet.dnsRefreshSeconds = doc["internet"]["dns_refresh_seconds"] | sConfig->internet.dnsRefreshSeconds;
    sConfig->internet.recoveryStabilitySeconds = doc["internet"]["recovery_stability_seconds"] | sConfig->internet.recoveryStabilitySeconds;

    sConfig->device.target = doc["device"]["target"] | sConfig->device.target;
    sConfig->device.failureThresholdSeconds = doc["device"]["failure_threshold_seconds"] | sConfig->device.failureThresholdSeconds;
    sConfig->device.powerOffSeconds = doc["device"]["power_off_seconds"] | sConfig->device.powerOffSeconds;
    sConfig->device.postRebootHoldoffSeconds = doc["device"]["post_reboot_holdoff_seconds"] | sConfig->device.postRebootHoldoffSeconds;
    sConfig->device.maxCyclesPerIncident = doc["device"]["max_cycles_per_incident"] | sConfig->device.maxCyclesPerIncident;
    sConfig->device.maxCyclesPerHour = doc["device"]["max_cycles_per_hour"] | sConfig->device.maxCyclesPerHour;
    sConfig->device.cooldownSeconds = doc["device"]["cooldown_seconds"] | sConfig->device.cooldownSeconds;
    sConfig->device.recoveryStabilitySeconds = doc["device"]["recovery_stability_seconds"] | sConfig->device.recoveryStabilitySeconds;

    sConfig->notifications.enabled = doc["notifications"]["enabled"] | sConfig->notifications.enabled;
    sConfig->notifications.webhookUrl = doc["notifications"]["webhook_url"] | sConfig->notifications.webhookUrl;
    sConfig->notifications.webhookAuthToken = doc["notifications"]["webhook_auth_token"] | sConfig->notifications.webhookAuthToken;

    sCfgMgr->save(*sConfig);
    sMonitor->resetIncident();
    sEventLog->add("config", "Configuration saved by API");
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