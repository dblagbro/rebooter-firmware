#pragma once

#include "types.h"

struct RuntimeStatus {
  bool wifiConnected = false;
  bool relayOn = true;
  bool inCaptivePortal = false;
  bool recoveryMode = false;
  bool autoRecoveryTriggered = false;
  bool lastKnownGoodRestored = false;
  String setupApName = "";
  bool inHoldoff = false;
  bool inCooldown = false;
  HealthState healthState = HealthState::Unknown;
  String lastEvent = "boot";
  uint32_t uptimeSeconds = 0;
  uint32_t currentIncidentCycles = 0;
  uint32_t currentHourCycles = 0;
  uint32_t holdoffRemainingSeconds = 0;
  uint32_t cooldownRemainingSeconds = 0;
  bool centralEnabled = false;
  bool centralRegistered = false;
  String centralState = "disabled";
  String centralDeviceId = "";
  uint8_t consecutiveUnhealthyBoots = 0;
  // Device uptime-second stamp of the last successful central heartbeat.
  uint32_t centralLastHeartbeatSeconds = 0;
};

