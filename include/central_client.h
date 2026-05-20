#pragma once

#include <ArduinoJson.h>
#include "types.h"
#include "app_state.h"
#include "relay_controller.h"

class ConfigManager;
class EventLog;

class CentralClient {
public:
  void begin(AppConfig* config, RuntimeStatus* status, ConfigManager* cfgMgr, EventLog* eventLog,
             RelayController* relay = nullptr);
  void loop();

private:
  void executeCommand(const String& commandId, const String& type, JsonObject payload);
  bool postCommandResult(const String& commandId, const String& status,
                         const String& message, JsonObject result);
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
  bool sendHeartbeat();
  bool pollCommands();
  String effectiveAlias() const;
  void setState(const String& state);

  AppConfig* config_ = nullptr;
  RuntimeStatus* status_ = nullptr;
  ConfigManager* cfgMgr_ = nullptr;
  EventLog* eventLog_ = nullptr;
  RelayController* relay_ = nullptr;

  uint32_t nextRegisterAttemptAt_ = 0;
  uint32_t nextHeartbeatAt_ = 0;
  uint32_t nextPollAt_ = 0;
  uint32_t retryBackoffMs_ = 30000;
};
