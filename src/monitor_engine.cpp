#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266Ping.h>
#include "monitor_engine.h"
#include "relay_controller.h"
#include "notification_manager.h"
#include "event_log.h"

void MonitorEngine::begin(AppConfig* config, RuntimeStatus* status,
                          RelayController* relay, NotificationManager* notifier,
                          EventLog* eventLog) {
  config_ = config;
  status_ = status;
  relay_ = relay;
  notifier_ = notifier;
  eventLog_ = eventLog;
}

void MonitorEngine::loop() {
  if (!config_ || !status_ || !relay_) return;

  const uint32_t now = millis();

  if (powerCycleActive_) {
    if (!relayPowerOffIssued_) {
      relay_->set(false);
      relayPowerOffIssued_ = true;
      powerOffStartMs_ = now;
      status_->relayOn = false;
      eventLog_->add("recovery", "Relay turned off for power cycle");
    } else if (now - powerOffStartMs_ >= (config_->currentMode == DeviceMode::InternetWatchdog ? config_->internet.powerOffSeconds : config_->device.powerOffSeconds) * 1000UL) {
      relay_->set(true);
      status_->relayOn = true;
      status_->inHoldoff = true;
      holdoffStartMs_ = now;
      powerCycleActive_ = false;
      relayPowerOffIssued_ = false;
      eventLog_->add("recovery", "Relay turned back on, entering holdoff");
    }
    return;
  }

  if (status_->inHoldoff) {
    const uint32_t holdoff = config_->currentMode == DeviceMode::InternetWatchdog ?
      config_->internet.postRebootHoldoffSeconds : config_->device.postRebootHoldoffSeconds;
    if (now - holdoffStartMs_ < holdoff * 1000UL) {
      status_->holdoffRemainingSeconds = holdoff - ((now - holdoffStartMs_) / 1000UL);
      return;
    }
    status_->inHoldoff = false;
    status_->holdoffRemainingSeconds = 0;
    failureStartMs_ = 0;
  }

  if (now - lastMonitorMs_ < config_->monitorIntervalSeconds * 1000UL) return;
  lastMonitorMs_ = now;

  switch (config_->currentMode) {
    case DeviceMode::SmartPlug:
      runSmartPlugMode();
      break;
    case DeviceMode::InternetWatchdog:
      runInternetWatchdogMode();
      break;
    case DeviceMode::DeviceWatchdog:
      runDeviceWatchdogMode();
      break;
  }
}

void MonitorEngine::resetIncident() {
  failureStartMs_ = 0;
  status_->currentIncidentCycles = 0;
}

bool MonitorEngine::checkInternetTargets() {
  bool anySuccess = false;
  for (const auto& target : config_->internet.targets) {
    IPAddress ip;
    if (ip.fromString(target)) {
      if (Ping.ping(ip, 1)) anySuccess = true;
    } else {
      IPAddress resolved;
      if (WiFi.hostByName(target.c_str(), resolved) && Ping.ping(resolved, 1)) anySuccess = true;
    }
    if (anySuccess) break;
  }
  return anySuccess;
}

bool MonitorEngine::checkSingleTarget(const String& target) {
  IPAddress ip;
  if (ip.fromString(target)) return Ping.ping(ip, 1);
  IPAddress resolved;
  if (WiFi.hostByName(target.c_str(), resolved)) return Ping.ping(resolved, 1);
  return false;
}

void MonitorEngine::runSmartPlugMode() {
  status_->healthState = HealthState::Healthy;
}

void MonitorEngine::runInternetWatchdogMode() {
  const bool ok = checkInternetTargets();
  if (ok) {
    status_->healthState = HealthState::Healthy;
    failureStartMs_ = 0;
    return;
  }

  status_->healthState = HealthState::Failed;
  if (failureStartMs_ == 0) failureStartMs_ = millis();

  if (millis() - failureStartMs_ >= config_->internet.failureThresholdSeconds * 1000UL) {
    triggerPowerCycle(config_->internet.powerOffSeconds, config_->internet.postRebootHoldoffSeconds, "all_targets_failed");
  }
}

void MonitorEngine::runDeviceWatchdogMode() {
  if (config_->device.target.isEmpty()) return;
  const bool ok = checkSingleTarget(config_->device.target);
  if (ok) {
    status_->healthState = HealthState::Healthy;
    failureStartMs_ = 0;
    return;
  }

  status_->healthState = HealthState::Failed;
  if (failureStartMs_ == 0) failureStartMs_ = millis();

  if (millis() - failureStartMs_ >= config_->device.failureThresholdSeconds * 1000UL) {
    triggerPowerCycle(config_->device.powerOffSeconds, config_->device.postRebootHoldoffSeconds, "device_target_failed");
  }
}

void MonitorEngine::triggerPowerCycle(uint32_t powerOffSeconds, uint32_t holdoffSeconds, const String& reason) {
  (void)powerOffSeconds;
  (void)holdoffSeconds;
  if (powerCycleActive_ || status_->inHoldoff) return;
  powerCycleActive_ = true;
  status_->currentIncidentCycles++;
  eventLog_->add("trigger", reason);
  notifier_->send("watchdog_trigger", reason, "{}");
  failureStartMs_ = 0;
}

