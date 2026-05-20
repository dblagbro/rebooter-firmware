#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <ArduinoJson.h>

#include "central_client.h"
#include "config_manager.h"
#include "event_log.h"
#include "firmware_version.h"

namespace {
static constexpr uint32_t INITIAL_RETRY_DELAY_MS = 30000;
static constexpr uint32_t MAX_RETRY_DELAY_MS = 300000;

String summarizeResponse(const String& response, size_t maxLen = 180) {
  String out = response;
  out.replace("\r", " ");
  out.replace("\n", " ");
  out.trim();
  if (out.length() > maxLen) {
    out = out.substring(0, maxLen) + "...";
  }
  return out;
}

void clearCentralRegistration(AppConfig* config, RuntimeStatus* status, ConfigManager* cfgMgr) {
  if (!config) return;
  config->central.deviceId = "";
  config->central.deviceToken = "";
  if (cfgMgr) cfgMgr->save(*config);
  if (status) {
    status->centralRegistered = false;
    status->centralDeviceId = "";
  }
}
}

void CentralClient::begin(AppConfig* config, RuntimeStatus* status, ConfigManager* cfgMgr, EventLog* eventLog,
                          RelayController* relay) {
  config_ = config;
  status_ = status;
  cfgMgr_ = cfgMgr;
  eventLog_ = eventLog;
  relay_ = relay;
  retryBackoffMs_ = INITIAL_RETRY_DELAY_MS;
  if (status_) {
    status_->centralEnabled = config_ && config_->central.enabled;
    status_->centralRegistered = config_ && !config_->central.deviceId.isEmpty() && !config_->central.deviceToken.isEmpty();
    status_->centralDeviceId = config_ ? config_->central.deviceId : "";
    status_->centralState = config_ && config_->central.enabled ? "idle" : "disabled";
  }
}

void CentralClient::setState(const String& state) {
  if (!status_) return;
  if (status_->centralState == state) return;
  status_->centralState = state;
  Serial.print("Central state: ");
  Serial.println(state);
}

String CentralClient::effectiveAlias() const {
  if (!config_) return "Rebooter";
  if (!config_->central.deviceAlias.isEmpty()) return config_->central.deviceAlias;
  return config_->deviceName;
}

String CentralClient::buildApiUrl(const String& baseUrl, const String& path) const {
  String root = baseUrl;
  if (root.endsWith("/")) root.remove(root.length() - 1);
  return root + "/api/v1" + path;
}

void CentralClient::scheduleRetry(bool rateLimited) {
  if (rateLimited) {
    retryBackoffMs_ = retryBackoffMs_ < MAX_RETRY_DELAY_MS ? min<uint32_t>(retryBackoffMs_ * 2, MAX_RETRY_DELAY_MS) : MAX_RETRY_DELAY_MS;
  } else {
    retryBackoffMs_ = retryBackoffMs_ < MAX_RETRY_DELAY_MS ? min<uint32_t>(retryBackoffMs_ * 2, MAX_RETRY_DELAY_MS) : MAX_RETRY_DELAY_MS;
  }
}

void CentralClient::markSuccess() {
  retryBackoffMs_ = INITIAL_RETRY_DELAY_MS;
}

bool CentralClient::postWithFallback(const String& path, const String& authToken,
                                     const String& body, String& responseBody, int& httpCode,
                                     String& selectedBaseUrl) {
  if (!config_) return false;
  const size_t count = config_->central.baseUrls.size();
  if (count == 0) return false;

  for (size_t i = 0; i < count; ++i) {
    const String baseUrl = config_->central.baseUrls[i];
    std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure());
    client->setInsecure();
    client->setBufferSizes(1024, 1024);
    HTTPClient http;
    http.setTimeout(15000);
    const String url = buildApiUrl(baseUrl, path);
    Serial.printf("Central POST attempt: %s (heap=%u)\n", url.c_str(), ESP.getFreeHeap());
    if (!http.begin(*client, url)) {
      Serial.printf("Central POST http.begin FAILED for %s\n", url.c_str());
      if (eventLog_) eventLog_->add("central_transport", "http.begin failed: " + url);
      continue;
    }

    http.addHeader("Content-Type", "application/json");
    if (!authToken.isEmpty()) http.addHeader("Authorization", "Bearer " + authToken);

    httpCode = http.POST(body);
    responseBody = http.getString();
    http.end();

    if ((httpCode >= 200 && httpCode < 300) || httpCode == 429 || (httpCode >= 400 && httpCode < 500)) {
      selectedBaseUrl = baseUrl;
      return true;
    }

    Serial.printf("Central POST %s failed: HTTP %d\n", url.c_str(), httpCode);
    if (eventLog_) eventLog_->add("central_transport", "POST " + url + " failed: HTTP " + String(httpCode));

    if (httpCode >= 500) {
      continue;
    }
  }

  httpCode = -1;
  responseBody = "";
  selectedBaseUrl = "";
  return false;
}

