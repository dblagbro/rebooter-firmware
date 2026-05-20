#pragma once

#include "types.h"
#include "app_state.h"

class RelayController;
class ConfigManager;
class EventLog;
class MonitorEngine;
class OtaManager;
class AuthManager;
class WifiManagerService;
class PowerMonitor;
class DiscoveryManager;

class WebServerManager {
public:
  void begin(AppConfig* config, RuntimeStatus* status,
             RelayController* relay, ConfigManager* cfgMgr,
             EventLog* eventLog, MonitorEngine* monitor,
             OtaManager* ota,
             AuthManager* auth,
             WifiManagerService* wifi,
             PowerMonitor* power,
             DiscoveryManager* discovery);
  void loop();
private:
  AppConfig* config_ = nullptr;
  RuntimeStatus* status_ = nullptr;
};
