#pragma once

#include <vector>

#include <ArduinoJson.h>

#include "types.h"
#include "app_state.h"

class ConfigManager;
class EventLog;
class RelayController;

class CentralClient {
public:
  void begin(AppConfig* config, RuntimeStatus* status, ConfigManager* cfgMgr, EventLog* eventLog, RelayController* relay);
  void loop();

private:
  bool postWithFallback(const String& path, const String& authToken,
                        const String& body, String& responseBody, int& httpCode,
                        String& selectedBaseUrl);
  bool getWithFallback(const String& path, const String& authToken,
                       String& responseBody, int& httpCode,
                       String& selectedBaseUrl);
  String buildApiUrl(const String& baseUrl, const String& path) const;
  void scheduleRetry(bool rateLimited);
  void markSuccess();
  bool registerDevice();
  bool announceDevice();
  bool sendHeartbeat();
  bool pollCommands();
  bool checkFirmwareAssignment();
  bool sendPowerSamples();
  void maybeQueuePowerSample();
  bool postCommandResult(const String& commandId, const String& status,
                         const String& message, bool includeRelayState,
                         bool relayState);
  bool executeCommand(const JsonObject& cmd, String& resultStatus,
                      String& resultMessage, bool& includeRelayState,
                      bool& relayState, bool& shouldRestart);
  void persistRelayState();
  String effectiveAlias() const;
  void setState(const String& state);

  AppConfig* config_ = nullptr;
  RuntimeStatus* status_ = nullptr;
  ConfigManager* cfgMgr_ = nullptr;
  EventLog* eventLog_ = nullptr;
  RelayController* relay_ = nullptr;

  uint32_t nextAnnounceAttemptAt_ = 0;
  uint32_t nextRegisterAttemptAt_ = 0;
  uint32_t nextHeartbeatAt_ = 0;
  uint32_t nextPollAt_ = 0;
  uint32_t nextFirmwareCheckAt_ = 0;
  uint32_t nextPowerSampleAt_ = 0;
  uint32_t nextPowerUploadAt_ = 0;
  uint32_t retryBackoffMs_ = 30000;

  struct PowerSampleRecord {
    uint32_t sampledUptimeSeconds = 0;
    int16_t rssiDbm = 0;
    uint8_t sourceFlags = 0;
  };
  std::vector<PowerSampleRecord> powerSamples_;
};
