#pragma once

#include "types.h"

struct BootHealthSnapshot {
  uint8_t consecutiveUnhealthyBoots = 0;
  bool previousBootIncomplete = false;
  bool autoRecoveryTriggered = false;
};

class ConfigManager {
public:
  bool begin();
  bool load(AppConfig& out);
  bool save(const AppConfig& config);
  bool reset();
  bool restoreLastKnownGood(AppConfig& out);
  BootHealthSnapshot beginBootSession();
  bool markBootHealthy();
  bool requestRecoveryBoot();
  bool consumeRecoveryBootRequest();
private:
  const char* configPath_ = "/config.json";
};

