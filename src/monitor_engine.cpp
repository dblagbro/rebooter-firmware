#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266Ping.h>
#include "monitor_engine.h"
#include "relay_controller.h"
#include "notification_manager.h"
#include "event_log.h"

static bool elapsed(uint32_t now, uint32_t startedAt, uint32_t seconds) {
  return now - startedAt >= seconds * 1000UL;
}

void MonitorEngine::begin(AppConfig* config, RuntimeStatus* status,
                          RelayController* relay, NotificationManager* notifier,
                          EventLog* eventLog) {
  config_ = config;
  status_ = status;
  relay_ = relay;
  notifier_ = notifier;
  eventLog_ = eventLog;
  bootMs_ = millis();
  hourWindowStartMs_ = bootMs_;
}

void MonitorEngine::loop() {
  if (!config_ || !status_ || !relay_) return;

  if (status_->recoveryMode) {
    failureStartMs_ = 0;
    powerCycleActive_ = false;
    relayPowerOffIssued_ = false;
    status_->inHoldoff = false;
    status_->holdoffRemainingSeconds = 0;
    status_->inCooldown = false;
    status_->cooldownRemainingSeconds = 0;
    status_->healthState = HealthState::Unknown;
    return;
  }

  const uint32_t now = millis();

  if (now - hourWindowStartMs_ >= 3600UL * 1000UL) {
    hourWindowStartMs_ = now;
    status_->currentHourCycles = 0;
  }

  if (status_->inCooldown) {
    const uint32_t cooldown = activeCooldownSeconds();
    status_->healthState = HealthState::Cooldown;
    if (!elapsed(now, cooldownStartMs_, cooldown)) {
      status_->cooldownRemainingSeconds = cooldown - ((now - cooldownStartMs_) / 1000UL);
      return;
    }
    status_->inCooldown = false;
    status_->cooldownRemainingSeconds = 0;
    resetIncident();
    eventLog_->add("cooldown", "Cooldown expired, monitoring resumed");
  }

  if (powerCycleActive_) {
    if (!relayPowerOffIssued_) {
      relay_->set(false);
      relayPowerOffIssued_ = true;
      powerOffStartMs_ = now;
      status_->relayOn = false;
      eventLog_->add("recovery", "Relay turned off for power cycle");
    } else if (elapsed(now, powerOffStartMs_, config_->currentMode == DeviceMode::InternetWatchdog ? config_->internet.powerOffSeconds : config_->device.powerOffSeconds)) {
      relay_->set(true);
      status_->relayOn = true;
      status_->inHoldoff = true;
      holdoffStartMs_ = now;
      powerCycleActive_ = false;
      relayPowerOffIssued_ = false;
      eventLog_->add("recovery", "Relay turned back on, entering holdoff");
      notifier_->send("relay_power_restored", "power_cycle_complete", "{}");
    }
    return;
  }

  if (status_->inHoldoff) {
    const uint32_t holdoff = config_->currentMode == DeviceMode::InternetWatchdog ?
      config_->internet.postRebootHoldoffSeconds : config_->device.postRebootHoldoffSeconds;
    status_->healthState = HealthState::Holdoff;
    if (!elapsed(now, holdoffStartMs_, holdoff)) {
      status_->holdoffRemainingSeconds = holdoff - ((now - holdoffStartMs_) / 1000UL);
      return;
    }
    status_->inHoldoff = false;
    status_->holdoffRemainingSeconds = 0;
    failureStartMs_ = 0;
  }

  if (!elapsed(now, bootMs_, config_->bootWarmupSeconds)) return;
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
  failureStartMs_ = 0;
}

