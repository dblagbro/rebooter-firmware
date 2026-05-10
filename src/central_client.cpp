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
static constexpr int HTTP_BEGIN_FAILED = -1000;

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

String describeTransportFailure(const String& url, int code) {
  String message = url;
  message += " -> ";
  message += String(code);
  if (code == HTTP_BEGIN_FAILED) {
    message += " (http_begin_failed)";
  } else if (code < 0) {
    message += " (";
    message += HTTPClient::errorToString(code);
    message += ")";
  }
  message += ", free_heap=";
  message += ESP.getFreeHeap();
  return message;
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

void CentralClient::begin(AppConfig* config, RuntimeStatus* status, ConfigManager* cfgMgr, EventLog* eventLog) {
  config_ = config;
  status_ = status;
  cfgMgr_ = cfgMgr;
  eventLog_ = eventLog;
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

  String failureSummary;
  for (size_t i = 0; i < count; ++i) {
    const String baseUrl = config_->central.baseUrls[i];
    std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure());
    client->setInsecure();
    client->setBufferSizes(512, 512);
    HTTPClient http;
    const String url = buildApiUrl(baseUrl, path);
    if (!http.begin(*client, url)) {
      httpCode = HTTP_BEGIN_FAILED;
      failureSummary = describeTransportFailure(url, httpCode);
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

    if (httpCode >= 500) {
      continue;
    }

    failureSummary = describeTransportFailure(url, httpCode);
  }

  responseBody = failureSummary;
  selectedBaseUrl = "";
  return false;
}

bool CentralClient::getWithFallback(const String& path, const String& authToken,
                                    String& responseBody, int& httpCode,
                                    String& selectedBaseUrl) {
  if (!config_) return false;
  const size_t count = config_->central.baseUrls.size();
  if (count == 0) return false;

  String failureSummary;
  for (size_t i = 0; i < count; ++i) {
    const String baseUrl = config_->central.baseUrls[i];
    std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure());
    client->setInsecure();
    client->setBufferSizes(512, 512);
    HTTPClient http;
    const String url = buildApiUrl(baseUrl, path);
    if (!http.begin(*client, url)) {
      httpCode = HTTP_BEGIN_FAILED;
      failureSummary = describeTransportFailure(url, httpCode);
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

    if (httpCode >= 500) {
      continue;
    }

    failureSummary = describeTransportFailure(url, httpCode);
  }

  responseBody = failureSummary;
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
    const String detail = response.isEmpty() ? String("unknown transport error") : summarizeResponse(response);
    Serial.print("Central register transport failed: ");
    Serial.println(detail);
    if (eventLog_) eventLog_->add("central", "Device registration transport failed: " + detail);
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
    const String detail = response.isEmpty() ? String("unknown transport error") : summarizeResponse(response);
    Serial.print("Central heartbeat transport failed: ");
    Serial.println(detail);
    if (eventLog_) eventLog_->add("central", "Heartbeat transport failed: " + detail);
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

bool CentralClient::pollCommands() {
  if (!config_ || config_->central.deviceId.isEmpty() || config_->central.deviceToken.isEmpty()) return false;

  setState("polling");

  String response;
  String selectedBaseUrl;
  int code = -1;
  if (!getWithFallback("/device/commands?device_id=" + config_->central.deviceId,
                       config_->central.deviceToken, response, code, selectedBaseUrl)) {
    const String detail = response.isEmpty() ? String("unknown transport error") : summarizeResponse(response);
    Serial.print("Central command poll transport failed: ");
    Serial.println(detail);
    if (eventLog_) eventLog_->add("central", "Command poll transport failed: " + detail);
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
    const String type = cmd["type"] | "unknown";
    Serial.print("Central command received: ");
    Serial.println(type);
    if (eventLog_) eventLog_->add("central_command", "Received command: " + type);
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