bool CentralClient::getWithFallback(const String& path, const String& authToken,
                                    String& responseBody, int& httpCode,
                                    String& selectedBaseUrl) {
  if (!config_) return false;
  const size_t count = config_->central.baseUrls.size();
  if (count == 0) return false;

  for (size_t i = 0; i < count; ++i) {
    const String baseUrl = config_->central.baseUrls[i];
    std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure());
    client->setInsecure();
    client->setBufferSizes(1024, 1024);
    HTTPClient http;
    http.setTimeout(15000);
    const String url = buildApiUrl(baseUrl, path);
    Serial.printf("Central GET attempt: %s (heap=%u)\n", url.c_str(), ESP.getFreeHeap());
    if (!http.begin(*client, url)) {
      Serial.printf("Central GET http.begin FAILED for %s\n", url.c_str());
      if (eventLog_) eventLog_->add("central_transport", "http.begin failed: " + url);
      continue;
    }

    if (!authToken.isEmpty()) http.addHeader("Authorization", "Bearer " + authToken);
    httpCode = http.GET();
    responseBody = http.getString();
    http.end();

    if ((httpCode >= 200 && httpCode < 300) || httpCode == 429 || (httpCode >= 400 && httpCode < 500)) {
      selectedBaseUrl = baseUrl;
      return true;
    }

    Serial.printf("Central GET %s failed: HTTP %d\n", url.c_str(), httpCode);
    if (eventLog_) eventLog_->add("central_transport", "GET " + url + " failed: HTTP " + String(httpCode));

    if (httpCode >= 500) {
      continue;
    }
  }

  httpCode = -1;
  responseBody = "";
  selectedBaseUrl = "";
  return false;
}

bool CentralClient::registerDevice() {
  if (!config_ || !cfgMgr_) return false;
  if (config_->central.enrollmentToken.isEmpty()) {
    setState("missing_enrollment_token");
    return false;
  }

  setState("registering");

  JsonDocument doc;
  doc["enrollment_token"] = config_->central.enrollmentToken;
  doc["hardware_model"] = "sonoff_s31";
  doc["hardware_revision"] = "v1.0";
  doc["firmware_version"] = FIRMWARE_VERSION;
  doc["mac_address"] = WiFi.macAddress();
  doc["display_name"] = effectiveAlias();
  doc["local_ip"] = WiFi.localIP().toString();
  JsonObject caps = doc["capabilities"].to<JsonObject>();
  caps["local_web_ui"] = true;
  caps["local_ota"] = true;
  caps["internet_watchdog"] = true;
  caps["device_watchdog"] = true;
  caps["relay_control"] = true;

  String body;
  serializeJson(doc, body);
  String response;
  String selectedBaseUrl;
  int code = -1;
  if (!postWithFallback("/device/register", "", body, response, code, selectedBaseUrl)) {
    if (eventLog_) eventLog_->add("central", "Device registration transport failed");
    setState("register_transport_failed");
    return false;
  }

  if (code == 429) {
    if (eventLog_) eventLog_->add("central", "Device registration rate-limited");
    setState("register_rate_limited");
    scheduleRetry(true);
    return false;
  }

  if (code < 200 || code >= 300) {
    Serial.printf("Central register failed: %d\n", code);
    Serial.println(response);
    if (eventLog_) eventLog_->add("central", "Device registration failed (" + String(code) + "): " + summarizeResponse(response));
    setState("register_failed");
    scheduleRetry(false);
    return false;
  }

  JsonDocument res;
  if (deserializeJson(res, response) != DeserializationError::Ok) {
    setState("register_bad_json");
    return false;
  }

  JsonObject data = res["data"];
  const String deviceId = data["device_id"] | "";
  const String token = data["device_token"] | "";
  if (deviceId.isEmpty() || token.isEmpty()) {
    setState("register_missing_credentials");
    return false;
  }

  config_->central.deviceId = deviceId;
  config_->central.deviceToken = token;
  config_->central.pollIntervalSeconds = data["poll_interval_seconds"] | config_->central.pollIntervalSeconds;
  config_->central.heartbeatIntervalSeconds = data["heartbeat_interval_seconds"] | config_->central.heartbeatIntervalSeconds;
  cfgMgr_->save(*config_);

  status_->centralRegistered = true;
  status_->centralDeviceId = deviceId;
  if (eventLog_) eventLog_->add("central", "Device registered with central service via " + selectedBaseUrl);
  setState("registered");
  markSuccess();

  nextHeartbeatAt_ = millis();
  nextPollAt_ = millis();
  return true;
}

