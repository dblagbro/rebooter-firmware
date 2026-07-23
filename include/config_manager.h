#pragma once

#include "types.h"

struct BootHealthSnapshot {
  uint8_t consecutiveUnhealthyBoots = 0;
  bool previousBootIncomplete = false;
  bool previousBootPlannedRestart = false;
  bool previousBootDifferentFirmware = false;
  bool autoRecoveryTriggered = false;
  String previousPlannedRestartReason = "";
  // 0.2.44 BUG-088: unix-time (SECONDS since epoch) of the most recent
  // proactive-restart fire. Survives every reset type — Power On,
  // Exception, WDT — so the 4h burst-suppressor in
  // central_client_heap.cpp can hold across non-planned reboots.
  // Zero when never fired OR when the fire happened before wall-clock
  // was ever synced. Populated from `/bootstate.json` in
  // beginBootSession(); the heap client checks it against a fresh
  // wall-clock read at cooldown-decision time.
  uint32_t lastProactiveFireUnixSeconds = 0;
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
  // 0.2.44 BUG-088: `proactiveFireUnixSeconds` is only used when
  // `reason == "heap_pressure_proactive"` — the heap client passes
  // its current SNTP-synced wall-clock reading so the burst-suppressor
  // check on the NEXT boot can measure real elapsed time regardless
  // of intervening Exception / Power On / WDT resets. Pass 0 (default)
  // to leave the persistent field unchanged; other planned-restart
  // callers (OTA, factory-reset, hub-issued device_restart) don't
  // need to touch it.
  bool prepareForPlannedRestart(const String& reason = "",
                                uint32_t proactiveFireUnixSeconds = 0);
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

