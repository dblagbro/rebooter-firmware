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
};
