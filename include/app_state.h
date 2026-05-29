#pragma once

#include "types.h"

enum PowerSampleSourceFlags : uint8_t {
  POWER_SAMPLE_FLAG_SYNTHETIC = 0x01,
  POWER_SAMPLE_FLAG_REAL = 0x02,
  POWER_SAMPLE_FLAG_VOLTAGE_VALID = 0x04,
  POWER_SAMPLE_FLAG_CURRENT_VALID = 0x08,
  POWER_SAMPLE_FLAG_POWER_VALID = 0x10,
  POWER_SAMPLE_FLAG_FREQUENCY_VALID = 0x20,
  POWER_SAMPLE_FLAG_ENERGY_VALID = 0x40,
  POWER_SAMPLE_FLAG_CURRENT_ESTIMATED = 0x80,
};

// Fixed-size rolling power aggregate. Held in RAM only, never grows; the
// heartbeat reads it and resets the window. This is the bounded replacement
// for the standalone raw-sample HTTPS upload that crashes low-heap units.
struct PowerAggregate {
  bool hasData = false;
  float minW = 0.0f;
  float maxW = 0.0f;
  float sumW = 0.0f;
  uint32_t sampleCount = 0;
  float lastV = 0.0f;
  float lastA = 0.0f;
  float lastPF = 1.0f;
  uint32_t energyWh = 0;
  uint32_t windowStartUptimeSeconds = 0;
};

struct PowerLiveStatus {
  bool chipSeen = false;
  bool realSample = false;
  bool voltageValid = false;
  bool currentValid = false;
  bool powerValid = false;
  bool frequencyValid = false;
  bool energyValid = false;
  bool currentEstimated = false;
  // True when the CSE7766 frame stream looks contended by the USB-serial
  // debug header sharing GPIO3 (no chip seen + sustained invalid frames).
  bool uartContended = false;
  uint8_t sourceFlags = 0;
  uint32_t lastSampleUptimeSeconds = 0;
  uint32_t lastSampleMillis = 0;
  uint64_t lastSampleUnixMs = 0;
  uint32_t validFrameCount = 0;
  uint32_t invalidFrameCount = 0;
  float voltageV = 0.0f;
  uint32_t currentMa = 0;
  uint32_t estimatedCurrentMa = 0;
  float powerW = 0.0f;
  float apparentPowerVa = 0.0f;
  float powerFactor = 1.0f;
  float frequencyHz = 0.0f;
  uint32_t energyWh = 0;
  PowerAggregate aggregate;
};

struct RuntimeStatus {
  bool wifiConnected = false;
  bool relayOn = true;
  bool inCaptivePortal = false;
  bool recoveryMode = false;
  bool autoRecoveryTriggered = false;
  bool lastKnownGoodRestored = false;
  bool previousBootDifferentFirmware = false;
  String setupApName = "";
  String resetReason = "";
  String lastPlannedRestartReason = "";
  // 0.2.8 (#154): latest opt-in periodic nearby-network scan, mirrored from
  // WifiManagerService so the heartbeat builder can read it. Empty unless
  // wifi.periodicScanEnabled. Compact "[{ssid,rssi}]" + the uptime it was taken.
  String wifiScanSummary = "";
  uint32_t wifiScanUptimeSeconds = 0;
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
  bool bootHealthyMarked = false;
  bool lastCrashPresent = false;
  String lastCrashReason = "";
  bool timeSynced = false;
  uint64_t wallClockUnixMs = 0;
  // Device uptime-second stamp of the last successful central heartbeat.
  uint32_t centralLastHeartbeatSeconds = 0;
  PowerLiveStatus power;
};

