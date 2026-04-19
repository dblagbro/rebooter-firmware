#pragma once

#include "types.h"
#include "app_state.h"

class RelayController;
class NotificationManager;
class EventLog;

class MonitorEngine {
public:
  void begin(AppConfig* config, RuntimeStatus* status,
             RelayController* relay, NotificationManager* notifier,
             EventLog* eventLog);
  void loop();
  void resetIncident();
private:
  bool checkInternetTargets();
  bool checkSingleTarget(const String& target);
  void runSmartPlugMode();
  void runInternetWatchdogMode();
  void runDeviceWatchdogMode();
  void triggerPowerCycle(uint32_t powerOffSeconds, uint32_t holdoffSeconds, const String& reason);
  void enterCooldown(const String& reason);
  bool cycleLimitReached() const;
  uint32_t activeCooldownSeconds() const;
  uint32_t activeMaxCyclesPerIncident() const;
  uint32_t activeMaxCyclesPerHour() const;

  AppConfig* config_ = nullptr;
  RuntimeStatus* status_ = nullptr;
  RelayController* relay_ = nullptr;
  NotificationManager* notifier_ = nullptr;
  EventLog* eventLog_ = nullptr;

  uint32_t bootMs_ = 0;
  uint32_t lastMonitorMs_ = 0;
  uint32_t failureStartMs_ = 0;
  uint32_t holdoffStartMs_ = 0;
  uint32_t cooldownStartMs_ = 0;
  uint32_t powerOffStartMs_ = 0;
  uint32_t hourWindowStartMs_ = 0;
  bool powerCycleActive_ = false;
  bool relayPowerOffIssued_ = false;
};