bool CentralClient::sendHeartbeat() {
  if (!config_ || config_->central.deviceId.isEmpty() || config_->central.deviceToken.isEmpty()) return false;

  setState("heartbeat");

  JsonDocument doc;
  doc["device_id"] = config_->central.deviceId;
  doc["firmware_version"] = FIRMWARE_VERSION;
  doc["local_ip"] = WiFi.localIP().toString();
  doc["mode"] = config_->currentMode == DeviceMode::InternetWatchdog ? "internet_watchdog" :
                 config_->currentMode == DeviceMode::DeviceWatchdog ? "device_watchdog" : "smart_plug";
  doc["relay_on"] = status_ ? status_->relayOn : true;
  doc["wifi_connected"] = status_ ? status_->wifiConnected : true;
  doc["health_state"] = status_ ? (
      status_->healthState == HealthState::Healthy ? "healthy" :
      status_->healthState == HealthState::PartialFailure ? "partial_failure" :
      status_->healthState == HealthState::Failed ? "failed" :
      status_->healthState == HealthState::Holdoff ? "holdoff" :
      status_->healthState == HealthState::Cooldown ? "cooldown" : "unknown") : "unknown";
  doc["uptime_seconds"] = status_ ? status_->uptimeSeconds : 0;
  doc["incident_cycles"] = status_ ? status_->currentIncidentCycles : 0;
  doc["hour_cycles"] = status_ ? status_->currentHourCycles : 0;
  doc["last_event_type"] = status_ ? status_->lastEvent : "boot";
  doc["last_event_at"] = "";

  String body;
  serializeJson(doc, body);
  String response;
  String selectedBaseUrl;
  int code = -1;
  if (!postWithFallback("/device/heartbeat", config_->central.deviceToken, body, response, code, selectedBaseUrl)) {
    setState("heartbeat_transport_failed");
    scheduleRetry(false);
    return false;
  }

  if (code == 429) {
    setState("heartbeat_rate_limited");
    scheduleRetry(true);
    return false;
  }

  if (code == 401 || code == 403) {
    if (eventLog_) eventLog_->add("central", "Heartbeat unauthorized; clearing cached registration");
    clearCentralRegistration(config_, status_, cfgMgr_);
    setState("reauth_required");
    scheduleRetry(false);
    nextRegisterAttemptAt_ = millis() + retryBackoffMs_;
    return false;
  }

  if (code < 200 || code >= 300) {
    Serial.printf("Central heartbeat failed: %d\n", code);
    setState("heartbeat_failed");
    scheduleRetry(false);
    return false;
  }

  JsonDocument res;
  if (deserializeJson(res, response) == DeserializationError::Ok) {
    JsonObject data = res["data"];
    config_->central.pollIntervalSeconds = data["next_poll_after_seconds"] | config_->central.pollIntervalSeconds;
    config_->central.heartbeatIntervalSeconds = data["next_heartbeat_after_seconds"] | config_->central.heartbeatIntervalSeconds;
  }

  if (status_) status_->centralLastHeartbeatSeconds = millis() / 1000;
  if (eventLog_) eventLog_->add("central", "Heartbeat accepted via " + selectedBaseUrl);
  setState("heartbeat_ok");
  markSuccess();
  return true;
}

bool CentralClient::postCommandResult(const String& commandId, const String& status,
                                      const String& message, JsonObject result) {
  if (!config_ || config_->central.deviceToken.isEmpty()) return false;

  JsonDocument doc;
  doc["device_id"] = config_->central.deviceId;
  doc["command_id"] = commandId;
  doc["status"] = status;
  doc["message"] = message;
  if (!result.isNull()) {
    doc["result"] = result;
  }

  String body;
  serializeJson(doc, body);
  String response;
  String selectedBaseUrl;
  int code = -1;
  if (!postWithFallback("/device/command-result", config_->central.deviceToken, body, response, code, selectedBaseUrl)) {
    Serial.println("Command result post transport failed");
    return false;
  }
  if (code >= 200 && code < 300) {
    Serial.printf("Command result posted for %s: %s\n", commandId.c_str(), status.c_str());
    return true;
  }
  Serial.printf("Command result post failed: %d\n", code);
  return false;
}

