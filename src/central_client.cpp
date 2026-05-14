#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <WiFiClientSecureBearSSL.h>
#include <ArduinoJson.h>

#include "central_client.h"
#include "config_manager.h"
#include "event_log.h"
#include "firmware_version.h"
#include "relay_controller.h"
#include "status_payload.h"

namespace {
static constexpr uint32_t INITIAL_RETRY_DELAY_MS = 30000;
static constexpr uint32_t MAX_RETRY_DELAY_MS = 300000;
static constexpr uint32_t MAX_ANNOUNCE_RETRY_DELAY_MS = 60000;
static constexpr uint32_t FIRMWARE_CHECK_INTERVAL_MS = 15000;
static constexpr int HTTP_BEGIN_FAILED = -1000;
static constexpr uint32_t MIN_SUCCESS_RETRY_DELAY_MS = 30000;
static constexpr uint32_t REGISTERED_NO_TOKEN_RETRY_DELAY_MS = 300000;

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

String extractErrorCode(const String& response) {
  JsonDocument doc;
  if (deserializeJson(doc, response) != DeserializationError::Ok) {
    return "";
  }
  return String((const char*)(doc["error"]["code"] | ""));
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

namespace {
String commandModeToString(DeviceMode mode) {
  switch (mode) {
    case DeviceMode::InternetWatchdog: return "internet_watchdog";
    case DeviceMode::DeviceWatchdog: return "device_watchdog";
    default: return "smart_plug";
  }
}

DeviceMode commandModeFromString(const String& value) {
  if (value == "internet_watchdog") return DeviceMode::InternetWatchdog;
  if (value == "device_watchdog") return DeviceMode::DeviceWatchdog;
  return DeviceMode::SmartPlug;
}

RelayRestoreBehavior restoreBehaviorFromString(const String& value) {
  if (value == "always_on") return RelayRestoreBehavior::AlwaysOn;
  if (value == "always_off") return RelayRestoreBehavior::AlwaysOff;
  return RelayRestoreBehavior::RestorePrevious;
}
}

void CentralClient::begin(AppConfig* config, RuntimeStatus* status, ConfigManager* cfgMgr, EventLog* eventLog, RelayController* relay) {
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

bool CentralClient::announceDevice() {
  if (!config_ || !cfgMgr_) return false;

  if (!status_ || status_->centralState != "registered_no_token") {
    setState("announce");
  }

  JsonDocument doc;
  doc["mac_address"] = WiFi.macAddress();
  doc["hardware_model"] = "sonoff_s31";
  doc["hardware_revision"] = "v1.0";
  doc["firmware_version"] = FIRMWARE_VERSION;
  doc["local_ip"] = WiFi.localIP().toString();
  doc["display_name_hint"] = effectiveAlias();

  String body;
  serializeJson(doc, body);

  String response;
  String selectedBaseUrl;
  int code = -1;
  if (!postWithFallback("/device/announce", "", body, response, code, selectedBaseUrl)) {
    const String detail = response.isEmpty() ? String("unknown transport error") : summarizeResponse(response);
    Serial.print("Central announce transport failed: ");
    Serial.println(detail);
    if (eventLog_) eventLog_->add("central", "Announce transport failed: " + detail);
    setState("announce_transport_failed");
    scheduleRetry(false);
    nextAnnounceAttemptAt_ = millis() + min<uint32_t>(retryBackoffMs_, MAX_ANNOUNCE_RETRY_DELAY_MS);
    return false;
  }

  if (code < 200 || code >= 300) {
    if (eventLog_) eventLog_->add("central", "Announce failed (" + String(code) + "): " + summarizeResponse(response));
    setState(code == 400 ? "announce_validation_failed" : "announce_failed");
    scheduleRetry(false);
    nextAnnounceAttemptAt_ = millis() + min<uint32_t>(retryBackoffMs_, MAX_ANNOUNCE_RETRY_DELAY_MS);
    return false;
  }

  JsonDocument res;
  if (deserializeJson(res, response) != DeserializationError::Ok) {
    setState("announce_bad_json");
    nextAnnounceAttemptAt_ = millis() + 30000UL;
    return false;
  }

  JsonObject data = res["data"];
  const String status = data["status"] | "";
  const uint32_t retryAfterSeconds = data["retry_after_seconds"] | 30;
  const uint32_t retryDelayMs = max<uint32_t>(retryAfterSeconds * 1000UL, MIN_SUCCESS_RETRY_DELAY_MS);

  if (status == "adopted") {
    const String token = data["enrollment_token"] | "";
    if (token.isEmpty()) {
      if (eventLog_) eventLog_->add("central", "Announce adopted but token missing");
      setState("announce_missing_token");
      nextAnnounceAttemptAt_ = millis() + 30000UL;
      return false;
    }

    config_->central.enrollmentToken = token;
    cfgMgr_->save(*config_);
    if (eventLog_) eventLog_->add("central", "Enrollment token received via announce from " + selectedBaseUrl);
    setState("announce_adopted");
    markSuccess();
    nextRegisterAttemptAt_ = millis();
    nextAnnounceAttemptAt_ = millis() + retryDelayMs;
    return true;
  }

  if (status == "pending") {
    if (eventLog_) eventLog_->add("central", "Awaiting operator adoption via " + selectedBaseUrl);
    setState("announce_pending");
    markSuccess();
    nextAnnounceAttemptAt_ = millis() + retryDelayMs;
    return false;
  }

  if (status == "awaiting_register") {
    if (eventLog_) eventLog_->add("central", "Hub reports awaiting register");
    setState(config_->central.enrollmentToken.isEmpty() ? "awaiting_register_no_token" : "awaiting_register");
    markSuccess();
    nextRegisterAttemptAt_ = millis();
    nextAnnounceAttemptAt_ = millis() + retryDelayMs;
    return false;
  }

  if (status == "registered") {
    if (config_->central.deviceToken.isEmpty()) {
      const String token = data["enrollment_token"] | "";
      if (!token.isEmpty()) {
        config_->central.enrollmentToken = token;
        cfgMgr_->save(*config_);
        if (eventLog_) eventLog_->add("central", "Hub supplied replacement enrollment token for known device");
        setState("awaiting_register");
        markSuccess();
        nextRegisterAttemptAt_ = millis();
        nextAnnounceAttemptAt_ = millis() + retryDelayMs;
        return true;
      }

      if (eventLog_) eventLog_->add("central", "Hub reports device already registered but local device token is missing");
      setState("registered_no_token");
      markSuccess();
      nextAnnounceAttemptAt_ = millis() + max<uint32_t>(retryDelayMs, REGISTERED_NO_TOKEN_RETRY_DELAY_MS);
      return false;
    }

    if (eventLog_) eventLog_->add("central", "Hub reports device already registered");
    setState("registered");
    markSuccess();
    nextAnnounceAttemptAt_ = millis() + retryDelayMs;
    return false;
  }

  if (status == "rejected") {
    if (eventLog_) eventLog_->add("central", "Device adoption rejected by operator");
    config_->central.enrollmentToken = "";
    cfgMgr_->save(*config_);
    setState("announce_rejected");
    markSuccess();
    nextAnnounceAttemptAt_ = millis() + retryDelayMs;
    return false;
  }

  if (eventLog_) eventLog_->add("central", "Unknown announce status: " + summarizeResponse(response));
  setState("announce_unknown_status");
  nextAnnounceAttemptAt_ = millis() + 30000UL;
  return false;
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
  caps["has_relay"] = true;
  caps["can_power_cycle"] = true;
  caps["can_switch_load"] = true;
  caps["is_observe_only"] = false;
  caps["channels"] = 1;
  caps["chip_type"] = "CSE7766";
  caps["has_frequency"] = true;
  caps["integration_path"] = "rebooter_native";
  caps["supports_runtime_settings"] = true;
  caps["power_analytics"] = false;
  caps["power_analytics_transport_ready"] = true;
  caps["power_analytics_configurable"] = true;

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

  if (code == 401 || code == 403) {
    if (eventLog_) eventLog_->add("central", "Enrollment token rejected; clearing token and returning to announce");
    config_->central.enrollmentToken = "";
    cfgMgr_->save(*config_);
    setState("enrollment_rejected");
    scheduleRetry(false);
    nextAnnounceAttemptAt_ = millis() + min<uint32_t>(retryBackoffMs_, MAX_ANNOUNCE_RETRY_DELAY_MS);
    return false;
  }

  if (code == 409) {
    const String errorCode = extractErrorCode(response);
    if (errorCode == "enrollment_consumed") {
      if (eventLog_) eventLog_->add("central", "Enrollment token consumed; clearing token and returning to announce");
      config_->central.enrollmentToken = "";
      cfgMgr_->save(*config_);
      setState("enrollment_consumed");
      scheduleRetry(false);
      nextAnnounceAttemptAt_ = millis() + min<uint32_t>(retryBackoffMs_, MAX_ANNOUNCE_RETRY_DELAY_MS);
      return false;
    }
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
  StatusPayload::fillHeartbeatDocument(doc, *config_, status_, FIRMWARE_VERSION);

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
  setState("heartbeat_ok");
  markSuccess();
  return true;
}

void CentralClient::maybeQueuePowerSample() {
  if (!config_ || !status_) return;
  if (!config_->power.enabled) return;
  if (WiFi.status() != WL_CONNECTED) return;

  const uint32_t intervalMs = 1000UL / max<uint8_t>(config_->power.sampleRateHz, 1);
  const uint32_t now = millis();
  if (now < nextPowerSampleAt_) return;

  nextPowerSampleAt_ = now + intervalMs;

  PowerSampleRecord sample;
  sample.sampledUptimeSeconds = status_->uptimeSeconds;
  sample.rssiDbm = static_cast<int16_t>(WiFi.RSSI());
  sample.sourceFlags = 0x01;  // synthetic transport-validation sample
  powerSamples_.push_back(sample);

  const size_t maxBuffered = static_cast<size_t>(
      max<uint16_t>(10, min<uint16_t>(config_->power.batchSeconds * config_->power.sampleRateHz, 120)));
  if (powerSamples_.size() > maxBuffered) {
    powerSamples_.erase(powerSamples_.begin(), powerSamples_.begin() + (powerSamples_.size() - maxBuffered));
  }
}

bool CentralClient::sendPowerSamples() {
  if (!config_ || powerSamples_.empty()) return false;
  if (config_->central.deviceId.isEmpty() || config_->central.deviceToken.isEmpty()) return false;

  JsonDocument doc;
  doc["device_id"] = config_->central.deviceId;
  JsonArray samples = doc["samples"].to<JsonArray>();
  for (const auto& sample : powerSamples_) {
    JsonObject row = samples.add<JsonObject>();
    row["sampled_uptime_seconds"] = sample.sampledUptimeSeconds;
    row["source"] = "synthetic";
    row["source_flags"] = sample.sourceFlags;
    if (config_->power.includeWifiStats) {
      row["rssi_dbm"] = sample.rssiDbm;
    }
    row["chip_type"] = "CSE7766";
  }

  String body;
  serializeJson(doc, body);

  String response;
  String selectedBaseUrl;
  int code = -1;
  if (!postWithFallback("/device/power-samples", config_->central.deviceToken, body, response, code, selectedBaseUrl)) {
    if (eventLog_) eventLog_->add("central", "Power-sample transport failed: " + summarizeResponse(response));
    return false;
  }

  if (code < 200 || code >= 300) {
    if (eventLog_) eventLog_->add("central", "Power-sample upload failed (" + String(code) + "): " + summarizeResponse(response));
    return false;
  }

  if (eventLog_) {
    eventLog_->add("central", "Power-sample batch uploaded via " + selectedBaseUrl + " (" + String(powerSamples_.size()) + " samples)");
  }
  powerSamples_.clear();
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
    const String commandId = cmd["command_id"] | "";
    const String type = cmd["type"] | "unknown";
    Serial.print("Central command received: ");
    Serial.println(type);
    if (eventLog_) eventLog_->add("central_command", "Received command: " + type + " (" + commandId + ")");

    String resultStatus = "failed";
    String resultMessage = "Command execution failed";
    bool includeRelayState = false;
    bool relayState = false;
    bool shouldRestart = false;
    const bool executed = executeCommand(cmd, resultStatus, resultMessage, includeRelayState, relayState, shouldRestart);
    if (!postCommandResult(commandId, resultStatus, resultMessage, includeRelayState, relayState) && eventLog_) {
      eventLog_->add("central_command", "Failed to post command result for " + commandId + ": " + resultStatus);
    }
    if (executed && shouldRestart) {
      if (cfgMgr_) cfgMgr_->markBootHealthy();
      delay(200);
      ESP.restart();
    }
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

void CentralClient::persistRelayState() {
  if (!config_ || !cfgMgr_ || !relay_) return;
  config_->lastRelayOn = relay_->isOn();
  cfgMgr_->save(*config_);
  if (status_) status_->relayOn = relay_->isOn();
}

bool CentralClient::postCommandResult(const String& commandId, const String& status,
                                      const String& message, bool includeRelayState,
                                      bool relayState) {
  if (!config_ || config_->central.deviceId.isEmpty() || config_->central.deviceToken.isEmpty() || commandId.isEmpty()) {
    return false;
  }

  JsonDocument doc;
  doc["device_id"] = config_->central.deviceId;
  doc["command_id"] = commandId;
  doc["status"] = status;
  if (!message.isEmpty()) doc["message"] = message;
  if (includeRelayState) {
    JsonObject result = doc["result"].to<JsonObject>();
    result["relay_on"] = relayState;
  }

  String body;
  serializeJson(doc, body);
  String response;
  String selectedBaseUrl;
  int code = -1;
  if (!postWithFallback("/device/command-result", config_->central.deviceToken, body, response, code, selectedBaseUrl)) {
    if (eventLog_) eventLog_->add("central_command", "Command result transport failed: " + summarizeResponse(response));
    return false;
  }

  if (code < 200 || code >= 300) {
    if (eventLog_) eventLog_->add("central_command", "Command result rejected (" + String(code) + "): " + summarizeResponse(response));
    return false;
  }

  if (eventLog_) eventLog_->add("central_command", "Command result posted via " + selectedBaseUrl + ": " + commandId + " -> " + status);
  return true;
}

bool CentralClient::checkFirmwareAssignment() {
  if (!config_ || config_->central.deviceId.isEmpty() || config_->central.deviceToken.isEmpty()) {
    return false;
  }

  String response;
  String selectedBaseUrl;
  int code = -1;
  if (!getWithFallback("/device/firmware", config_->central.deviceToken, response, code, selectedBaseUrl)) {
    if (eventLog_) eventLog_->add("firmware", "Firmware assignment check transport failed: " + summarizeResponse(response));
    setState("firmware_check_transport_failed");
    return false;
  }

  if (code < 200 || code >= 300) {
    if (eventLog_) eventLog_->add("firmware", "Firmware assignment check failed (" + String(code) + "): " + summarizeResponse(response));
    setState("firmware_check_failed");
    return false;
  }

  JsonDocument res;
  if (deserializeJson(res, response) != DeserializationError::Ok) {
    setState("firmware_check_bad_json");
    return false;
  }

  JsonObject data = res["data"];
  const bool assigned = data["assigned"] | false;
  if (!assigned) {
    return true;
  }

  const String targetVersion = data["target_version"] | "";
  const String downloadUrl = data["download_url"] | "";
  if (targetVersion.isEmpty() || downloadUrl.isEmpty()) {
    if (eventLog_) eventLog_->add("firmware", "Firmware assignment missing target_version or download_url");
    setState("firmware_assignment_invalid");
    return false;
  }

  if (targetVersion == FIRMWARE_VERSION) {
    return true;
  }

  setState("firmware_updating");
  if (eventLog_) eventLog_->add("firmware", "Applying assigned firmware " + targetVersion + " from " + downloadUrl);

  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure());
  client->setInsecure();
  client->setBufferSizes(512, 512);
  ESPhttpUpdate.rebootOnUpdate(true);
  if (cfgMgr_) cfgMgr_->markBootHealthy();
  t_httpUpdate_return ret = ESPhttpUpdate.update(*client, downloadUrl);
  if (ret == HTTP_UPDATE_FAILED) {
    const String message = ESPhttpUpdate.getLastErrorString();
    if (eventLog_) eventLog_->add("firmware", "Assigned firmware update failed: " + message);
    setState("firmware_update_failed");
    return false;
  }

  if (ret == HTTP_UPDATE_NO_UPDATES) {
    if (eventLog_) eventLog_->add("firmware", "Assigned firmware reports no update: " + targetVersion);
    setState("firmware_no_update");
    return true;
  }

  if (cfgMgr_) cfgMgr_->markBootHealthy();
  if (eventLog_) eventLog_->add("firmware", "Assigned firmware update applied: " + targetVersion);
  return true;
}

bool CentralClient::executeCommand(const JsonObject& cmd, String& resultStatus,
                                   String& resultMessage, bool& includeRelayState,
                                   bool& relayState, bool& shouldRestart) {
  if (!config_ || !cfgMgr_ || !relay_) {
    resultStatus = "failed";
    resultMessage = "Command execution unavailable";
    return false;
  }

  const String type = cmd["type"] | "";
  JsonObject payload = cmd["payload"].as<JsonObject>();

  includeRelayState = true;
  shouldRestart = false;

  if (type == "relay_on") {
    relay_->set(true);
    persistRelayState();
    relayState = relay_->isOn();
    resultStatus = "completed";
    resultMessage = "Relay turned on";
    return true;
  }

  if (type == "relay_off") {
    relay_->set(false);
    persistRelayState();
    relayState = relay_->isOn();
    resultStatus = "completed";
    resultMessage = "Relay turned off";
    return true;
  }

  if (type == "relay_toggle") {
    relay_->toggle();
    persistRelayState();
    relayState = relay_->isOn();
    resultStatus = "completed";
    resultMessage = relayState ? "Relay toggled on" : "Relay toggled off";
    return true;
  }

  if (type == "relay_cycle") {
    const uint32_t powerOffSeconds = payload["power_off_seconds"] | 5;
    relay_->set(false);
    persistRelayState();
    delay(powerOffSeconds * 1000UL);
    relay_->set(true);
    persistRelayState();
    relayState = relay_->isOn();
    resultStatus = "completed";
    resultMessage = "Relay cycle completed";
    return true;
  }

  if (type == "device_restart") {
    includeRelayState = false;
    resultStatus = "completed";
    resultMessage = "Device restart scheduled";
    shouldRestart = true;
    return true;
  }

  if (type == "factory_reset") {
    includeRelayState = false;
    cfgMgr_->reset();
    resultStatus = "completed";
    resultMessage = "Factory reset scheduled";
    shouldRestart = true;
    return true;
  }

  if (type == "set_mode") {
    const String mode = payload["mode"] | "";
    config_->currentMode = commandModeFromString(mode);
    cfgMgr_->save(*config_);
    relayState = relay_->isOn();
    resultStatus = "completed";
    resultMessage = "Mode updated to " + commandModeToString(config_->currentMode);
    return true;
  }

  if (type == "apply_config") {
    if (payload["device_name"].is<const char*>()) config_->deviceName = String((const char*)payload["device_name"]);
    if (payload["monitor_interval_seconds"].is<uint32_t>()) config_->monitorIntervalSeconds = payload["monitor_interval_seconds"].as<uint32_t>();
    if (payload["boot_warmup_seconds"].is<uint32_t>()) config_->bootWarmupSeconds = payload["boot_warmup_seconds"].as<uint32_t>();
    if (payload["manual_button_enabled"].is<bool>()) config_->manualButtonEnabled = payload["manual_button_enabled"].as<bool>();
    if (payload["relay_restore_behavior"].is<const char*>()) {
      config_->relayRestoreBehavior = restoreBehaviorFromString(String((const char*)payload["relay_restore_behavior"]));
    }

    JsonVariantConst internet = payload["internet"];
    if (!internet.isNull()) {
      if (internet["targets"].is<JsonArrayConst>()) {
        config_->internet.targets.clear();
        for (JsonVariantConst v : internet["targets"].as<JsonArrayConst>()) {
          if (v.is<const char*>()) config_->internet.targets.push_back(String((const char*)v));
        }
      }
      if (internet["failure_threshold_seconds"].is<uint32_t>()) {
        config_->internet.failureThresholdSeconds = internet["failure_threshold_seconds"].as<uint32_t>();
      }
      if (internet["power_off_seconds"].is<uint32_t>()) {
        config_->internet.powerOffSeconds = internet["power_off_seconds"].as<uint32_t>();
      }
      if (internet["post_reboot_holdoff_seconds"].is<uint32_t>()) {
        config_->internet.postRebootHoldoffSeconds = internet["post_reboot_holdoff_seconds"].as<uint32_t>();
      }
      if (internet["max_cycles_per_incident"].is<uint32_t>()) {
        config_->internet.maxCyclesPerIncident = internet["max_cycles_per_incident"].as<uint32_t>();
      }
      if (internet["max_cycles_per_hour"].is<uint32_t>()) {
        config_->internet.maxCyclesPerHour = internet["max_cycles_per_hour"].as<uint32_t>();
      }
      if (internet["cooldown_seconds"].is<uint32_t>()) {
        config_->internet.cooldownSeconds = internet["cooldown_seconds"].as<uint32_t>();
      }
      if (internet["dns_refresh_seconds"].is<uint32_t>()) {
        config_->internet.dnsRefreshSeconds = internet["dns_refresh_seconds"].as<uint32_t>();
      }
      if (internet["recovery_stability_seconds"].is<uint32_t>()) {
        config_->internet.recoveryStabilitySeconds = internet["recovery_stability_seconds"].as<uint32_t>();
      }
    }

    JsonVariantConst device = payload["device"];
    if (!device.isNull()) {
      if (device["target"].is<const char*>()) {
        config_->device.target = String((const char*)device["target"]);
      }
      if (device["failure_threshold_seconds"].is<uint32_t>()) {
        config_->device.failureThresholdSeconds = device["failure_threshold_seconds"].as<uint32_t>();
      }
      if (device["power_off_seconds"].is<uint32_t>()) {
        config_->device.powerOffSeconds = device["power_off_seconds"].as<uint32_t>();
      }
      if (device["post_reboot_holdoff_seconds"].is<uint32_t>()) {
        config_->device.postRebootHoldoffSeconds = device["post_reboot_holdoff_seconds"].as<uint32_t>();
      }
      if (device["max_cycles_per_incident"].is<uint32_t>()) {
        config_->device.maxCyclesPerIncident = device["max_cycles_per_incident"].as<uint32_t>();
      }
      if (device["max_cycles_per_hour"].is<uint32_t>()) {
        config_->device.maxCyclesPerHour = device["max_cycles_per_hour"].as<uint32_t>();
      }
      if (device["cooldown_seconds"].is<uint32_t>()) {
        config_->device.cooldownSeconds = device["cooldown_seconds"].as<uint32_t>();
      }
      if (device["recovery_stability_seconds"].is<uint32_t>()) {
        config_->device.recoveryStabilitySeconds = device["recovery_stability_seconds"].as<uint32_t>();
      }
    }

    JsonVariantConst notifications = payload["notifications"];
    if (!notifications.isNull()) {
      if (notifications["enabled"].is<bool>()) {
        config_->notifications.enabled = notifications["enabled"].as<bool>();
      }
      if (notifications["type"].is<const char*>()) {
        config_->notifications.type = String((const char*)notifications["type"]);
      }
      if (notifications["webhook_url"].is<const char*>()) {
        config_->notifications.webhookUrl = String((const char*)notifications["webhook_url"]);
      }
      if (notifications["webhook_method"].is<const char*>()) {
        config_->notifications.webhookMethod = String((const char*)notifications["webhook_method"]);
      }
      if (notifications["webhook_auth_token"].is<const char*>()) {
        config_->notifications.webhookAuthToken = String((const char*)notifications["webhook_auth_token"]);
      }
      if (notifications["send_on_trigger"].is<bool>()) {
        config_->notifications.sendOnTrigger = notifications["send_on_trigger"].as<bool>();
      }
      if (notifications["send_on_recovery"].is<bool>()) {
        config_->notifications.sendOnRecovery = notifications["send_on_recovery"].as<bool>();
      }
      if (notifications["send_on_max_cycles_reached"].is<bool>()) {
        config_->notifications.sendOnMaxCyclesReached = notifications["send_on_max_cycles_reached"].as<bool>();
      }
      if (notifications["send_test_notification_enabled"].is<bool>()) {
        config_->notifications.sendTestNotificationEnabled = notifications["send_test_notification_enabled"].as<bool>();
      }
    }

    JsonVariantConst power = payload["power"];
    if (!power.isNull()) {
      if (power["enabled"].is<bool>()) {
        config_->power.enabled = power["enabled"].as<bool>();
      }
      if (power["sample_rate_hz"].is<uint8_t>()) {
        config_->power.sampleRateHz = power["sample_rate_hz"].as<uint8_t>();
      } else if (power["sample_rate_hz"].is<uint32_t>()) {
        config_->power.sampleRateHz = static_cast<uint8_t>(power["sample_rate_hz"].as<uint32_t>());
      }
      if (power["batch_seconds"].is<uint16_t>()) {
        config_->power.batchSeconds = power["batch_seconds"].as<uint16_t>();
      } else if (power["batch_seconds"].is<uint32_t>()) {
        config_->power.batchSeconds = static_cast<uint16_t>(power["batch_seconds"].as<uint32_t>());
      }
      if (power["include_wifi_stats"].is<bool>()) {
        config_->power.includeWifiStats = power["include_wifi_stats"].as<bool>();
      }
      if (power["include_frequency"].is<bool>()) {
        config_->power.includeFrequency = power["include_frequency"].as<bool>();
      }
    }

    cfgMgr_->save(*config_);
    relayState = relay_->isOn();
    resultStatus = "completed";
    resultMessage = "Config applied";
    return true;
  }

  if (type == "check_firmware" || type == "start_firmware_update") {
    includeRelayState = false;
    resultStatus = "failed";
    resultMessage = "Firmware update commands not implemented on device";
    return false;
  }

  resultStatus = "failed";
  resultMessage = "Unsupported command type: " + type;
  relayState = relay_->isOn();
  return false;
}

void CentralClient::loop() {
  if (!config_ || !status_) return;

  status_->centralEnabled = config_->central.enabled;
  status_->centralRegistered = !config_->central.deviceId.isEmpty() && !config_->central.deviceToken.isEmpty();
  status_->centralDeviceId = config_->central.deviceId;

  if (status_->recoveryMode) {
    setState("recovery_mode");
    return;
  }

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
  maybeQueuePowerSample();

  const bool hasDeviceToken = !config_->central.deviceId.isEmpty() && !config_->central.deviceToken.isEmpty();
  const bool hasEnrollmentToken = !config_->central.enrollmentToken.isEmpty();

  if (!hasDeviceToken) {
    if (hasEnrollmentToken) {
      if (now >= nextRegisterAttemptAt_) {
        if (!registerDevice()) nextRegisterAttemptAt_ = now + retryBackoffMs_;
      }
    } else {
      if (now >= nextAnnounceAttemptAt_) announceDevice();
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

  if (now >= nextFirmwareCheckAt_) {
    checkFirmwareAssignment();
    nextFirmwareCheckAt_ = now + FIRMWARE_CHECK_INTERVAL_MS;
  }

  if (config_->power.enabled) {
    if (nextPowerUploadAt_ == 0) nextPowerUploadAt_ = now + (config_->power.batchSeconds * 1000UL);
    if (now >= nextPowerUploadAt_) {
      if (sendPowerSamples()) nextPowerUploadAt_ = now + (config_->power.batchSeconds * 1000UL);
      else nextPowerUploadAt_ = now + 5000UL;
    }
  } else {
    powerSamples_.clear();
    nextPowerUploadAt_ = 0;
  }
}
