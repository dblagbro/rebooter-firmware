#pragma once

#include "types.h"
#include "app_state.h"

class RelayController;
class ConfigManager;
class EventLog;
class MonitorEngine;

class WebServerManager {
public:
  void begin(AppConfig* config, RuntimeStatus* status,
             RelayController* relay, ConfigManager* cfgMgr,
             EventLog* eventLog, MonitorEngine* monitor);
  void loop();
private:
  AppConfig* config_ = nullptr;
  RuntimeStatus* status_ = nullptr;
};