void CentralClient::executeCommand(const String& commandId, const String& type, JsonObject payload) {
  Serial.printf("Executing command %s: %s\n", commandId.c_str(), type.c_str());

  JsonDocument resultDoc;
  JsonObject result = resultDoc.to<JsonObject>();

  if (type == "relay_on") {
    if (relay_) {
      relay_->set(true);
      if (config_) { config_->lastRelayOn = true; if (cfgMgr_) cfgMgr_->save(*config_); }
      result["relay_on"] = true;
      if (eventLog_) eventLog_->add("relay", "Relay turned on by hub command");
      postCommandResult(commandId, "completed", "Relay on", result);
    } else {
      postCommandResult(commandId, "failed", "Relay controller not available", result);
    }

  } else if (type == "relay_off") {
    if (relay_) {
      relay_->set(false);
      if (config_) { config_->lastRelayOn = false; if (cfgMgr_) cfgMgr_->save(*config_); }
      result["relay_on"] = false;
      if (eventLog_) eventLog_->add("relay", "Relay turned off by hub command");
      postCommandResult(commandId, "completed", "Relay off", result);
    } else {
      postCommandResult(commandId, "failed", "Relay controller not available", result);
    }

  } else if (type == "relay_cycle") {
    if (relay_) {
      uint32_t offSeconds = payload["power_off_seconds"] | 5;
      relay_->set(false);
      if (eventLog_) eventLog_->add("relay", "Relay cycle: off for " + String(offSeconds) + "s (hub command)");
      postCommandResult(commandId, "running", "Relay off, waiting " + String(offSeconds) + "s", result);
      delay(offSeconds * 1000);
      relay_->set(true);
      if (config_) { config_->lastRelayOn = true; if (cfgMgr_) cfgMgr_->save(*config_); }
      result["relay_on"] = true;
      if (eventLog_) eventLog_->add("relay", "Relay cycle complete: back on");
      postCommandResult(commandId, "completed", "Relay cycle completed", result);
    } else {
      postCommandResult(commandId, "failed", "Relay controller not available", result);
    }

  } else if (type == "set_mode") {
    const String mode = payload["mode"] | "";
    if (config_ && cfgMgr_) {
      if (mode == "smart_plug") config_->currentMode = DeviceMode::SmartPlug;
      else if (mode == "internet_watchdog") config_->currentMode = DeviceMode::InternetWatchdog;
      else if (mode == "device_watchdog") config_->currentMode = DeviceMode::DeviceWatchdog;
      else {
        postCommandResult(commandId, "failed", "Unknown mode: " + mode, result);
        return;
      }
      cfgMgr_->save(*config_);
      result["mode"] = mode;
      if (eventLog_) eventLog_->add("config", "Mode set to " + mode + " by hub command");
      postCommandResult(commandId, "completed", "Mode set to " + mode, result);
    } else {
      postCommandResult(commandId, "failed", "Config not available", result);
    }

  } else if (type == "reboot") {
    if (eventLog_) eventLog_->add("system", "Reboot requested by hub command");
    postCommandResult(commandId, "completed", "Rebooting", result);
    delay(500);
    ESP.restart();

  } else if (type == "lan_scan") {
    int rangeStart = payload["start"] | 1;
    int rangeEnd = payload["end"] | 254;
    if (rangeStart < 1) rangeStart = 1;
    if (rangeEnd > 254) rangeEnd = 254;
    if (rangeEnd - rangeStart > 50) rangeEnd = rangeStart + 50;

    postCommandResult(commandId, "running", "Scanning " + String(rangeStart) + "-" + String(rangeEnd), result);

    IPAddress localIp = WiFi.localIP();
    JsonDocument scanDoc;
    JsonArray devices = scanDoc["devices"].to<JsonArray>();
    scanDoc["scanner_ip"] = localIp.toString();

    for (int i = rangeStart; i <= rangeEnd; i++) {
      IPAddress target(localIp[0], localIp[1], localIp[2], i);
      if (target == localIp) continue;
      ESP.wdtFeed();
      yield();

      WiFiClient probe;
      probe.setTimeout(1000);
      if (probe.connect(target, 80)) {
        probe.setTimeout(2000);
        probe.print("GET /api/status HTTP/1.0\r\nHost: ");
        probe.print(target.toString());
        probe.print("\r\nConnection: close\r\n\r\n");

        String body = "";
        uint32_t start = millis();
        bool inBody = false;
        String hdrBuf = "";
        while (probe.connected() && millis() - start < 3000) {
          while (probe.available()) {
            char c = probe.read();
            if (inBody) { body += c; }
            else { hdrBuf += c; if (hdrBuf.endsWith("\r\n\r\n")) { inBody = true; } }
          }
          if (body.length() > 600) break;
          yield();
        }
        probe.stop();

        JsonObject dev = devices.add<JsonObject>();
        dev["ip"] = target.toString();
        JsonDocument peer;
        if (body.length() > 2 && deserializeJson(peer, body) == DeserializationError::Ok && peer.containsKey("device_name")) {
          dev["is_rebooter"] = true;
          dev["device_name"] = peer["device_name"] | "?";
          dev["firmware_version"] = peer["firmware_version"] | "?";
          dev["mode"] = peer["mode"] | "?";
          dev["relay_on"] = peer["relay_on"] | false;
          dev["central_state"] = peer["central_state"] | "?";
        } else {
          dev["is_rebooter"] = false;
        }
      }
    }

    scanDoc["count"] = devices.size();
    // Build result from scan
    JsonDocument finalResult;
    JsonObject finalObj = finalResult.to<JsonObject>();
    finalObj["scan"] = scanDoc;
    if (eventLog_) eventLog_->add("lan", "Hub-commanded LAN scan found " + String(devices.size()) + " device(s)");
    postCommandResult(commandId, "completed", "Found " + String(devices.size()) + " device(s)", finalObj);

  } else if (type == "lan_proxy") {
    const String targetIp = payload["ip"] | "";
    const String path = payload["path"] | "/api/status";
    const String method = payload["method"] | "GET";
    const String proxyBody = payload["body"] | "";

    if (targetIp.isEmpty()) {
      postCommandResult(commandId, "failed", "ip required", result);
      return;
    }

    postCommandResult(commandId, "running", "Proxying " + method + " to " + targetIp + path, result);

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(10000);
    String url = "http://" + targetIp + path;

    if (!http.begin(client, url)) {
      result["error"] = "connection failed";
      postCommandResult(commandId, "failed", "Cannot connect to " + targetIp, result);
      return;
    }

    if (!proxyBody.isEmpty()) http.addHeader("Content-Type", "application/json");

    int httpCode = (method == "POST") ? http.POST(proxyBody) : http.GET();
    String response = http.getString();
    http.end();

    result["target_ip"] = targetIp;
    result["path"] = path;
    result["http_code"] = httpCode;
    // Try to include parsed response
    JsonDocument respDoc;
    if (response.length() < 1024 && deserializeJson(respDoc, response) == DeserializationError::Ok) {
      result["response"] = respDoc;
    } else {
      result["response_text"] = response.substring(0, 256);
    }
    if (eventLog_) eventLog_->add("lan", "Hub proxy " + method + " " + url + " -> " + String(httpCode));
    postCommandResult(commandId, "completed", "Proxy " + method + " " + targetIp + path + " -> " + String(httpCode), result);

  } else if (type == "lan_ota_push") {
    const String targetIp = payload["ip"] | "";
    const String firmwareUrl = payload["url"] | "";

    if (targetIp.isEmpty() || firmwareUrl.isEmpty()) {
      postCommandResult(commandId, "failed", "ip and url required", result);
      return;
    }

    postCommandResult(commandId, "running", "Pushing OTA pull to " + targetIp, result);

    // Tell the peer device to pull firmware from the URL
    WiFiClient client;
    HTTPClient http;
    http.setTimeout(15000);
    String url = "http://" + targetIp + "/api/system/ota-pull";

    if (!http.begin(client, url)) {
      result["error"] = "connection failed";
      postCommandResult(commandId, "failed", "Cannot connect to " + targetIp, result);
      return;
    }

    http.addHeader("Content-Type", "application/json");
    String body = "{\"url\":\"" + firmwareUrl + "\"}";
    int httpCode = http.POST(body);
    String response = http.getString();
    http.end();

    result["target_ip"] = targetIp;
    result["firmware_url"] = firmwareUrl;
    result["http_code"] = httpCode;
    if (httpCode >= 200 && httpCode < 300) {
      if (eventLog_) eventLog_->add("lan", "OTA pull command sent to " + targetIp + " -> " + String(httpCode));
      postCommandResult(commandId, "completed", "OTA pull sent to " + targetIp, result);
    } else {
      if (eventLog_) eventLog_->add("lan", "OTA pull to " + targetIp + " failed: " + String(httpCode));
      postCommandResult(commandId, "failed", "OTA pull failed: HTTP " + String(httpCode), result);
    }

  } else {
    Serial.printf("Unknown command type: %s\n", type.c_str());
    if (eventLog_) eventLog_->add("central_command", "Unknown command type: " + type);
    postCommandResult(commandId, "failed", "Unknown command type: " + type, result);
  }
}

