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
#include "power_monitor.h"
#include "pre_crash_breadcrumb.h"
#include "relay_controller.h"
#include "status_payload.h"
#include "wifi_manager.h"

namespace {
static constexpr uint32_t INITIAL_RETRY_DELAY_MS = 30000;
static constexpr uint32_t MAX_RETRY_DELAY_MS = 300000;
static constexpr uint32_t MAX_ANNOUNCE_RETRY_DELAY_MS = 60000;
// 0.2.9: bumped from 15min→60min. The firmware-check HTTPS call is the
// biggest contiguous BearSSL allocation (~12K), and on 0.2.7 we saw devices
// SDK-restart with empty last_planned_restart_reason after hours of uptime —
// consistent with heap fragmentation eventually failing a BearSSL alloc
// inside the SDK. Cutting this poll 4x lowers fragmentation pressure while
// we hunt the root-cause leaker (see project_rebooter_pause_state).
static constexpr uint32_t FIRMWARE_CHECK_INTERVAL_MS = 3600000;
static constexpr uint32_t CENTRAL_ACTION_SPACING_MS = 1500;
static constexpr uint32_t INITIAL_HEARTBEAT_DELAY_MS = 2000;
static constexpr uint32_t INITIAL_POLL_DELAY_MS = 5000;
static constexpr uint32_t INITIAL_FIRMWARE_CHECK_DELAY_MS = 3600000;
static constexpr int HTTP_BEGIN_FAILED = -1000;
static constexpr uint32_t MIN_SUCCESS_RETRY_DELAY_MS = 30000;
static constexpr uint32_t REGISTERED_NO_TOKEN_RETRY_DELAY_MS = 300000;
static constexpr uint32_t TRANSPORT_FAILURE_LOG_INTERVAL_MS = 120000;
// #172 / 0.2.14: proactive planned-restart thresholds. mfb is the largest
// contiguous free block — BearSSL needs ~5-7K contiguous for _sc and
// failure inside the SDK presents as system_restart_local() (ghost reboot
// with empty restart reason) or REASON_EXCEPTION_RST (.185 at 23:17:31
// UTC). Restart proactively once mfb stays below the threshold for the
// debounce window AND the device has been up long enough that we're sure
// we're not bouncing on startup pressure. Both conditions must hold.
// 0.2.24: lowered to 10000 from 0.2.22's 13000. The 13K bump was too
// aggressive given 0.2.22+0.2.23's own ~5K of new static cost (UDP
// listener buffer, RTC breadcrumb, hardened atomic-write paths) —
// post-OTA steady-state mfb dropped to 11-15K, and 13K threshold fired
// proactive restarts every ~5-10 min on healthy-trending devices.
// .190 logged 10 planned restarts in ~1h on 0.2.23 with the 13K floor.
// 10K is the new compromise: high enough to give margin above the
// ~7-8K BearSSL handshake actual-failure point, low enough not to
// fire on transient post-HTTPS dips.
static constexpr uint16_t HEAP_PRESSURE_MFB_THRESHOLD = 10000;
static constexpr uint8_t HEAP_PRESSURE_DEBOUNCE_SAMPLES = 6;  // 6 samples * 5s = 30s sustained
static constexpr uint32_t HEAP_PRESSURE_CHECK_INTERVAL_MS = 5000;
static constexpr uint32_t HEAP_PRESSURE_MIN_UPTIME_S = 1800;  // don't fire in the first 30min
// 0.2.17 sweep S6: heap samples taken within this window after an HTTPS
// call returns are biased by the BearSSL buffer's transient post-alloc
// state and are skipped to avoid false-positive heap-pressure trips.
static constexpr uint32_t HTTPS_SETTLE_WINDOW_MS = 1500;
static constexpr uint32_t TRANSPORT_FAILURE_SLOT_DELAY_MS = 10000;
static constexpr uint32_t TRANSPORT_FAILURE_FIRMWARE_FLOOR_MS = 120000;
static constexpr uint32_t REPORTED_CONFIG_STARTUP_DELAY_SECONDS = 600;
static constexpr uint32_t REPORTED_CONFIG_INTERVAL_MS = 900000;
static constexpr uint32_t COMPACT_HEARTBEAT_FREE_HEAP_THRESHOLD = 20000;
// Cap on hub base URLs attempted in a single failover cycle. With up to 10
// configurable URLs, a fully-down list would otherwise do 10 sequential TLS
// handshakes per cycle, which is the most expensive heap event in the
// firmware. The rotating start point ensures every URL is still reachable
// across cycles.
static constexpr size_t MAX_ATTEMPTS_PER_CYCLE = 3;

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
                          RelayController* relay, WifiManagerService* wifi, PowerMonitor* power) {
  config_ = config;
  status_ = status;
  cfgMgr_ = cfgMgr;
  eventLog_ = eventLog;
  relay_ = relay;
  wifi_ = wifi;
  power_ = power;
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
  nextTransportSlotAt_ = now + CENTRAL_ACTION_SPACING_MS;
}

