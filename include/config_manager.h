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
  bool clearPlannedRestart();
  // 0.2.37 BUG-082: RAII guard. Stage a planned-restart reason at
  // scope entry; auto-clear it at scope exit UNLESS commit() was
  // called explicitly to "arm for restart." Catches the call sites
  // (factory-reset endpoint, hub-issued device_restart) that staged
  // a reason then returned without an ESP.restart() — the stale
  // reason got attributed to the next ACTUAL reset (brown-out,
  // hardware watchdog, etc) and corrupted the reboot-classifier
  // telemetry.
  class PlannedRestartGuard {
  public:
    PlannedRestartGuard(ConfigManager& mgr, const String& reason)
        : mgr_(mgr), armed_(true) { mgr.prepareForPlannedRestart(reason); }
    ~PlannedRestartGuard() { if (armed_) mgr_.clearPlannedRestart(); }
    void commit() { armed_ = false; }  // call before ESP.restart()
    PlannedRestartGuard(const PlannedRestartGuard&) = delete;
    PlannedRestartGuard& operator=(const PlannedRestartGuard&) = delete;
  private:
    ConfigManager& mgr_;
    bool armed_;
  };
  bool requestRecoveryBoot();
  bool consumeRecoveryBootRequest();
private:
  const char* configPath_ = "/config.json";
};