bool CentralClient::pollCommands() {
  if (!config_ || config_->central.deviceId.isEmpty() || config_->central.deviceToken.isEmpty()) return false;

  setState("polling");

  String response;
  String selectedBaseUrl;
  int code = -1;
  if (!getWithFallback("/device/commands?device_id=" + config_->central.deviceId,
                       config_->central.deviceToken, response, code, selectedBaseUrl)) {
    setState("poll_transport_failed");
    scheduleRetry(false);
    return false;
  }

  if (code == 429) {
    setState("poll_rate_limited");
    scheduleRetry(true);
    return false;
  }

  if (code == 401 || code == 403) {
    if (eventLog_) eventLog_->add("central", "Command poll unauthorized; clearing cached registration");
    clearCentralRegistration(config_, status_, cfgMgr_);
    setState("reauth_required");
    scheduleRetry(false);
    nextRegisterAttemptAt_ = millis() + retryBackoffMs_;
    return false;
  }

  if (code < 200 || code >= 300) {
    Serial.printf("Central command poll failed: %d\n", code);
    setState("poll_failed");
    scheduleRetry(false);
    return false;
  }

  JsonDocument res;
  if (deserializeJson(res, response) != DeserializationError::Ok) {
    setState("poll_bad_json");
    return false;
  }

  JsonArray commands = res["data"]["commands"].as<JsonArray>();
  size_t count = 0;
  for (JsonObject cmd : commands) {
    count++;
    const String commandId = cmd["command_id"] | "";
    const String type = cmd["type"] | "unknown";
    JsonObject payload = cmd["payload"].as<JsonObject>();
    Serial.print("Central command received: ");
    Serial.print(type);
    Serial.print(" (");
    Serial.print(commandId);
    Serial.println(")");
    if (eventLog_) eventLog_->add("central_command", "Received command: " + type + " (" + commandId + ")");
    executeCommand(commandId, type, payload);
  }

  if (count == 0) {
    setState("idle");
  } else {
    if (eventLog_) eventLog_->add("central", "Command poll returned " + String(count) + " command(s) via " + selectedBaseUrl);
    setState("commands_received");
  }
  markSuccess();
  return true;
}