void CentralClient::scheduleTransportFailureCooldown(uint32_t now, bool rateLimited) {
  scheduleRetry(rateLimited);

  const uint32_t baseDelay = retryBackoffMs_;
  const uint32_t announceDelay = min<uint32_t>(baseDelay, MAX_ANNOUNCE_RETRY_DELAY_MS);
  const uint32_t firmwareDelay = max<uint32_t>(baseDelay, TRANSPORT_FAILURE_FIRMWARE_FLOOR_MS);

  nextAnnounceAttemptAt_ = max(nextAnnounceAttemptAt_, now + announceDelay);
  nextRegisterAttemptAt_ = max(nextRegisterAttemptAt_, now + baseDelay);
  nextHeartbeatAt_ = max(nextHeartbeatAt_, now + baseDelay);
  nextPollAt_ = max(nextPollAt_, now + baseDelay);
  nextFirmwareCheckAt_ = max(nextFirmwareCheckAt_, now + firmwareDelay);
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

void CentralClient::maybeHeapPressureRestart() {
  if (!status_ || !cfgMgr_) return;
  const uint32_t now = millis();
  if (now - lastHeapPressureCheckMs_ < HEAP_PRESSURE_CHECK_INTERVAL_MS) return;
  lastHeapPressureCheckMs_ = now;

  // Don't bounce during startup or recovery — give the device room to
  // settle and let an operator clear a stuck state without us looping.
  if (status_->uptimeSeconds < HEAP_PRESSURE_MIN_UPTIME_S) return;
  if (status_->recoveryMode) return;

  const uint32_t mfb = ESP.getMaxFreeBlockSize();
  if (mfb >= HEAP_PRESSURE_MFB_THRESHOLD) {
    heapPressureSampleCount_ = 0;
    return;
  }
  if (heapPressureSampleCount_ < HEAP_PRESSURE_DEBOUNCE_SAMPLES) {
    heapPressureSampleCount_++;
    return;
  }
  // 0.2.16 review fix #6: dropped the `if (!pendingCommandsJson_.isEmpty()) return`
  // guard. loop() ordering called this BEFORE the deferred-drain block, so the
  // check saw stale data from the previous iteration. With the drain now
  // re-ordered to run first, the buffer is already empty by the time we get
  // here in the common case. And even if it isn't: the hub queue is idempotent,
  // a command re-delivered after the restart still executes correctly, and
  // silently suppressing the safety net was the more dangerous failure mode.

  // Sustained pressure — fire a clean planned restart with breadcrumb.
  Serial.print("Heap-pressure proactive restart: mfb=");
  Serial.print(mfb);
  Serial.print(" sustained for ");
  Serial.print(heapPressureSampleCount_ * HEAP_PRESSURE_CHECK_INTERVAL_MS / 1000);
  Serial.println("s");
  if (eventLog_) {
    eventLog_->add("system",
                   "Proactive restart: mfb=" + String(mfb) +
                       " below threshold, uptime=" + String(status_->uptimeSeconds) + "s");
    eventLog_->flush();
  }
  cfgMgr_->prepareForPlannedRestart("heap_pressure_proactive");
  // 0.2.26 CRITICAL fix (code review F6): pre-fix `delay(500)` yielded
  // to the SYS task while we were waiting for flash commit. Under the
  // exact heap pressure that triggered the restart, lwIP RX / BearSSL
  // background could `alloc-fail → REASON_EXCEPTION_RST` inside the
  // delay window — the safety net was producing more ghost reboots
  // than it prevented. .188 saw 8 Exception-class reboots / hr on
  // 0.2.23-0.2.25 sitting downstream of this code path.
  //
  // Tight busy-wait keeps the CPU on this thread (no SYS yield) while
  // feeding both watchdogs so the device doesn't WDT-reset mid-wait.
  // 500ms is preserved as the budget because LittleFS metadata commits
  // are technically async at the flash-driver layer; pre-empting the
  // settle window can leave the planned-restart breadcrumb un-persisted.
  const uint32_t restartStart = millis();
  while (millis() - restartStart < 500) {
    ESP.wdtFeed();
    delayMicroseconds(1000);  // busy-wait, no scheduler yield
  }
  ESP.restart();
}

void CentralClient::sampleHeap() {
  const uint32_t now = millis();
  if (lastHeapSampleAtMs_ != 0 && now - lastHeapSampleAtMs_ < HEAP_SAMPLE_INTERVAL_MS) return;
  // 0.2.17 sweep S6: skip sampling inside the post-HTTPS settle window.
  // During the ~12K BearSSL handshake, ESP.getMaxFreeBlockSize() reports
  // the transient post-alloc value, not the steady-state baseline. If
  // the heap-pressure debounce (6 samples) catches us on a burst of
  // HTTPS calls — one sample per loop tick right after each return —
  // it would falsely trip the proactive restart on devices whose
  // BASELINE mfb is healthy. lastHttpsCompletedAtMs_ is bumped by
  // postWithFallback/getWithFallback when they finish.
  if (lastHttpsCompletedAtMs_ != 0 && now - lastHttpsCompletedAtMs_ < HTTPS_SETTLE_WINDOW_MS) return;
  lastHeapSampleAtMs_ = now;
  HeapSample& s = heapRing_[heapRingHead_];
  s.uptime_s = status_ ? status_->uptimeSeconds : (now / 1000UL);
  s.free_heap = static_cast<uint16_t>(min<uint32_t>(ESP.getFreeHeap(), 65535));
  s.max_free_block = static_cast<uint16_t>(min<uint32_t>(ESP.getMaxFreeBlockSize(), 65535));
  s.frag_pct = static_cast<uint8_t>(min<uint32_t>(ESP.getHeapFragmentation(), 255));
  heapRingHead_ = (heapRingHead_ + 1) % HEAP_RING_SIZE;
  if (heapRingCount_ < HEAP_RING_SIZE) heapRingCount_++;
}

void CentralClient::serializeHeapTrajectory(JsonDocument& doc) {
  if (heapRingCount_ == 0) return;
  JsonArray arr = doc["heap_trajectory"].to<JsonArray>();
  const uint8_t start = (heapRingHead_ + HEAP_RING_SIZE - heapRingCount_) % HEAP_RING_SIZE;
  for (uint8_t i = 0; i < heapRingCount_; ++i) {
    const HeapSample& s = heapRing_[(start + i) % HEAP_RING_SIZE];
    JsonObject e = arr.add<JsonObject>();
    e["up"] = s.uptime_s;
    e["fh"] = s.free_heap;
    e["mfb"] = s.max_free_block;
    e["fp"] = s.frag_pct;
  }
  // 0.2.17 sweep S8: reset the ring after flushing. Without this, the
  // 12-slot × 5s ring fills in ~60s — the same cadence as heartbeats —
  // so the next heartbeat re-ships up to all 12 samples it already
  // shipped (different received_at, same `up` values). Hub stored each
  // sample 2-3x and any chart that flattens trajectories by `up` showed
  // duplicate points. New samples are appended from the head; next
  // heartbeat ships only what was captured in the intervening window.
  heapRingCount_ = 0;
  heapRingHead_ = 0;
}

String CentralClient::buildApiUrl(const String& baseUrl, const String& path) const {
  String root = baseUrl;
  if (root.endsWith("/")) root.remove(root.length() - 1);
  return root + "/api/v1" + path;
}

bool CentralClient::announceDevice() {
  PreCrashBreadcrumb::Scope _bc(PreCrashBreadcrumb::OP_HTTPS_ANNOUNCE);
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

  // 0.2.17 sweep S6: stamp the HTTPS-settle window at call-START. loop()
  // doesn't tick during the blocking http.POST so the next sampleHeap
  // fires only after we return; the window then covers ~1.5s of settle
  // for the BearSSL teardown so the captured mfb reflects baseline,
  // not the transient post-handshake low.
  lastHttpsCompletedAtMs_ = millis();

  String failureSummary;
  for (size_t idx : attemptOrder) {
    const String baseUrl = config_->central.baseUrls[idx];
    // 0.2.5: per-call BearSSL client. The 0.2.3 long-lived pool was reverted —
    // it cost ~13K standing heap and leaked ~50-100 B/min (the pinned client
    // accumulated state across reuses), crashing devices every 15-40 min
    // regardless of frame flow. Per-call alloc is freed on scope exit
    // (unique_ptr -> _ctx=nullptr -> _freeSSL), which 8.6h of stable-heap
    // 0.2.0 runtime on .188 proved leak-free.
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

    // 0.2.4: feed both watchdogs right before the blocking TLS+POST.
    // Under low free-heap the BearSSL handshake can take several seconds;
    // without this, the soft-WDT (default ~3.5s) can fire mid-handshake and
    // reset the chip with a "Software/System restart" that bypasses our
    // planned-restart breadcrumb path.
    ESP.wdtFeed();
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
  lastHttpsCompletedAtMs_ = millis();  // 0.2.17 sweep S6: see postWithFallback

  String failureSummary;
  for (size_t idx : attemptOrder) {
    const String baseUrl = config_->central.baseUrls[idx];
    // 0.2.5: per-call BearSSL client. The 0.2.3 long-lived pool was reverted —
    // it cost ~13K standing heap and leaked ~50-100 B/min (the pinned client
    // accumulated state across reuses), crashing devices every 15-40 min
    // regardless of frame flow. Per-call alloc is freed on scope exit
    // (unique_ptr -> _ctx=nullptr -> _freeSSL), which 8.6h of stable-heap
    // 0.2.0 runtime on .188 proved leak-free.
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

    // 0.2.4: see notes in postWithFallback — feed WDT before TLS+POST.
    ESP.wdtFeed();
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
  lastHttpsCompletedAtMs_ = millis();  // 0.2.17 sweep S6: see postWithFallback

  String failureSummary;
  for (size_t idx : attemptOrder) {
    const String baseUrl = config_->central.baseUrls[idx];
    // 0.2.5: per-call BearSSL client. The 0.2.3 long-lived pool was reverted —
    // it cost ~13K standing heap and leaked ~50-100 B/min (the pinned client
    // accumulated state across reuses), crashing devices every 15-40 min
    // regardless of frame flow. Per-call alloc is freed on scope exit
    // (unique_ptr -> _ctx=nullptr -> _freeSSL), which 8.6h of stable-heap
    // 0.2.0 runtime on .188 proved leak-free.
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
    // 0.2.4: see notes in postWithFallback — feed WDT before TLS+GET.
    ESP.wdtFeed();
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
  PreCrashBreadcrumb::Scope _bc(PreCrashBreadcrumb::OP_HTTPS_REGISTER);
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
  PreCrashBreadcrumb::Scope _bc(PreCrashBreadcrumb::OP_HTTPS_HEARTBEAT);
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
  // 0.2.10: flush the heap-trajectory ring into the heartbeat envelope.
  // Adds ~30 bytes/sample × up to 12 samples = ~360 bytes when full —
  // negligible vs the existing heartbeat body.
  // 0.2.16 review fix #10: do this even in compact mode. Compact mode
  // triggers precisely when free_heap is low (≤20K) — exactly the
  // regime where fragmentation forensics matter most. The hub's
  // live-detail panel reads heap fields ONLY from heap_trajectory[-1],
  // so skipping the array in compact mode left devices invisible on
  // the dashboard at the moment they needed surfacing.
  serializeHeapTrajectory(doc);

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
    // #170 / 0.2.13: command piggyback (deferred). Capture the
    // pending_commands array as a raw JSON string. We CANNOT execute
    // here because postCommandResult would nest another BearSSL
    // handshake inside this heartbeat's stack (doc + body + response +
    // res still live → peak heap exceeds budget → severe fragmentation).
    // Instead we hand it to the loop() tick that follows, which fires
    // after sendHeartbeat() fully returns and all its locals are freed.
    JsonArray commands = data["pending_commands"].as<JsonArray>();
    if (!commands.isNull() && commands.size() > 0) {
      pendingCommandsJson_ = "";
      serializeJson(commands, pendingCommandsJson_);
    }
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

bool CentralClient::pollCommands() {
  PreCrashBreadcrumb::Scope _bc(PreCrashBreadcrumb::OP_HTTPS_FETCH_COMMANDS);
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
  size_t count = processCommandsArray(commands, "poll via " + selectedBaseUrl);
  if (count == 0) {
    setState("idle");
  } else {
    setState("commands_received");
  }
  markSuccess();
  return true;
}

size_t CentralClient::processCommandsArray(JsonArray commands, const String& sourceLabel) {
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
  if (count > 0 && eventLog_) {
    eventLog_->add("central", "Processed " + String(count) + " command(s) via " + sourceLabel);
  }
  return count;
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
  PreCrashBreadcrumb::Scope _bc(PreCrashBreadcrumb::OP_HTTPS_POST_RESULT);
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

  // 0.2.5: per-call BearSSL client (pool reverted — see postWithFallback note).
  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure());
  client->setInsecure();
  client->setBufferSizes(512, 512);
  ESPhttpUpdate.rebootOnUpdate(true);
  // 0.2.17 sweep S5: prepareForPlannedRestart MUST fire before update()
  // because ESPhttpUpdate.update() with rebootOnUpdate(true) calls
  // ESP.restart() internally on success — no code after a successful
  // return executes. But on HTTP_UPDATE_FAILED or HTTP_UPDATE_NO_UPDATES
  // the flag stays set on a device that never restarts, and after the
  // 0.2.10 #164 fix surfaces the reason on every next boot, the device
  // misattributes its next ghost reboot as 'assigned_firmware_update'.
  // Clear on each non-success exit.
  if (cfgMgr_) cfgMgr_->prepareForPlannedRestart("assigned_firmware_update");
  t_httpUpdate_return ret = ESPhttpUpdate.update(*client, downloadUrl);
  if (ret == HTTP_UPDATE_FAILED) {
    if (cfgMgr_) cfgMgr_->clearPlannedRestart();
    const String message = ESPhttpUpdate.getLastErrorString();
    if (eventLog_) eventLog_->add("firmware", "Assigned firmware update failed: " + message);
    setState("firmware_update_failed");
    return false;
  }

  if (ret == HTTP_UPDATE_NO_UPDATES) {
    if (cfgMgr_) cfgMgr_->clearPlannedRestart();
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
    // 0.2.19 (#197): leave a permanent breadcrumb BEFORE clearing WiFi.
    // The button + API paths already log; this hub-command path didn't —
    // so a forensic look at /api/events couldn't distinguish "operator
    // ran factory_reset from the hub" from "WiFi creds vanished from
    // LittleFS corruption" (the .185 06-06 case). With the breadcrumb
    // in place, the next time creds disappear without a matching log
    // entry, we know it was corruption, not a user-initiated reset.
    if (eventLog_) {
      eventLog_->add("system", "Factory reset requested by hub command");
      eventLog_->flush();
    }
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
      // include_frequency is intentionally not applied: the CSE7766 path
      // never produces a real mains-frequency value, so the field is kept
      // off by validateConfig and not advertised as a capability.
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

  sampleHeap();
  // 0.2.16 review fix #6: maybeHeapPressureRestart MUST run on every
  // tick regardless of central state — fragmentation creeps during
  // WiFi-down or boot-warmup too, so we can't gate it on the guards
  // below. The in-flight `pendingCommandsJson_` check that was here
  // before is gone (see comment in the function body) so ordering is
  // no longer load-bearing for correctness.
  maybeHeapPressureRestart();

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

  if (!status_->bootHealthyMarked) {
    setState("boot_warmup");
    return;
  }

  // 0.2.16 review fix #5: drain deferred piggybacked commands AFTER the
  // recoveryMode / central.enabled / bootHealthy guards. Pre-fix this
  // block ran first, which meant a recovery-mode device would still
  // execute (and HTTPS-post results for) the prior heartbeat's queued
  // commands, undermining the recovery-mode contract. The heap-nesting
  // reason for deferral (heartbeat's JsonDoc/String state still alive)
  // is unchanged — drain still runs ONE loop tick after sendHeartbeat
  // returned, so all that state has been freed.
  if (!pendingCommandsJson_.isEmpty()) {
    String pending = pendingCommandsJson_;
    pendingCommandsJson_ = "";
    JsonDocument doc;
    if (deserializeJson(doc, pending) == DeserializationError::Ok) {
      // 0.2.16 review fix #8: align with pollCommands() — set the
      // transport state and refresh the success-bookkeeping so
      // central_state advances and retry-backoff acks the working
      // channel. Pre-fix the deferred path skipped both, so a device
      // whose commands only ever arrived via piggyback reported
      // central_state='heartbeat_ok' forever.
      const size_t n = processCommandsArray(doc.as<JsonArray>(), "heartbeat (deferred)");
      setState(n > 0 ? "commands_received" : "idle");
      markSuccess();
    } else if (eventLog_) {
      eventLog_->add("central_command", "Failed to parse deferred pending_commands JSON");
    }
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
    if (sendHeartbeat()) {
      nextHeartbeatAt_ = now + (config_->central.heartbeatIntervalSeconds * 1000UL);
      // Power telemetry rides the heartbeat; once a cycle's aggregate has
      // been reported, start a fresh rolling window so min/avg/max reflect
      // the next interval rather than accumulating forever.
      if (power_ && config_->power.enabled) power_->resetAggregate();
    } else {
      nextHeartbeatAt_ = now + retryBackoffMs_;
    }
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
}
