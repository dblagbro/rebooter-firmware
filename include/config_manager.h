#pragma once

#include "types.h"

struct BootHealthSnapshot {
  uint8_t consecutiveUnhealthyBoots = 0;
  bool previousBootIncomplete = false;
  bool previousBootPlannedRestart = false;
  bool previousBootDifferentFirmware = false;
  bool autoRecoveryTriggered = false;
  String previousPlannedRestartReason = "";
};

class ConfigManager {
public:
  bool begin();
  bool load(AppConfig& out);
  bool save(const AppConfig& config);
  bool reset();
  bool restoreLastKnownGood(AppConfig& out);
  BootHealthSnapshot beginBootSession(const String& currentFirmwareVersion);
  bool markBootHealthy();
  bool prepareForPlannedRestart(const String& reason = "");
  bool requestRecoveryBoot();
  bool consumeRecoveryBootRequest();
private:
  const char* configPath_ = "/config.json";
};

