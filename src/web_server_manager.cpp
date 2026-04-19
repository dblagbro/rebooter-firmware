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

