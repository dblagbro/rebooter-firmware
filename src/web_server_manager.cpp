#include <Arduino.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiClientSecureBearSSL.h>
#include "https_client_helpers.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "web_server_manager.h"
#include "relay_controller.h"
#include "config_manager.h"
#include "event_log.h"
#include "firmware_version.h"
#include "safe_restart.h"
#include "monitor_engine.h"
#include "ota_manager.h"
#include "auth_manager.h"
#include "status_payload.h"
#include "wifi_manager.h"
#include "crash_recorder.h"
#include "power_monitor.h"
#include "discovery_manager.h"
#include "web_assets.h"


static ESP8266WebServer server(80);
static RelayController* sRelay = nullptr;
static AppConfig* sConfig = nullptr;
static RuntimeStatus* sStatus = nullptr;
static ConfigManager* sCfgMgr = nullptr;
static EventLog* sEventLog = nullptr;
static MonitorEngine* sMonitor = nullptr;
static OtaManager* sOta = nullptr;
static AuthManager* sAuth = nullptr;
static WifiManagerService* sWifi = nullptr;
static PowerMonitor* sPower = nullptr;
static DiscoveryManager* sDiscovery = nullptr;

static bool parseBaseUrl(const String& baseUrl, String& host, uint16_t& port, String& rootPath) {
  String url = baseUrl;
  url.trim();
  if (url.endsWith("/")) url.remove(url.length() - 1);

  port = 443;
  if (url.startsWith("https://")) {
    url.remove(0, 8);
  } else if (url.startsWith("http://")) {
    url.remove(0, 7);
    port = 80;
  } else {
    return false;
  }

  const int slash = url.indexOf('/');
  String hostPort = slash >= 0 ? url.substring(0, slash) : url;
  rootPath = slash >= 0 ? url.substring(slash) : "/";
  if (rootPath.isEmpty()) rootPath = "/";

  const int colon = hostPort.indexOf(':');
  if (colon >= 0) {
    host = hostPort.substring(0, colon);
    port = static_cast<uint16_t>(hostPort.substring(colon + 1).toInt());
  } else {
    host = hostPort;
  }

  return !host.isEmpty() && port > 0;
}

static void addCentralDiagnostic(JsonObject target, const String& baseUrl) {
  target["base_url"] = baseUrl;
  target["free_heap"] = ESP.getFreeHeap();

  String host;
  String rootPath;
  uint16_t port = 0;
  if (!parseBaseUrl(baseUrl, host, port, rootPath)) {
    target["parse_ok"] = false;
    return;
  }

  target["parse_ok"] = true;
  target["host"] = host;
  target["port"] = port;
  target["root_path"] = rootPath;

  IPAddress resolved;
  const bool dnsOk = WiFi.hostByName(host.c_str(), resolved);
  target["dns_ok"] = dnsOk;
  target["resolved_ip"] = dnsOk ? resolved.toString() : "";

  WiFiClient tcp;
  tcp.setTimeout(5000);
  const bool tcpOk = dnsOk ? tcp.connect(resolved, port) : tcp.connect(host.c_str(), port);
  target["tcp_connect_ok"] = tcpOk;
  if (tcpOk) tcp.stop();

  std::unique_ptr<BearSSL::WiFiClientSecure> secure(new BearSSL::WiFiClientSecure());
  configureBearSSLClient(*secure);
  HTTPClient http;
  const String versionUrl = baseUrl + (baseUrl.endsWith("/") ? "" : "/") + "api/v1/version";
  const bool beginOk = http.begin(*secure, versionUrl);
  target["https_begin_ok"] = beginOk;
  target["version_url"] = versionUrl;
  if (!beginOk) {
    return;
  }

  // 0.2.4: feed both watchdogs before the blocking TLS+GET. Same defensive
  // pattern as central_client.cpp's HTTPS sites — keeps the soft-WDT from
  // firing if a slow handshake takes >3.5s under heap pressure.
  ESP.wdtFeed();
  const int code = http.GET();
  target["https_code"] = code;
  if (code < 0) {
    target["https_error"] = HTTPClient::errorToString(code);
  } else {
    const String body = http.getString();
    target["https_body"] = body.substring(0, min(static_cast<int>(body.length()), 180));
  }
  http.end();
}

