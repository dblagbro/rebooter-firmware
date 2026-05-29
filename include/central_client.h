#pragma once

#include <memory>
#include <vector>

#include <ArduinoJson.h>
#include <WiFiClientSecureBearSSL.h>

#include "types.h"
#include "app_state.h"

class ConfigManager;
class EventLog;
class RelayController;
class WifiManagerService;
class PowerMonitor;

class CentralClient {
public:
  void begin(AppConfig* config, RuntimeStatus* status, ConfigManager* cfgMgr, EventLog* eventLog,
             RelayController* relay, WifiManagerService* wifi, PowerMonitor* power);
  void loop();

private:
  bool postWithFallback(const String& path, const String& authToken,
                        const String& body, String& responseBody, int& httpCode,
                        String& selectedBaseUrl);
  bool postWithoutResponseWithFallback(const String& path, const String& authToken,
                                       const String& body, String& responseBody, int& httpCode,
                                       String& selectedBaseUrl);
  bool getWithFallback(const String& path, const String& authToken,
                       String& responseBody, int& httpCode,
                       String& selectedBaseUrl);
  String buildApiUrl(const String& baseUrl, const String& path) const;
  // Builds the ordered list of base-URL indices to attempt this cycle:
  // starts at lastGoodBaseUrlIndex_ and wraps, capped at MAX_ATTEMPTS_PER_CYCLE
  // so a fully-down 10-entry list never does 10 TLS handshakes back-to-back.
  std::vector<size_t> buildAttemptOrder() const;
  void scheduleRetry(bool rateLimited);
  void markSuccess();
  bool registerDevice();
  bool announceDevice();
  bool sendHeartbeat();
  bool pollCommands();
  bool checkFirmwareAssignment();
  bool postCommandResult(const String& commandId, const String& status,
                         const String& message, bool includeRelayState,
                         bool relayState);
  bool executeCommand(const JsonObject& cmd, String& resultStatus,
                      String& resultMessage, bool& includeRelayState,
                      bool& relayState, bool& shouldRestart,
                      String& restartReason);
  // #170 / 0.2.11: execute a commands array delivered either via the
  // /device/commands poll or piggybacked on the heartbeat response.
  // Returns count of commands processed; ESP.restart()s mid-iteration
  // if any command sets shouldRestart=true.
  size_t processCommandsArray(JsonArray commands, const String& sourceLabel);
  void persistRelayState();
  String effectiveAlias() const;
  void setState(const String& state);
  void scheduleSteadyStateWork(uint32_t now);
  void scheduleTransportFailureCooldown(uint32_t now, bool rateLimited);
  void logThrottled(uint32_t& lastAtMs, const String& type, const String& message,
                    uint32_t minIntervalMs);
  bool shouldIncludeReportedConfig(uint32_t now) const;
  bool shouldUseCompactHeartbeat() const;

  AppConfig* config_ = nullptr;
  RuntimeStatus* status_ = nullptr;
  ConfigManager* cfgMgr_ = nullptr;
  EventLog* eventLog_ = nullptr;
  RelayController* relay_ = nullptr;
  WifiManagerService* wifi_ = nullptr;
  PowerMonitor* power_ = nullptr;

  uint32_t nextAnnounceAttemptAt_ = 0;
  uint32_t nextRegisterAttemptAt_ = 0;
  uint32_t nextHeartbeatAt_ = 0;
  uint32_t nextPollAt_ = 0;
  uint32_t nextFirmwareCheckAt_ = 0;
  uint32_t nextTransportSlotAt_ = 0;
  uint32_t lastReportedConfigSentAtMs_ = 0;
  uint32_t retryBackoffMs_ = 30000;
  // Index of the hub base URL that last produced a usable response. Failover
  // iteration starts here so a healthy hub is reached in a single TLS
  // handshake on the common path instead of always retrying from index 0.
  size_t lastGoodBaseUrlIndex_ = 0;
  uint32_t lastAnnounceFailureLogAtMs_ = 0;
  uint32_t lastRegisterFailureLogAtMs_ = 0;
  uint32_t lastHeartbeatFailureLogAtMs_ = 0;
  uint32_t lastPollFailureLogAtMs_ = 0;
  uint32_t lastFirmwareFailureLogAtMs_ = 0;
  uint32_t lastCommandResultFailureLogAtMs_ = 0;
  uint32_t lastCompactHeartbeatLogAtMs_ = 0;
  bool pendingReportedConfig_ = true;
  bool steadyStateScheduled_ = false;

  // #170 / 0.2.13: pending-commands JSON captured from a heartbeat
  // response, deferred to a later loop tick. 0.2.12 (reverted) tried
  // to execute commands INSIDE the heartbeat-response handler — but
  // postCommandResult inside that scope nests a second BearSSL ~12K
  // alloc while the heartbeat's JsonDoc + body String + response
  // String + res JsonDoc (with trajectory + power summary) are still
  // alive on the heap. That doubled peak heap pressure and fragmented
  // .190 from 22K→10K mfb within one heartbeat cycle, ghost-rebooting
  // it every ~150s vs 1h+ on per-call BearSSL alone. Deferral lets
  // the heartbeat fully exit (all its String+JsonDoc state freed)
  // before postCommandResult fires. Bounded — only ever holds the
  // most recent heartbeat's pending list; new arrivals overwrite.
  String pendingCommandsJson_;

  // 0.2.10: heap-trajectory ring. Sampled every HEAP_SAMPLE_INTERVAL_MS in
  // loop(), flushed into the heartbeat JSON as heap_trajectory: [...].
  // Streams 5s-resolution heap data to the hub via the existing HTTPS call,
  // so fragmentation creep is visible even when free_heap stays flat.
  struct HeapSample {
    uint32_t uptime_s;
    uint16_t free_heap;
    uint16_t max_free_block;
    uint8_t frag_pct;
  };
  static constexpr size_t HEAP_RING_SIZE = 12;
  static constexpr uint32_t HEAP_SAMPLE_INTERVAL_MS = 5000;
  HeapSample heapRing_[HEAP_RING_SIZE];
  uint8_t heapRingHead_ = 0;
  uint8_t heapRingCount_ = 0;
  uint32_t lastHeapSampleAtMs_ = 0;
  void sampleHeap();
  void serializeHeapTrajectory(JsonDocument& doc) const;
};