void CentralClient::loop() {
  if (!config_ || !status_) return;

  status_->centralEnabled = config_->central.enabled;
  status_->centralRegistered = !config_->central.deviceId.isEmpty() && !config_->central.deviceToken.isEmpty();
  status_->centralDeviceId = config_->central.deviceId;

  if (!config_->central.enabled) {
    setState("disabled");
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    setState("wifi_wait");
    return;
  }

  if (config_->central.baseUrls.empty()) {
    setState("missing_base_urls");
    return;
  }

  const uint32_t now = millis();

  if (config_->central.deviceId.isEmpty() || config_->central.deviceToken.isEmpty()) {
    if (now >= nextRegisterAttemptAt_) {
      if (!registerDevice()) nextRegisterAttemptAt_ = now + retryBackoffMs_;
    }
    return;
  }

  if (now >= nextHeartbeatAt_) {
    if (sendHeartbeat()) nextHeartbeatAt_ = now + (config_->central.heartbeatIntervalSeconds * 1000UL);
    else nextHeartbeatAt_ = now + retryBackoffMs_;
  }

  if (now >= nextPollAt_) {
    if (pollCommands()) nextPollAt_ = now + (config_->central.pollIntervalSeconds * 1000UL);
    else nextPollAt_ = now + retryBackoffMs_;
  }
}