void MonitorEngine::runInternetWatchdogMode() {
  if (WiFi.status() != WL_CONNECTED) {
    status_->healthState = HealthState::PartialFailure;
    eventLog_->add("network", "Wi-Fi disconnected, watchdog checks paused");
    failureStartMs_ = 0;
    return;
  }

  const bool ok = checkInternetTargets();
  if (ok) {
    status_->healthState = HealthState::Healthy;
    if (status_->currentIncidentCycles > 0) {
      eventLog_->add("incident", "Internet watchdog recovered");
      notifier_->send("incident_resolved", "internet_targets_recovered", "{}");
    }
    resetIncident();
    return;
  }

  status_->healthState = HealthState::Failed;
  if (failureStartMs_ == 0) failureStartMs_ = millis();

  if (millis() - failureStartMs_ >= config_->internet.failureThresholdSeconds * 1000UL) {
    triggerPowerCycle(config_->internet.powerOffSeconds, config_->internet.postRebootHoldoffSeconds, "all_targets_failed");
  }
}

void MonitorEngine::runDeviceWatchdogMode() {
  if (WiFi.status() != WL_CONNECTED) {
    status_->healthState = HealthState::PartialFailure;
    eventLog_->add("network", "Wi-Fi disconnected, device watchdog checks paused");
    failureStartMs_ = 0;
    return;
  }

  if (config_->device.target.isEmpty()) {
    status_->healthState = HealthState::Unknown;
    return;
  }

  const bool ok = checkSingleTarget(config_->device.target);
  if (ok) {
    status_->healthState = HealthState::Healthy;
    if (status_->currentIncidentCycles > 0) {
      eventLog_->add("incident", "Device watchdog target recovered");
      notifier_->send("incident_resolved", "device_target_recovered", "{}");
    }
    resetIncident();
    return;
  }

  status_->healthState = HealthState::Failed;
  if (failureStartMs_ == 0) failureStartMs_ = millis();

  if (millis() - failureStartMs_ >= config_->device.failureThresholdSeconds * 1000UL) {
    triggerPowerCycle(config_->device.powerOffSeconds, config_->device.postRebootHoldoffSeconds, "device_target_failed");
  }
}

bool MonitorEngine::cycleLimitReached() const {
  return status_->currentIncidentCycles >= activeMaxCyclesPerIncident() ||
         status_->currentHourCycles >= activeMaxCyclesPerHour();
}

uint32_t MonitorEngine::activeCooldownSeconds() const {
  return config_->currentMode == DeviceMode::InternetWatchdog ? config_->internet.cooldownSeconds : config_->device.cooldownSeconds;
}

uint32_t MonitorEngine::activeMaxCyclesPerIncident() const {
  return config_->currentMode == DeviceMode::InternetWatchdog ? config_->internet.maxCyclesPerIncident : config_->device.maxCyclesPerIncident;
}

uint32_t MonitorEngine::activeMaxCyclesPerHour() const {
  return config_->currentMode == DeviceMode::InternetWatchdog ? config_->internet.maxCyclesPerHour : config_->device.maxCyclesPerHour;
}

void MonitorEngine::enterCooldown(const String& reason) {
  status_->inCooldown = true;
  cooldownStartMs_ = millis();
  status_->cooldownRemainingSeconds = activeCooldownSeconds();
  status_->healthState = HealthState::Cooldown;
  eventLog_->add("cooldown", reason);
  notifier_->send("max_cycles_reached", reason, "{}");
}

void MonitorEngine::triggerPowerCycle(uint32_t powerOffSeconds, uint32_t holdoffSeconds, const String& reason) {
  (void)powerOffSeconds;
  (void)holdoffSeconds;
  if (powerCycleActive_ || status_->inHoldoff || status_->inCooldown) return;

  if (cycleLimitReached()) {
    enterCooldown("cycle_limit_reached_before_power_cycle");
    failureStartMs_ = 0;
    return;
  }

  powerCycleActive_ = true;
  status_->currentIncidentCycles++;
  status_->currentHourCycles++;
  eventLog_->add("trigger", reason);
  notifier_->send("watchdog_trigger", reason, "{}");
  failureStartMs_ = 0;
}
