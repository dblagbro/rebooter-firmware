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
#include "wifi_manager.h"

namespace {
static constexpr uint32_t INITIAL_RETRY_DELAY_MS = 30000;
static constexpr uint32_t MAX_RETRY_DELAY_MS = 300000;
static constexpr uint32_t MAX_ANNOUNCE_RETRY_DELAY_MS = 60000;
static constexpr uint32_t FIRMWARE_CHECK_INTERVAL_MS = 900000;
static constexpr uint32_t CENTRAL_ACTION_SPACING_MS = 1500;
static constexpr uint32_t INITIAL_HEARTBEAT_DELAY_MS = 2000;
static constexpr uint32_t INITIAL_POLL_DELAY_MS = 5000;
static constexpr uint32_t INITIAL_FIRMWARE_CHECK_DELAY_MS = 900000;
static constexpr int HTTP_BEGIN_FAILED = -1000;
static constexpr uint32_t MIN_SUCCESS_RETRY_DELAY_MS = 30000;
static constexpr uint32_t REGISTERED_NO_TOKEN_RETRY_DELAY_MS = 300000;
static constexpr uint32_t TRANSPORT_FAILURE_LOG_INTERVAL_MS = 120000;
static constexpr uint32_t TRANSPORT_FAILURE_SLOT_DELAY_MS = 10000;
static constexpr uint32_t TRANSPORT_FAILURE_POWER_FLOOR_MS = 60000;
static constexpr uint32_t TRANSPORT_FAILURE_FIRMWARE_FLOOR_MS = 120000;
static constexpr uint32_t REPORTED_CONFIG_STARTUP_DELAY_SECONDS = 600;
static constexpr uint32_t REPORTED_CONFIG_INTERVAL_MS = 900000;
static constexpr uint32_t COMPACT_HEARTBEAT_FREE_HEAP_THRESHOLD = 20000;
static constexpr uint32_t COMPACT_POWER_UPLOAD_FREE_HEAP_THRESHOLD = 21000;
static constexpr uint32_t COMPACT_POWER_UPLOAD_MIN_INTERVAL_MS = 60000;
// Cap on hub base URLs attempted in a single failover cycle. With up to 10
// configurable URLs, a fully-down list would otherwise do 10 sequential TLS
// handshakes per cycle, which is the most expensive heap event in the
// firmware. The rotating start point ensures every URL is still reachable
// across cycles.
static constexpr size_t MAX_ATTEMPTS_PER_CYCLE = 3;
static constexpr uint32_t COMPACT_POWER_UPLOAD_STARTUP_DELAY_MS = 30000;

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

bool looksLikeJsonEnvelope(const String& response) {
  for (size_t i = 0; i < response.length(); ++i) {
    const char ch = response[i];
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
    return ch == '{' || ch == '[';
  }
  return false;
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

void CentralClient::begin(AppConfig* config, RuntimeStatus* status, ConfigManager* cfgMgr, EventLog* eventLog,
                          RelayController* relay, WifiManagerService* wifi) {
  config_ = config;
  status_ = status;
  cfgMgr_ = cfgMgr;
  eventLog_ = eventLog;
  relay_ = relay;
  wifi_ = wifi;
  retryBackoffMs_ = INITIAL_RETRY_DELAY_MS;
  // Already-registered devices do not need to resend the full reported
  // config blob on every boot. That 600s delayed heartbeat has become a
  // repeatable crash trigger on the ESP8266 fleet, so only new /
  // unregistered devices start with a pending config push. Fresh
  // registrations still mark this true in registerDevice().
  pendingReportedConfig_ = !(config_ && !config_->central.deviceId.isEmpty() &&
                             !config_->central.deviceToken.isEmpty());
  lastReportedConfigSentAtMs_ = 0;
  steadyStateScheduled_ = false;
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

void CentralClient::scheduleSteadyStateWork(uint32_t now) {
  nextHeartbeatAt_ = now + INITIAL_HEARTBEAT_DELAY_MS;
  nextPollAt_ = now + INITIAL_POLL_DELAY_MS;
  nextFirmwareCheckAt_ = now + INITIAL_FIRMWARE_CHECK_DELAY_MS;
  nextPowerUploadAt_ = 0;
  if (config_ && config_->power.enabled) {
    nextPowerUploadAt_ = now + powerUploadIntervalMs();
    if (shouldUseCompactPowerUpload()) {
      nextPowerUploadAt_ += COMPACT_POWER_UPLOAD_STARTUP_DELAY_MS;
    }
  }
  nextTransportSlotAt_ = now + CENTRAL_ACTION_SPACING_MS;
}

void CentralClient::scheduleTransportFailureCooldown(uint32_t now, bool rateLimited) {
  scheduleRetry(rateLimited);

  const uint32_t baseDelay = retryBackoffMs_;
  const uint32_t announceDelay = min<uint32_t>(baseDelay, MAX_ANNOUNCE_RETRY_DELAY_MS);
  const uint32_t firmwareDelay = max<uint32_t>(baseDelay, TRANSPORT_FAILURE_FIRMWARE_FLOOR_MS);
  const uint32_t powerDelay = max<uint32_t>(baseDelay, TRANSPORT_FAILURE_POWER_FLOOR_MS);

  nextAnnounceAttemptAt_ = max(nextAnnounceAttemptAt_, now + announceDelay);
  nextRegisterAttemptAt_ = max(nextRegisterAttemptAt_, now + baseDelay);
  nextHeartbeatAt_ = max(nextHeartbeatAt_, now + baseDelay);
  nextPollAt_ = max(nextPollAt_, now + baseDelay);
  nextFirmwareCheckAt_ = max(nextFirmwareCheckAt_, now + firmwareDelay);
  if (config_ && config_->power.enabled) {
    nextPowerUploadAt_ = max(nextPowerUploadAt_, now + powerDelay);
  } else {
    nextPowerUploadAt_ = 0;
  }
  nextTransportSlotAt_ = max(nextTransportSlotAt_, now + min<uint32_t>(baseDelay, TRANSPORT_FAILURE_SLOT_DELAY_MS));
}

void CentralClient::logThrottled(uint32_t& lastAtMs, const String& type, const String& message,
                                 uint32_t minIntervalMs) {
  if (!eventLog_) return;
  const uint32_t now = millis();
  if (lastAtMs != 0 && (now - lastAtMs) < minIntervalMs) return;
  lastAtMs = now;
  eventLog_->add(type, message);
}

bool CentralClient::shouldIncludeReportedConfig(uint32_t now) const {
  if (!status_ || !status_->bootHealthyMarked) return false;
  if (status_->uptimeSeconds < REPORTED_CONFIG_STARTUP_DELAY_SECONDS) return false;
  if (retryBackoffMs_ != INITIAL_RETRY_DELAY_MS) return false;
  if (pendingReportedConfig_) {
    return status_->centralLastHeartbeatSeconds > 0;
  }
  if (lastReportedConfigSentAtMs_ == 0) return false;
  return (now - lastReportedConfigSentAtMs_) >= REPORTED_CONFIG_INTERVAL_MS;
}

bool CentralClient::shouldUseCompactHeartbeat() const {
  return ESP.getFreeHeap() < COMPACT_HEARTBEAT_FREE_HEAP_THRESHOLD;
}

bool CentralClient::shouldUseCompactPowerUpload() const {
  return ESP.getFreeHeap() < COMPACT_POWER_UPLOAD_FREE_HEAP_THRESHOLD;
}

uint32_t CentralClient::powerUploadIntervalMs() const {
  if (!config_) return 60000UL;
  uint32_t intervalMs = max<uint32_t>(config_->power.batchSeconds, 1) * 1000UL;
  if (shouldUseCompactPowerUpload()) {
    intervalMs = max<uint32_t>(intervalMs, COMPACT_POWER_UPLOAD_MIN_INTERVAL_MS);
  }
  return intervalMs;
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
    logThrottled(lastAnnounceFailureLogAtMs_, "central", "Announce transport failed; backing off",
                 TRANSPORT_FAILURE_LOG_INTERVAL_MS);
    setState("announce_transport_failed");
    const uint32_t now = millis();
    scheduleTransportFailureCooldown(now, false);
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

std::vector<size_t> CentralClient::buildAttemptOrder() const {
  std::vector<size_t> order;
  if (!config_) return order;
  const size_t count = config_->central.baseUrls.size();
  if (count == 0) return order;

  size_t start = lastGoodBaseUrlIndex_;
  if (start >= count) start = 0;

  const size_t attempts = count < MAX_ATTEMPTS_PER_CYCLE ? count : MAX_ATTEMPTS_PER_CYCLE;
  for (size_t step = 0; step < attempts; ++step) {
    order.push_back((start + step) % count);
  }
  return order;
}

bool CentralClient::postWithFallback(const String& path, const String& authToken,
                                     const String& body, String& responseBody, int& httpCode,
                                     String& selectedBaseUrl) {
  if (!config_) return false;
  const std::vector<size_t> attemptOrder = buildAttemptOrder();
  if (attemptOrder.empty()) return false;

  String failureSummary;
  for (size_t idx : attemptOrder) {
    const String baseUrl = config_->central.baseUrls[idx];
    std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure());
    client->setInsecure();
    client->setBufferSizes(512, 512);
    HTTPClient http;
    const String url = buildApiUrl(baseUrl, path);
    http.setTimeout(4000);
    if (!http.begin(*client, url)) {
      httpCode = HTTP_BEGIN_FAILED;
      failureSummary = describeTransportFailure(url, httpCode);
      continue;
    }

    http.addHeader("Content-Type", "application/json");
    if (!authToken.isEmpty()) http.addHeader("Authorization", "Bearer " + authToken);

    httpCode = http.POST(body);
    responseBody = httpCode > 0 ? http.getString() : "";
    http.end();

    if (httpCode >= 200 && httpCode < 300) {
      selectedBaseUrl = baseUrl;
      lastGoodBaseUrlIndex_ = idx;
      return true;
    }

    if (httpCode == 429) {
      selectedBaseUrl = baseUrl;
      lastGoodBaseUrlIndex_ = idx;
      return true;
    }

    if (httpCode >= 400 && httpCode < 500) {
      if (looksLikeJsonEnvelope(responseBody)) {
        selectedBaseUrl = baseUrl;
        lastGoodBaseUrlIndex_ = idx;
        return true;
      }
      failureSummary = summarizeResponse(responseBody);
      continue;
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

bool CentralClient::postWithoutResponseWithFallback(const String& path, const String& authToken,
                                                    const String& body, String& responseBody,
                                                    int& httpCode, String& selectedBaseUrl) {
  if (!config_) return false;
  const std::vector<size_t> attemptOrder = buildAttemptOrder();
  if (attemptOrder.empty()) return false;

  String failureSummary;
  for (size_t idx : attemptOrder) {
    const String baseUrl = config_->central.baseUrls[idx];
    std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure());
    client->setInsecure();
    client->setBufferSizes(512, 512);
    HTTPClient http;
    const String url = buildApiUrl(baseUrl, path);
    http.setTimeout(4000);
    if (!http.begin(*client, url)) {
      httpCode = HTTP_BEGIN_FAILED;
      failureSummary = describeTransportFailure(url, httpCode);
      continue;
    }

    http.addHeader("Content-Type", "application/json");
    if (!authToken.isEmpty()) http.addHeader("Authorization", "Bearer " + authToken);

    httpCode = http.POST(body);

    if (httpCode >= 200 && httpCode < 300) {
      http.end();
      selectedBaseUrl = baseUrl;
      lastGoodBaseUrlIndex_ = idx;
      responseBody = "";
      return true;
    }

    if (httpCode == 429) {
      http.end();
      selectedBaseUrl = baseUrl;
      lastGoodBaseUrlIndex_ = idx;
      responseBody = "";
      return true;
    }

    responseBody = httpCode > 0 ? http.getString() : "";
    http.end();

    if (httpCode >= 400 && httpCode < 500) {
      if (looksLikeJsonEnvelope(responseBody)) {
        selectedBaseUrl = baseUrl;
        lastGoodBaseUrlIndex_ = idx;
        return true;
      }
      failureSummary = summarizeResponse(responseBody);
      continue;
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
  const std::vector<size_t> attemptOrder = buildAttemptOrder();
  if (attemptOrder.empty()) return false;

  String failureSummary;
  for (size_t idx : attemptOrder) {
    const String baseUrl = config_->central.baseUrls[idx];
    std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure());
    client->setInsecure();
    client->setBufferSizes(512, 512);
    HTTPClient http;
    const String url = buildApiUrl(baseUrl, path);
    http.setTimeout(4000);
    if (!http.begin(*client, url)) {
      httpCode = HTTP_BEGIN_FAILED;
      failureSummary = describeTransportFailure(url, httpCode);
      continue;
    }

    if (!authToken.isEmpty()) http.addHeader("Authorization", "Bearer " + authToken);
    httpCode = http.GET();
    responseBody = httpCode > 0 ? http.getString() : "";
    http.end();

    if (httpCode >= 200 && httpCode < 300) {
      selectedBaseUrl = baseUrl;
      lastGoodBaseUrlIndex_ = idx;
      return true;
    }

    if (httpCode == 429) {
      selectedBaseUrl = baseUrl;
      lastGoodBaseUrlIndex_ = idx;
      return true;
    }

    if (httpCode >= 400 && httpCode < 500) {
      if (looksLikeJsonEnvelope(responseBody)) {
        selectedBaseUrl = baseUrl;
        lastGoodBaseUrlIndex_ = idx;
        return true;
      }
      failureSummary = summarizeResponse(responseBody);
      continue;
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
    logThrottled(lastRegisterFailureLogAtMs_, "central", "Device registration transport failed; backing off",
                 TRANSPORT_FAILURE_LOG_INTERVAL_MS);
    setState("register_transport_failed");
    scheduleTransportFailureCooldown(millis(), false);
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
    scheduleTransportFailureCooldown(millis(), false);
    return false;
  }

  if (code == 409) {
    const String errorCode = extractErrorCode(response);
    if (errorCode == "enrollment_consumed") {
      if (eventLog_) eventLog_->add("central", "Enrollment token consumed; clearing token and returning to announce");
      config_->central.enrollmentToken = "";
      cfgMgr_->save(*config_);
      setState("enrollment_consumed");
      scheduleTransportFailureCooldown(millis(), false);
      return false;
    }
  }

  if (code < 200 || code >= 300) {
    Serial.printf("Central register failed: %d\n", code);
    Serial.println(response);
    if (eventLog_) eventLog_->add("central", "Device registration failed (" + String(code) + "): " + summarizeResponse(response));
    setState("register_failed");
    scheduleTransportFailureCooldown(millis(), false);
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
  pendingReportedConfig_ = true;
  lastReportedConfigSentAtMs_ = 0;

  status_->centralRegistered = true;
  status_->centralDeviceId = deviceId;
  if (eventLog_) eventLog_->add("central", "Device registered with central service via " + selectedBaseUrl);
  setState("registered");
  markSuccess();

  scheduleSteadyStateWork(millis());
  steadyStateScheduled_ = true;
  return true;
}

bool CentralClient::sendHeartbeat() {
  if (!config_ || config_->central.deviceId.isEmpty() || config_->central.deviceToken.isEmpty()) return false;

  setState("heartbeat");

  const uint32_t now = millis();
  const bool includeReportedConfig = shouldIncludeReportedConfig(now);
  const bool compactHeartbeat = shouldUseCompactHeartbeat();
  if (compactHeartbeat) {
    logThrottled(lastCompactHeartbeatLogAtMs_, "central",
                 "Low-heap compact heartbeat mode active",
                 TRANSPORT_FAILURE_LOG_INTERVAL_MS);
  }
  JsonDocument doc;
  StatusPayload::fillHeartbeatDocument(doc, *config_, status_, FIRMWARE_VERSION,
                                       includeReportedConfig, compactHeartbeat);

  String body;
  serializeJson(doc, body);
  String response;
  String selectedBaseUrl;
  int code = -1;
  if (!postWithFallback("/device/heartbeat", config_->central.deviceToken, body, response, code, selectedBaseUrl)) {
    const String detail = response.isEmpty() ? String("unknown transport error") : summarizeResponse(response);
    Serial.print("Central heartbeat transport failed: ");
    Serial.println(detail);
    logThrottled(lastHeartbeatFailureLogAtMs_, "central", "Heartbeat transport failed; backing off",
                 TRANSPORT_FAILURE_LOG_INTERVAL_MS);
    setState("heartbeat_transport_failed");
    scheduleTransportFailureCooldown(millis(), false);
    return false;
  }

  if (code == 429) {
    setState("heartbeat_rate_limited");
    scheduleTransportFailureCooldown(millis(), true);
    return false;
  }

  if (code == 401 || code == 403) {
    if (eventLog_) eventLog_->add("central", "Heartbeat unauthorized; clearing cached registration");
    clearCentralRegistration(config_, status_, cfgMgr_);
    setState("reauth_required");
    scheduleTransportFailureCooldown(millis(), false);
    return false;
  }

  if (code < 200 || code >= 300) {
    Serial.printf("Central heartbeat failed: %d\n", code);
    setState("heartbeat_failed");
    scheduleTransportFailureCooldown(millis(), false);
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
  if (includeReportedConfig) {
    pendingReportedConfig_ = false;
    lastReportedConfigSentAtMs_ = now;
  }
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
  sample.rssiDbm = static_cast<int16_t>(WiFi.RSSI());
  const bool hasFreshRealSample =
      status_->power.realSample &&
      status_->power.lastSampleMillis > 0 &&
      (now - status_->power.lastSampleMillis) <= 5000UL &&
      status_->power.lastSampleMillis != lastQueuedRealSampleMillis_;

  if (hasFreshRealSample) {
    sample.sampledUptimeSeconds = status_->power.lastSampleUptimeSeconds;
    sample.sampledUnixMs = status_->power.lastSampleUnixMs;
    sample.sourceFlags = status_->power.sourceFlags;
    sample.voltageV = status_->power.voltageV;
    sample.currentMa = status_->power.currentMa;
    sample.estimatedCurrentMa = status_->power.estimatedCurrentMa;
    sample.powerW = status_->power.powerW;
    sample.apparentPowerVa = status_->power.apparentPowerVa;
    sample.powerFactor = status_->power.powerFactor;
    sample.frequencyHz = status_->power.frequencyHz;
    sample.energyWh = status_->power.energyWh;
    lastQueuedRealSampleMillis_ = status_->power.lastSampleMillis;
  } else {
    sample.sampledUptimeSeconds = status_->uptimeSeconds;
    sample.sourceFlags = POWER_SAMPLE_FLAG_SYNTHETIC;
  }

  powerSamples_.push_back(sample);

  const bool compactUpload = shouldUseCompactPowerUpload();
  const size_t maxBuffered = compactUpload
      ? static_cast<size_t>(3)
      : static_cast<size_t>(
            max<uint16_t>(10, min<uint16_t>(config_->power.batchSeconds * config_->power.sampleRateHz, 120)));
  if (powerSamples_.size() > maxBuffered) {
    powerSamples_.erase(powerSamples_.begin(), powerSamples_.begin() + (powerSamples_.size() - maxBuffered));
  }
}

bool CentralClient::sendPowerSamples() {
  if (!config_ || powerSamples_.empty()) return false;
  if (config_->central.deviceId.isEmpty() || config_->central.deviceToken.isEmpty()) return false;

  const bool compactUpload = shouldUseCompactPowerUpload();
  JsonDocument doc;
  doc["device_id"] = config_->central.deviceId;
  JsonArray samples = doc["samples"].to<JsonArray>();
  const size_t startIndex = compactUpload && !powerSamples_.empty() ? (powerSamples_.size() - 1) : 0;
  for (size_t idx = startIndex; idx < powerSamples_.size(); ++idx) {
    const auto& sample = powerSamples_[idx];
    JsonObject row = samples.add<JsonObject>();
    row["sampled_uptime_seconds"] = sample.sampledUptimeSeconds;
    if (sample.sampledUnixMs > 0) {
      row["sampled_unix_ms"] = sample.sampledUnixMs;
    }
    row["source"] = (sample.sourceFlags & POWER_SAMPLE_FLAG_REAL) ? "steady" : "synthetic";
    row["source_flags"] = sample.sourceFlags;
    if (!compactUpload && config_->power.includeWifiStats) {
      row["rssi_dbm"] = sample.rssiDbm;
    }
    row["chip_type"] = "CSE7766";
    if (sample.sourceFlags & POWER_SAMPLE_FLAG_REAL) {
      if (sample.sourceFlags & POWER_SAMPLE_FLAG_VOLTAGE_VALID) {
        row["v_v"] = sample.voltageV;
      }
      row["i_ma"] = sample.currentMa;
      if (!compactUpload && (sample.sourceFlags & POWER_SAMPLE_FLAG_CURRENT_ESTIMATED)) {
        row["i_ma_est"] = sample.estimatedCurrentMa;
      }
      row["p_w"] = sample.powerW;
      if (!compactUpload) {
        row["s_va"] = sample.apparentPowerVa;
        row["pf"] = sample.powerFactor;
      }
      if (!compactUpload &&
          config_->power.includeFrequency &&
          (sample.sourceFlags & POWER_SAMPLE_FLAG_FREQUENCY_VALID)) {
        row["hz"] = sample.frequencyHz;
      }
      if (!compactUpload &&
          (sample.sourceFlags & POWER_SAMPLE_FLAG_ENERGY_VALID)) {
        row["energy_wh"] = sample.energyWh;
      }
    }
  }

  String body;
  body.reserve(static_cast<unsigned int>(measureJson(doc) + 16));
  serializeJson(doc, body);

  String response;
  String selectedBaseUrl;
  int code = -1;
  if (!postWithoutResponseWithFallback("/device/power-samples", config_->central.deviceToken, body,
                                       response, code, selectedBaseUrl)) {
    logThrottled(lastPowerFailureLogAtMs_, "central", "Power-sample transport failed; backing off",
                 TRANSPORT_FAILURE_LOG_INTERVAL_MS);
    scheduleTransportFailureCooldown(millis(), false);
    return false;
  }

  if (code < 200 || code >= 300) {
    logThrottled(lastPowerFailureLogAtMs_, "central", "Power-sample upload failed; backing off",
                 TRANSPORT_FAILURE_LOG_INTERVAL_MS);
    scheduleTransportFailureCooldown(millis(), false);
    return false;
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
    logThrottled(lastPollFailureLogAtMs_, "central", "Command poll transport failed; backing off",
                 TRANSPORT_FAILURE_LOG_INTERVAL_MS);
    setState("poll_transport_failed");
    scheduleTransportFailureCooldown(millis(), false);
    return false;
  }

  if (code == 429) {
    setState("poll_rate_limited");
    scheduleTransportFailureCooldown(millis(), true);
    return false;
  }

  if (code == 401 || code == 403) {
    if (eventLog_) eventLog_->add("central", "Command poll unauthorized; clearing cached registration");
    clearCentralRegistration(config_, status_, cfgMgr_);
    setState("reauth_required");
    scheduleTransportFailureCooldown(millis(), false);
    return false;
  }

  if (code < 200 || code >= 300) {
    Serial.printf("Central command poll failed: %d\n", code);
    setState("poll_failed");
    scheduleTransportFailureCooldown(millis(), false);
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
    String restartReason = "";
    const bool executed = executeCommand(cmd, resultStatus, resultMessage,
                                         includeRelayState, relayState,
                                         shouldRestart, restartReason);
    if (!postCommandResult(commandId, resultStatus, resultMessage, includeRelayState, relayState) && eventLog_) {
      eventLog_->add("central_command", "Failed to post command result for " + commandId + ": " + resultStatus);
    }
    if (executed && shouldRestart) {
      if (cfgMgr_) cfgMgr_->prepareForPlannedRestart(restartReason);
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
    logThrottled(lastCommandResultFailureLogAtMs_, "central_command",
                 "Command result transport failed; backing off",
                 TRANSPORT_FAILURE_LOG_INTERVAL_MS);
    scheduleTransportFailureCooldown(millis(), false);
    return false;
  }

  if (code < 200 || code >= 300) {
    logThrottled(lastCommandResultFailureLogAtMs_, "central_command",
                 "Command result rejected; backing off",
                 TRANSPORT_FAILURE_LOG_INTERVAL_MS);
    scheduleTransportFailureCooldown(millis(), false);
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
    logThrottled(lastFirmwareFailureLogAtMs_, "firmware",
                 "Firmware assignment check transport failed; backing off",
                 TRANSPORT_FAILURE_LOG_INTERVAL_MS);
    setState("firmware_check_transport_failed");
    scheduleTransportFailureCooldown(millis(), false);
    return false;
  }

  if (code < 200 || code >= 300) {
    logThrottled(lastFirmwareFailureLogAtMs_, "firmware",
                 "Firmware assignment check failed; backing off",
                 TRANSPORT_FAILURE_LOG_INTERVAL_MS);
    setState("firmware_check_failed");
    scheduleTransportFailureCooldown(millis(), false);
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
  if (cfgMgr_) cfgMgr_->prepareForPlannedRestart("assigned_firmware_update");
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

  if (cfgMgr_) cfgMgr_->prepareForPlannedRestart("assigned_firmware_update_complete");
  if (eventLog_) eventLog_->add("firmware", "Assigned firmware update applied: " + targetVersion);
  return true;
}

bool CentralClient::executeCommand(const JsonObject& cmd, String& resultStatus,
                                   String& resultMessage, bool& includeRelayState,
                                   bool& relayState, bool& shouldRestart,
                                   String& restartReason) {
  if (!config_ || !cfgMgr_ || !relay_) {
    resultStatus = "failed";
    resultMessage = "Command execution unavailable";
    return false;
  }

  const String type = cmd["type"] | "";
  JsonObject payload = cmd["payload"].as<JsonObject>();

  includeRelayState = true;
  shouldRestart = false;
  restartReason = "";

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
    restartReason = "central_command_device_restart";
    return true;
  }

  if (type == "factory_reset") {
    includeRelayState = false;
    if (wifi_) {
      wifi_->clearProvisionedCredentials();
    }
    cfgMgr_->reset();
    resultStatus = "completed";
    resultMessage = "Factory reset scheduled";
    shouldRestart = true;
    restartReason = "central_command_factory_reset";
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
    nextPowerUploadAt_ = 0;
    return;
  }

  if (!status_->bootHealthyMarked) {
    setState("boot_warmup");
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
  if (now < nextTransportSlotAt_) return;

  const bool hasDeviceToken = !config_->central.deviceId.isEmpty() && !config_->central.deviceToken.isEmpty();
  const bool hasEnrollmentToken = !config_->central.enrollmentToken.isEmpty();

  if (!hasDeviceToken) {
    steadyStateScheduled_ = false;
    if (hasEnrollmentToken) {
      if (now >= nextRegisterAttemptAt_) {
        if (!registerDevice()) nextRegisterAttemptAt_ = now + retryBackoffMs_;
        nextTransportSlotAt_ = millis() + CENTRAL_ACTION_SPACING_MS;
      }
    } else {
      if (now >= nextAnnounceAttemptAt_) {
        announceDevice();
        nextTransportSlotAt_ = millis() + CENTRAL_ACTION_SPACING_MS;
      }
    }
    return;
  }

  if (!steadyStateScheduled_) {
    scheduleSteadyStateWork(now);
    steadyStateScheduled_ = true;
    setState("idle");
    return;
  }

  if (now >= nextHeartbeatAt_) {
    if (sendHeartbeat()) nextHeartbeatAt_ = now + (config_->central.heartbeatIntervalSeconds * 1000UL);
    else nextHeartbeatAt_ = now + retryBackoffMs_;
    nextTransportSlotAt_ = millis() + CENTRAL_ACTION_SPACING_MS;
    return;
  }

  if (now >= nextPollAt_) {
    if (pollCommands()) nextPollAt_ = now + (config_->central.pollIntervalSeconds * 1000UL);
    else nextPollAt_ = now + retryBackoffMs_;
    nextTransportSlotAt_ = millis() + CENTRAL_ACTION_SPACING_MS;
    return;
  }

  if (now >= nextFirmwareCheckAt_) {
    if (checkFirmwareAssignment()) {
      nextFirmwareCheckAt_ = now + FIRMWARE_CHECK_INTERVAL_MS;
    }
    nextTransportSlotAt_ = millis() + CENTRAL_ACTION_SPACING_MS;
    return;
  }

  if (config_->power.enabled) {
    if (nextPowerUploadAt_ == 0) nextPowerUploadAt_ = now + powerUploadIntervalMs();
    if (now >= nextPowerUploadAt_) {
      if (sendPowerSamples()) nextPowerUploadAt_ = now + powerUploadIntervalMs();
      nextTransportSlotAt_ = millis() + CENTRAL_ACTION_SPACING_MS;
      return;
    }
  } else {
    powerSamples_.clear();
    nextPowerUploadAt_ = 0;
  }
}