static void serveFileOrFallback(const char* path, const char* contentType, PGM_P fallback) {
  if (LittleFS.exists(path)) {
    File f = LittleFS.open(path, "r");
    server.streamFile(f, contentType);
    f.close();
    return;
  }

  server.send_P(200, contentType, fallback);
}

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

static bool requireAuth() {
  return sAuth && sAuth->requireAuth(server);
}

static bool authRequiredForUi() {
  return sAuth && sAuth->isProvisioned();
}

static void sendMethodNotAllowed(const char* allowed) {
  server.sendHeader("Allow", allowed);
  server.send(405, "application/json", "{\"error\":\"method not allowed\"}");
}

static void sendConfigJson(bool includeProtectedFields = false) {
  JsonDocument doc;
  doc["schema_version"] = sConfig->schemaVersion;
  doc["device_name"] = sConfig->deviceName;
  doc["admin_username"] = sConfig->adminUsername;
  doc["current_mode"] = modeToString(sConfig->currentMode);
  doc["relay_restore_behavior"] = restoreToString(sConfig->relayRestoreBehavior);
  doc["last_relay_on"] = sConfig->lastRelayOn;
  doc["timezone"] = sConfig->timezone;
  doc["monitor_interval_seconds"] = sConfig->monitorIntervalSeconds;
  doc["boot_warmup_seconds"] = sConfig->bootWarmupSeconds;
  doc["manual_button_enabled"] = sConfig->manualButtonEnabled;
  doc["status_led_enabled"] = sConfig->statusLedEnabled;
  doc["event_log_max_entries"] = sConfig->eventLogMaxEntries;
  doc["notification_cooldown_seconds"] = sConfig->notificationCooldownSeconds;

  // Wi-Fi saved networks: never echo plaintext passwords on the read surface.
  JsonArray savedNetworks = doc["wifi"]["saved_networks"].to<JsonArray>();
  for (const auto& network : sConfig->wifi.savedNetworks) {
    JsonObject entry = savedNetworks.add<JsonObject>();
    entry["ssid"] = network.ssid;
    entry["has_password"] = !network.password.isEmpty();
  }
  doc["wifi"]["connect_timeout_ms"] = sConfig->wifi.connectTimeoutMs;
  doc["wifi"]["prefer_strongest_known"] = sConfig->wifi.preferStrongestKnown;
  doc["wifi"]["periodic_scan_enabled"] = sConfig->wifi.periodicScanEnabled;
  doc["wifi"]["periodic_scan_interval_seconds"] = sConfig->wifi.periodicScanIntervalSeconds;

  JsonArray targets = doc["internet"]["targets"].to<JsonArray>();
  for (const auto& target : sConfig->internet.targets) targets.add(target);
  doc["internet"]["failure_threshold_seconds"] = sConfig->internet.failureThresholdSeconds;
  doc["internet"]["power_off_seconds"] = sConfig->internet.powerOffSeconds;
  doc["internet"]["post_reboot_holdoff_seconds"] = sConfig->internet.postRebootHoldoffSeconds;
  doc["internet"]["max_cycles_per_incident"] = sConfig->internet.maxCyclesPerIncident;
  doc["internet"]["max_cycles_per_hour"] = sConfig->internet.maxCyclesPerHour;
  doc["internet"]["cooldown_seconds"] = sConfig->internet.cooldownSeconds;
  doc["internet"]["dns_refresh_seconds"] = sConfig->internet.dnsRefreshSeconds;
  doc["internet"]["recovery_stability_seconds"] = sConfig->internet.recoveryStabilitySeconds;

  doc["device"]["target"] = sConfig->device.target;
  doc["device"]["failure_threshold_seconds"] = sConfig->device.failureThresholdSeconds;
  doc["device"]["power_off_seconds"] = sConfig->device.powerOffSeconds;
  doc["device"]["post_reboot_holdoff_seconds"] = sConfig->device.postRebootHoldoffSeconds;
  doc["device"]["max_cycles_per_incident"] = sConfig->device.maxCyclesPerIncident;
  doc["device"]["max_cycles_per_hour"] = sConfig->device.maxCyclesPerHour;
  doc["device"]["cooldown_seconds"] = sConfig->device.cooldownSeconds;
  doc["device"]["recovery_stability_seconds"] = sConfig->device.recoveryStabilitySeconds;

  doc["notifications"]["enabled"] = sConfig->notifications.enabled;
  doc["notifications"]["type"] = sConfig->notifications.type;
  doc["notifications"]["webhook_url"] = sConfig->notifications.webhookUrl;
  doc["notifications"]["webhook_method"] = sConfig->notifications.webhookMethod;
  doc["notifications"]["has_webhook_auth_token"] = !sConfig->notifications.webhookAuthToken.isEmpty();
  doc["notifications"]["send_on_trigger"] = sConfig->notifications.sendOnTrigger;
  doc["notifications"]["send_on_recovery"] = sConfig->notifications.sendOnRecovery;
  doc["notifications"]["send_on_max_cycles_reached"] = sConfig->notifications.sendOnMaxCyclesReached;
  doc["notifications"]["send_test_notification_enabled"] = sConfig->notifications.sendTestNotificationEnabled;
  if (includeProtectedFields) {
    doc["notifications"]["webhook_auth_token"] = sConfig->notifications.webhookAuthToken;
  }

  doc["central"]["enabled"] = sConfig->central.enabled;
  JsonArray centralBaseUrls = doc["central"]["base_urls"].to<JsonArray>();
  for (const auto& url : sConfig->central.baseUrls) centralBaseUrls.add(url);
  doc["central"]["device_alias"] = sConfig->central.deviceAlias;
  doc["central"]["registered"] =
      !sConfig->central.deviceId.isEmpty() && !sConfig->central.deviceToken.isEmpty();
  doc["central"]["has_enrollment_token"] = !sConfig->central.enrollmentToken.isEmpty();
  if (includeProtectedFields) {
    doc["central"]["enrollment_token"] = sConfig->central.enrollmentToken;
    doc["central"]["site_id"] = sConfig->central.siteId;
    doc["central"]["device_id"] = sConfig->central.deviceId;
    doc["central"]["device_token"] = sConfig->central.deviceToken;
  }
  doc["central"]["poll_interval_seconds"] = sConfig->central.pollIntervalSeconds;
  doc["central"]["heartbeat_interval_seconds"] = sConfig->central.heartbeatIntervalSeconds;

  doc["power"]["enabled"] = sConfig->power.enabled;
  doc["power"]["sample_rate_hz"] = sConfig->power.sampleRateHz;
  doc["power"]["batch_seconds"] = sConfig->power.batchSeconds;
  doc["power"]["include_wifi_stats"] = sConfig->power.includeWifiStats;
  // include_frequency is intentionally not exposed: the CSE7766 path never produces a
  // mains-frequency value, so surfacing it as a settable field advertises a fake capability.

  doc["discovery"]["mdns_enabled"] = sConfig->discovery.mdnsEnabled;
  doc["discovery"]["udp_announce_enabled"] = sConfig->discovery.udpAnnounceEnabled;
  doc["discovery"]["udp_port"] = sConfig->discovery.udpPort;

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void WebServerManager::begin(AppConfig* config, RuntimeStatus* status,
                             RelayController* relay, ConfigManager* cfgMgr,
                             EventLog* eventLog, MonitorEngine* monitor,
                             OtaManager* ota,
                             AuthManager* auth,
                             WifiManagerService* wifi,
                             PowerMonitor* power,
                             DiscoveryManager* discovery) {
  config_ = config;
  status_ = status;
  sConfig = config;
  sStatus = status;
  sRelay = relay;
  sCfgMgr = cfgMgr;
  sEventLog = eventLog;
  sMonitor = monitor;
  sOta = ota;
  sAuth = auth;
  sWifi = wifi;
  sPower = power;
  sDiscovery = discovery;
  server.collectHeaders("X-Rebooter-Auth");

  server.on("/api/status", HTTP_GET, []() {
      JsonDocument doc;
      const uint32_t lastHeartbeatStamp = sStatus->centralLastHeartbeatSeconds;
      const uint32_t heartbeatAgeSeconds =
          (lastHeartbeatStamp > 0 && sStatus->uptimeSeconds >= lastHeartbeatStamp)
              ? (sStatus->uptimeSeconds - lastHeartbeatStamp)
              : 0;
      doc["device_name"] = sConfig->deviceName;
      doc["firmware_version"] = FIRMWARE_VERSION;
      doc["mode"] = modeToString(sConfig->currentMode);
      doc["relay_on"] = sRelay->isOn();
      doc["wifi_connected"] = sStatus->wifiConnected;
      // 0.2.7: current-connection RSSI (dBm), only when associated.
      if (WiFi.isConnected()) {
        doc["wifi_rssi_dbm"] = WiFi.RSSI();
      }
      // 0.2.8 (#154): latest opt-in periodic nearby-network scan (top-N),
      // for local verification + parity with the heartbeat. Present only
      // when the feature is on + a summary has been captured.
      {
        const String& scan = sWifi->latestScanSummary();
        if (scan.length() > 2) {
          JsonDocument scanDoc;
          if (deserializeJson(scanDoc, scan) == DeserializationError::Ok) {
            doc["wifi_scan"] = scanDoc;
            doc["wifi_scan_uptime_seconds"] = sWifi->latestScanUptimeSeconds();
          }
        }
      }
      doc["in_captive_portal"] = sStatus->inCaptivePortal;
      doc["recovery_mode"] = sStatus->recoveryMode;
      doc["auto_recovery_triggered"] = sStatus->autoRecoveryTriggered;
      doc["last_known_good_restored"] = sStatus->lastKnownGoodRestored;
      doc["previous_boot_different_firmware"] = sStatus->previousBootDifferentFirmware;
      doc["consecutive_unhealthy_boots"] = sStatus->consecutiveUnhealthyBoots;
      doc["setup_ap_name"] = sStatus->setupApName;
      doc["health_state"] = healthToString(sStatus->healthState);
      doc["uptime_seconds"] = sStatus->uptimeSeconds;
      doc["reset_reason"] = sStatus->resetReason;
      doc["last_planned_restart_reason"] = sStatus->lastPlannedRestartReason;
      doc["time_synced"] = sStatus->timeSynced;
      doc["wall_clock_unix_ms"] = sStatus->wallClockUnixMs;
      doc["free_heap"] = ESP.getFreeHeap();
      doc["max_free_block"] = ESP.getMaxFreeBlockSize();
      doc["heap_fragmentation_pct"] = ESP.getHeapFragmentation();
      doc["incident_cycles"] = sStatus->currentIncidentCycles;
      doc["hour_cycles"] = sStatus->currentHourCycles;
      doc["holdoff_remaining_seconds"] = sStatus->holdoffRemainingSeconds;
      doc["cooldown_remaining_seconds"] = sStatus->cooldownRemainingSeconds;
      doc["auth_required"] = authRequiredForUi();
      doc["central_enabled"] = sStatus->centralEnabled;
      doc["central_registered"] = sStatus->centralRegistered;
      doc["central_state"] = sStatus->centralState;
      doc["central_identity_present"] = !sStatus->centralDeviceId.isEmpty();
      doc["central_last_heartbeat_seconds"] = lastHeartbeatStamp;
      doc["central_last_heartbeat_uptime_seconds"] = lastHeartbeatStamp;
      doc["central_heartbeat_age_seconds"] = heartbeatAgeSeconds;
      doc["last_crash_present"] = sStatus->lastCrashPresent;
      if (sStatus->lastCrashPresent) {
        doc["last_crash_reason"] = sStatus->lastCrashReason;
      }
      StatusPayload::fillPowerStatus(doc, *sConfig, sStatus);
      String out;
      serializeJson(doc, out);
      server.send(200, "application/json", out);
    });

  server.on("/api/config", HTTP_GET, []() {
    sendConfigJson();
  });

  server.on("/api/system/config-backup", HTTP_GET, []() {
    if (!requireAuth()) return;
    if (!sAuth->isProvisioned()) {
      server.send(409, "application/json", "{\"error\":\"set admin password before exporting protected backup\"}");
      return;
    }
    sendConfigJson(true);
  });

  server.on("/api/events", HTTP_GET, []() {
    server.send(200, "application/json", sEventLog->asJson());
  });

  server.on("/api/system/central-diagnostic", HTTP_GET, []() {
    if (!requireAuth()) return;

    JsonDocument doc;
    doc["wifi_connected"] = sStatus->wifiConnected;
    doc["local_ip"] = WiFi.localIP().toString();
    doc["mac_address"] = WiFi.macAddress();
    doc["free_heap"] = ESP.getFreeHeap();

    JsonArray diagnostics = doc["targets"].to<JsonArray>();
    for (const auto& baseUrl : sConfig->central.baseUrls) {
      JsonObject entry = diagnostics.add<JsonObject>();
      addCentralDiagnostic(entry, baseUrl);
    }

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  server.on("/api/system/heartbeat-preview", HTTP_GET, []() {
    if (!requireAuth()) return;

    JsonDocument doc;
    StatusPayload::fillHeartbeatDocument(doc, *sConfig, sStatus, FIRMWARE_VERSION, true);
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // Crash dumps reveal addresses and stack words, so keep them behind auth
  // like the other diagnostic endpoints.
  server.on("/api/system/crash", HTTP_GET, []() {
    if (!requireAuth()) return;
    server.send(200, "application/json", CrashRecorder::storedCrashesJson());
  });

  server.on("/api/system/crash/clear", HTTP_POST, []() {
    if (!requireAuth()) return;
    CrashRecorder::clearStoredCrashes();
    sStatus->lastCrashPresent = false;
    sStatus->lastCrashReason = "";
    sEventLog->add("crash", "Stored crash dumps cleared by API");
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/system/crash/clear", HTTP_GET, []() { sendMethodNotAllowed("POST"); });

  // Fixed-size in-RAM ring of the most recent raw power samples. Public read,
  // consistent with the other read endpoints.
  server.on("/api/power/recent", HTTP_GET, []() {
    if (!sPower) {
      server.send(200, "application/json", "[]");
      return;
    }
    server.send(200, "application/json", sPower->recentSamplesJson());
  });

  server.on("/api/power/energy/reset", HTTP_POST, []() {
    if (!requireAuth()) return;
    if (sPower) sPower->resetEnergy();
    sEventLog->add("power", "Energy accumulator reset by API");
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/power/energy/reset", HTTP_GET, []() { sendMethodNotAllowed("POST"); });

  // On-demand LAN discovery UDP announce burst.
  server.on("/api/discovery/announce", HTTP_POST, []() {
    if (!requireAuth()) return;
    if (!sConfig->discovery.udpAnnounceEnabled) {
      server.send(409, "application/json",
                  "{\"ok\":false,\"error\":\"udp announce disabled in config\"}");
      return;
    }
    if (sDiscovery) sDiscovery->triggerAnnounceBurst();
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/discovery/announce", HTTP_GET, []() { sendMethodNotAllowed("POST"); });

  server.on("/api/relay/on", HTTP_POST, []() {
    if (!requireAuth()) return;
    sRelay->set(true);
    persistRelayState();
    sEventLog->add("relay", "Relay turned on by API");
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/relay/off", HTTP_POST, []() {
    if (!requireAuth()) return;
    sRelay->set(false);
    persistRelayState();
    sEventLog->add("relay", "Relay turned off by API");
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/relay/toggle", HTTP_POST, []() {
    if (!requireAuth()) return;
    sRelay->toggle();
    persistRelayState();
    sEventLog->add("relay", sRelay->isOn() ? "Relay toggled on by API" : "Relay toggled off by API");
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/config/save", HTTP_POST, []() {
    if (!requireAuth()) return;
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
    if (doc["admin_password"].is<const char*>()) {
      const String user = doc["admin_username"] | sConfig->adminUsername;
      const String password = doc["admin_password"] | "";
      if (!sAuth->setPassword(user, password)) {
        server.send(400, "application/json", "{\"error\":\"invalid credentials\"}");
        return;
      }
    }
    sConfig->timezone = doc["timezone"] | sConfig->timezone;
    sConfig->monitorIntervalSeconds = doc["monitor_interval_seconds"] | sConfig->monitorIntervalSeconds;
    sConfig->bootWarmupSeconds = doc["boot_warmup_seconds"] | sConfig->bootWarmupSeconds;
    sConfig->manualButtonEnabled = doc["manual_button_enabled"] | sConfig->manualButtonEnabled;
    sConfig->statusLedEnabled = doc["status_led_enabled"] | sConfig->statusLedEnabled;
    sConfig->eventLogMaxEntries = doc["event_log_max_entries"] | sConfig->eventLogMaxEntries;
    sConfig->notificationCooldownSeconds = doc["notification_cooldown_seconds"] | sConfig->notificationCooldownSeconds;

    if (doc["wifi"].is<JsonObject>()) {
      if (doc["wifi"]["saved_networks"].is<JsonArray>()) {
        // Snapshot existing passwords so an edit-without-retype (password field
        // absent for a known SSID) keeps the stored password. A present-but-empty
        // password explicitly sets an open network.
        std::vector<WifiNetwork> previous = sConfig->wifi.savedNetworks;
        std::vector<WifiNetwork> updated;
        for (JsonVariant v : doc["wifi"]["saved_networks"].as<JsonArray>()) {
          WifiNetwork network;
          network.ssid = String((const char*)(v["ssid"] | ""));
          if (v["password"].is<const char*>()) {
            network.password = String((const char*)v["password"]);
          } else {
            network.password = "";
            for (const auto& old : previous) {
              if (old.ssid == network.ssid) { network.password = old.password; break; }
            }
          }
          updated.push_back(network);
        }
        sConfig->wifi.savedNetworks = updated;
      }
      sConfig->wifi.connectTimeoutMs = doc["wifi"]["connect_timeout_ms"] | sConfig->wifi.connectTimeoutMs;
      sConfig->wifi.preferStrongestKnown = doc["wifi"]["prefer_strongest_known"] | sConfig->wifi.preferStrongestKnown;
      sConfig->wifi.periodicScanEnabled = doc["wifi"]["periodic_scan_enabled"] | sConfig->wifi.periodicScanEnabled;
      sConfig->wifi.periodicScanIntervalSeconds = doc["wifi"]["periodic_scan_interval_seconds"] | sConfig->wifi.periodicScanIntervalSeconds;
    }

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
    sConfig->notifications.type = doc["notifications"]["type"] | sConfig->notifications.type;
    sConfig->notifications.webhookUrl = doc["notifications"]["webhook_url"] | sConfig->notifications.webhookUrl;
    sConfig->notifications.webhookMethod = doc["notifications"]["webhook_method"] | sConfig->notifications.webhookMethod;
    sConfig->notifications.webhookAuthToken = doc["notifications"]["webhook_auth_token"] | sConfig->notifications.webhookAuthToken;
    sConfig->notifications.sendOnTrigger = doc["notifications"]["send_on_trigger"] | sConfig->notifications.sendOnTrigger;
    sConfig->notifications.sendOnRecovery = doc["notifications"]["send_on_recovery"] | sConfig->notifications.sendOnRecovery;
    sConfig->notifications.sendOnMaxCyclesReached = doc["notifications"]["send_on_max_cycles_reached"] | sConfig->notifications.sendOnMaxCyclesReached;
    sConfig->notifications.sendTestNotificationEnabled = doc["notifications"]["send_test_notification_enabled"] | sConfig->notifications.sendTestNotificationEnabled;

    const String previousEnrollmentToken = sConfig->central.enrollmentToken;
    sConfig->central.enabled = doc["central"]["enabled"] | sConfig->central.enabled;
    if (doc["central"]["base_urls"].is<JsonArray>()) {
      sConfig->central.baseUrls.clear();
      for (JsonVariant v : doc["central"]["base_urls"].as<JsonArray>()) sConfig->central.baseUrls.push_back(String((const char*)v));
    } else if (doc["central"]["base_url"].is<const char*>()) {
      sConfig->central.baseUrls.clear();
      sConfig->central.baseUrls.push_back(String((const char*)doc["central"]["base_url"]));
    }
    sConfig->central.enrollmentToken = doc["central"]["enrollment_token"] | sConfig->central.enrollmentToken;
    sConfig->central.deviceAlias = doc["central"]["device_alias"] | sConfig->central.deviceAlias;
    sConfig->central.siteId = doc["central"]["site_id"] | sConfig->central.siteId;
    sConfig->central.deviceId = doc["central"]["device_id"] | sConfig->central.deviceId;
    sConfig->central.deviceToken = doc["central"]["device_token"] | sConfig->central.deviceToken;
    sConfig->central.pollIntervalSeconds = doc["central"]["poll_interval_seconds"] | sConfig->central.pollIntervalSeconds;
    sConfig->central.heartbeatIntervalSeconds = doc["central"]["heartbeat_interval_seconds"] | sConfig->central.heartbeatIntervalSeconds;
    sConfig->power.enabled = doc["power"]["enabled"] | sConfig->power.enabled;
    sConfig->power.sampleRateHz = doc["power"]["sample_rate_hz"] | sConfig->power.sampleRateHz;
    sConfig->power.batchSeconds = doc["power"]["batch_seconds"] | sConfig->power.batchSeconds;
    sConfig->power.includeWifiStats = doc["power"]["include_wifi_stats"] | sConfig->power.includeWifiStats;
    // include_frequency intentionally not accepted: no real frequency value is ever produced.

    sConfig->discovery.mdnsEnabled = doc["discovery"]["mdns_enabled"] | sConfig->discovery.mdnsEnabled;
    sConfig->discovery.udpAnnounceEnabled = doc["discovery"]["udp_announce_enabled"] | sConfig->discovery.udpAnnounceEnabled;
    sConfig->discovery.udpPort = doc["discovery"]["udp_port"] | sConfig->discovery.udpPort;

    if (!sConfig->central.enrollmentToken.isEmpty() && sConfig->central.enrollmentToken != previousEnrollmentToken) {
      sConfig->central.deviceId = "";
      sConfig->central.deviceToken = "";
    }

    sCfgMgr->save(*sConfig);
    sMonitor->resetIncident();
    sEventLog->add("config", "Configuration saved by API");
    server.send(200, "application/json", "{\"ok\":true}");
  });


  server.on("/api/wifi/scan", HTTP_POST, []() {
    if (!requireAuth()) return;
    if (!sWifi) {
      server.send(503, "application/json", "{\"error\":\"wifi service unavailable\"}");
      return;
    }
    // Scanning briefly disrupts the link; it is gated behind an explicit
    // protected action and runs synchronously, freeing the result immediately.
    String networks = sWifi->scanNetworksJson();
    String out = "{\"networks\":";
    out += networks;
    out += "}";
    server.send(200, "application/json", out);
  });
  server.on("/api/wifi/scan", HTTP_GET, []() { sendMethodNotAllowed("POST"); });

  server.on("/api/system/reboot", HTTP_POST, []() {
    if (!requireAuth()) return;
    sEventLog->add("system", "Reboot requested by API");
    sEventLog->flush();
    sCfgMgr->prepareForPlannedRestart("api_reboot");
    server.send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
    safeRestartWait(100);  // 0.2.33 #205
    ESP.restart();
  });

  server.on("/api/system/recovery-boot", HTTP_POST, []() {
    if (!requireAuth()) return;
    sEventLog->add("system", "Recovery boot requested by API");
    sEventLog->flush();
    sCfgMgr->prepareForPlannedRestart("api_recovery_boot");
    sCfgMgr->requestRecoveryBoot();
    server.send(200, "application/json", "{\"ok\":true,\"rebooting\":true,\"recovery_boot\":true}");
    safeRestartWait(100);  // 0.2.33 #205
    ESP.restart();
  });

  server.on("/api/system/factory-reset", HTTP_POST, []() {
    if (!requireAuth()) return;
    sEventLog->add("system", "Factory reset requested by API");
    sEventLog->flush();
    if (sWifi) {
      sWifi->clearProvisionedCredentials();
    }
    CrashRecorder::clearStoredCrashes();
    sCfgMgr->reset();
    sCfgMgr->prepareForPlannedRestart("api_factory_reset");
    server.send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
    safeRestartWait(100);  // 0.2.33 #205
    ESP.restart();
  });

  server.on("/api/system/ota", HTTP_POST, []() {
    if (!requireAuth()) return;
    if (sOta->hasError()) {
      String body = "{\"ok\":false,\"error\":\"" + sOta->errorString() + "\"}";
      server.send(500, "application/json", body);
      return;
    }
    sCfgMgr->prepareForPlannedRestart("api_ota_finalize");
    server.send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
    safeRestartWait(250);  // 0.2.33 #205
    ESP.restart();
  }, []() {
    if (sAuth && !sAuth->isAuthorized(server)) return;
    HTTPUpload& upload = server.upload();
    sOta->handleUpload(upload);
  });

  server.on("/api/relay/on", HTTP_GET, []() { sendMethodNotAllowed("POST"); });
  server.on("/api/relay/off", HTTP_GET, []() { sendMethodNotAllowed("POST"); });
  server.on("/api/relay/toggle", HTTP_GET, []() { sendMethodNotAllowed("POST"); });
  server.on("/api/config/save", HTTP_GET, []() { sendMethodNotAllowed("POST"); });
  server.on("/api/system/reboot", HTTP_GET, []() { sendMethodNotAllowed("POST"); });
  server.on("/api/system/recovery-boot", HTTP_GET, []() { sendMethodNotAllowed("POST"); });
  server.on("/api/system/factory-reset", HTTP_GET, []() { sendMethodNotAllowed("POST"); });
  server.on("/api/system/ota", HTTP_GET, []() { sendMethodNotAllowed("POST"); });

  server.on("/favicon.ico", HTTP_GET, []() {
    server.send(204, "text/plain", "");
  });

  server.on("/style.css", HTTP_GET, []() {
    serveFileOrFallback("/style.css", "text/css", FALLBACK_STYLE_CSS);
  });

  server.on("/app.js", HTTP_GET, []() {
    serveFileOrFallback("/app.js", "application/javascript", FALLBACK_APP_JS);
  });

  server.on("/", HTTP_GET, []() {
    serveFileOrFallback("/index.html", "text/html", FALLBACK_INDEX_HTML);
  });

  server.begin();
}

void WebServerManager::loop() {
  server.handleClient();
}
