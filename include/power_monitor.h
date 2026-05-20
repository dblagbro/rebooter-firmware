#pragma once

#include <Arduino.h>
#include <SoftwareSerial.h>

#include "app_state.h"

// Number of raw power samples kept in a fixed-size in-RAM ring for
// GET /api/power/recent. Compile-time fixed: never a growing container.
static constexpr uint8_t POWER_RECENT_RING_SIZE = 20;

struct PowerRecentSample {
  uint32_t uptimeSeconds = 0;
  float voltageV = 0.0f;
  float powerW = 0.0f;
  uint32_t currentMa = 0;
  float powerFactor = 1.0f;
  uint32_t energyWh = 0;
  uint8_t sourceFlags = 0;
};

class PowerMonitor {
public:
  void begin(RuntimeStatus* status);
  void loop();

  // Resets the rolling aggregate window; called by the heartbeat after it
  // reads and reports the aggregate so each cycle reports a fresh window.
  void resetAggregate();

  // Serializes the fixed-size raw-sample ring as a JSON array string.
  String recentSamplesJson() const;

  // Zeroes the monotonic energy accumulator (POST /api/power/energy/reset).
  void resetEnergy();

private:
  bool checkByte_() const;
  void parseFrame_();
  uint32_t read24Bit_(uint8_t startIndex) const;
  void updateAggregate_();
  void pushRecentSample_();
  void updateUartContention_();

  RuntimeStatus* status_ = nullptr;
  SoftwareSerial serial_;
  uint8_t rawFrame_[24] = {0};
  uint8_t rawFrameIndex_ = 0;
  uint16_t cfPulsesLast_ = 0;
  uint32_t cfPulsesTotal_ = 0;
  uint32_t lastTransmissionAtMs_ = 0;
  uint32_t beginAtMs_ = 0;
  bool energyInitialized_ = false;
  PowerRecentSample recentRing_[POWER_RECENT_RING_SIZE];
  uint8_t recentCount_ = 0;
  uint8_t recentHead_ = 0;
};
